#include "fetcher.h"

#include "quote_parser.h"
#include "textfmt.h"

#include <nlohmann/json.hpp>

#include <process.h>

#include <cassert>
#include <ctime>

namespace st {
namespace {

// Yahoo's fundamentals endpoint wants a session cookie plus a "crumb" tied
// to it. These two URLs establish them; both are provider-specific.
constexpr wchar_t kCookieUrl[] = L"https://fc.yahoo.com/";
constexpr wchar_t kCrumbUrl[]  = L"https://query1.finance.yahoo.com/v1/test/getcrumb";

// Replaces every occurrence of `from` with `to` (bounded).
void ReplaceAll(std::wstring& s, const wchar_t* from, const std::wstring& to) {
    assert(from != nullptr && from[0] != L'\0');
    const size_t fromLen = wcslen(from);
    size_t pos = 0;
    for (int guard = 0; guard < 16; ++guard) {
        pos = s.find(from, pos);
        if (pos == std::wstring::npos) { return; }
        s.replace(pos, fromLen, to);
        pos += to.size();
    }
}

} // namespace

Fetcher::~Fetcher() { Stop(); }

bool Fetcher::Start(HWND notify, const Config& cfg, std::wstring& err) {
    assert(notify != nullptr);
    assert(cfg.stockCount <= kMaxStocks);
    if (thread_ != nullptr) {
        err = L"Fetcher already started";
        return false;
    }
    notify_    = notify;
    cfg_       = cfg;
    buf_       = std::make_unique<char[]>(kHttpBufSize);
    scratch_   = std::make_unique<QuoteData>();
    summaries_ = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    charts_    = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    insets_    = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    quotes_    = std::make_unique<std::array<QuoteStats, kMaxStocks>>();
    search_    = std::make_unique<std::array<SearchHit, kMaxSearchHits>>();
    news_      = std::make_unique<std::array<NewsSlot, kMaxStocks>>();
    bench_     = std::make_unique<QuoteData>();
    stop_      = false;
    static constexpr const wchar_t* kNames[kEndpointCount] = { L"chart", L"fundamentals", L"search", L"fx", L"news", L"crumb", L"tmx-fallback" };
    for (size_t i = 0; i < kEndpointCount; ++i) { health_.endpoints[i].name = kNames[i]; }

    unsigned id = 0;
    const uintptr_t h = _beginthreadex(nullptr, 0, &Fetcher::ThreadEntry, this, 0, &id);
    if (h == 0) {
        err = L"Could not start worker thread";
        return false;
    }
    thread_ = reinterpret_cast<HANDLE>(h);
    return true;
}

void Fetcher::Stop() {
    if (thread_ == nullptr) { return; }
    {
        std::lock_guard<std::mutex> lock(qMutex_);
        stop_ = true;
    }
    qCv_.notify_all();
    // HTTP timeouts bound every job, so the worker always returns promptly.
    const DWORD w = WaitForSingleObject(thread_, 60000);
    assert(w == WAIT_OBJECT_0);
    (void)w;
    CloseHandle(thread_);
    thread_ = nullptr;
}

unsigned __stdcall Fetcher::ThreadEntry(void* arg) {
    assert(arg != nullptr);
    static_cast<Fetcher*>(arg)->Run();
    return 0;
}

// Service loop: exits when Stop() raises stop_. Each iteration is bounded by
// the HTTP timeouts in http.cpp.
void Fetcher::Run() {
    for (;;) {
        FetchJob job;
        if (!Pop(job)) { return; }
        switch (job.kind) {
        case JobKind::Quote:  ProcessQuote(job); break;
        case JobKind::Search: ProcessSearch(job); break;
        case JobKind::Fx:     ProcessFx(job); break;
        case JobKind::News:   ProcessNews(job); break;
        case JobKind::Bench:  ProcessBench(job); break;
        default:              Process(job); break;
        }
    }
}

bool Fetcher::Pop(FetchJob& job) {
    std::unique_lock<std::mutex> lock(qMutex_);
    qCv_.wait(lock, [this] { return stop_ || qCount_ > 0; });
    if (stop_) { return false; }
    assert(qCount_ > 0);
    job    = queue_[qHead_];
    qHead_ = (qHead_ + 1) % kMaxJobs;
    --qCount_;
    return true;
}

// Adds a job unless an identical one is pending. `front` makes it next.
bool Fetcher::Push(const FetchJob& job, bool front) {
    {
        std::lock_guard<std::mutex> lock(qMutex_);
        for (size_t k = 0; k < qCount_; ++k) {
            const FetchJob& q = queue_[(qHead_ + k) % kMaxJobs];
            if (q.kind == job.kind && q.stock == job.stock && q.range == job.range && q.aux == job.aux) {
                return true;  // already pending
            }
        }
        if (qCount_ >= kMaxJobs) { return false; }
        if (front) {
            qHead_ = (qHead_ + kMaxJobs - 1) % kMaxJobs;
            queue_[qHead_] = job;
        } else {
            queue_[(qHead_ + qCount_) % kMaxJobs] = job;
        }
        ++qCount_;
    }
    qCv_.notify_one();
    return true;
}

void Fetcher::UpdateConfig(const Config& cfg) {
    assert(cfg.stockCount <= kMaxStocks);
    cfg_ = cfg;
    std::lock_guard<std::mutex> lock(qMutex_);
    qCount_ = 0;  // drop pending jobs; their indices may have shifted
}

bool Fetcher::Enqueue(JobKind kind, size_t stock, size_t range) {
    assert(thread_ != nullptr);
    assert(stock < cfg_.stockCount);
    assert(range < kRanges.size());
    const bool kindOk = kind == JobKind::Summary || kind == JobKind::Chart ||
                        kind == JobKind::Inset || kind == JobKind::News;
    if (!kindOk || stock >= cfg_.stockCount || range >= kRanges.size()) { return false; }
    if (kind == JobKind::News && cfg_.newsSource == NewsSource::None) { return false; }
    FetchJob job;
    job.kind   = kind;
    job.stock  = stock;
    job.range  = range;
    job.symbol = cfg_.stocks[stock].symbol;
    if (kind == JobKind::News && cfg_.newsSource == NewsSource::Google) {
        // Search by company name when we have one; a bare symbol like "TD"
        // matches far too much. Quoted so the phrase must appear.
        const StockEntry& e = cfg_.stocks[stock];
        const bool haveName = !e.name.empty() && _wcsicmp(e.name.c_str(), e.symbol.c_str()) != 0;
        const std::wstring query = haveName ? L"\"" + e.name + L"\"" : e.symbol;
        job.url = cfg_.newsRssTemplate;
        job.aux = L"rss";
        ReplaceAll(job.url, L"{query}", UrlEncode(query));
    } else if (kind == JobKind::News) {
        job.url = cfg_.newsUrlTemplate;
        job.aux = L"json";
        ReplaceAll(job.url, L"{symbol}", UrlEncode(job.symbol));
    } else {
        const RangeSpec& spec = (kind == JobKind::Summary) ? kSummarySpec : kRanges[range];
        job.url  = BuildUrl(job.symbol, spec);
        job.spec = &spec;
        if (cfg_.fallback == FallbackProvider::Tmx) { job.fallback = cfg_.tmxUrl; }
    }
    return Push(job, false);
}

bool Fetcher::EnqueueQuote() {
    assert(thread_ != nullptr);
    if (cfg_.quoteUrlTemplate.empty() || cfg_.stockCount == 0) { return false; }
    std::wstring symbols;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (i > 0) { symbols += L","; }
        symbols += UrlEncode(cfg_.stocks[i].symbol);
    }
    FetchJob job;
    job.kind   = JobKind::Quote;
    job.symbol = L"*";
    job.url    = cfg_.quoteUrlTemplate;
    ReplaceAll(job.url, L"{symbols}", symbols);  // {crumb} is filled in by the worker
    return Push(job, false);
}

bool Fetcher::EnqueueSearch(const std::wstring& query, HWND notify) {
    assert(thread_ != nullptr);
    assert(notify != nullptr);
    if (cfg_.searchUrlTemplate.empty() || query.empty() || query.size() > 64) { return false; }
    FetchJob job;
    job.kind   = JobKind::Search;
    job.symbol = query;
    job.url    = cfg_.searchUrlTemplate;
    job.notify = notify;
    ReplaceAll(job.url, L"{query}", UrlEncode(query));
    {
        std::lock_guard<std::mutex> lock(qMutex_);
        // Drop any search still waiting: only the newest query matters.
        size_t kept = 0;
        for (size_t k = 0; k < qCount_; ++k) {
            const size_t from = (qHead_ + k) % kMaxJobs;
            if (queue_[from].kind == JobKind::Search) { continue; }
            const size_t to = (qHead_ + kept) % kMaxJobs;
            if (to != from) { queue_[to] = queue_[from]; }
            ++kept;
        }
        qCount_ = kept;
    }
    return Push(job, true);   // the user is typing: do not wait behind refreshes
}

bool Fetcher::EnqueueFx(const std::wstring& from, const std::wstring& to) {
    assert(thread_ != nullptr);
    if (from.empty() || to.empty() || from == to) { return false; }
    FetchJob job;
    job.kind   = JobKind::Fx;
    job.symbol = from + to + L"=X";
    job.aux    = from + L"|" + to;
    job.url    = BuildUrl(job.symbol, kSummarySpec);
    if (cfg_.fallback != FallbackProvider::None) { job.fallback = L"boc"; }   // Bank of Canada, CAD pairs only
    return Push(job, false);
}

bool Fetcher::EnqueueBench(size_t range) {
    assert(thread_ != nullptr);
    assert(range < kRanges.size());
    if (cfg_.benchmark.empty() || range >= kRanges.size()) { return false; }
    FetchJob job;
    job.kind   = JobKind::Bench;
    job.range  = range;
    job.symbol = cfg_.benchmark;
    job.url    = BuildUrl(job.symbol, kRanges[range]);
    job.spec   = &kRanges[range];
    if (cfg_.fallback == FallbackProvider::Tmx) { job.fallback = cfg_.tmxUrl; }
    return Push(job, false);
}

std::wstring Fetcher::BuildUrl(const std::wstring& symbol, const RangeSpec& spec) const {
    assert(!symbol.empty());
    std::wstring url = cfg_.urlTemplate;
    ReplaceAll(url, L"{symbol}",   UrlEncode(symbol));
    ReplaceAll(url, L"{range}",    spec.range);
    ReplaceAll(url, L"{interval}", spec.interval);
    // Older config files predate dividend events; ask for them anyway.
    if (url.find(L"events=") == std::wstring::npos && url.find(L'?') != std::wstring::npos) {
        url += L"&events=div";
    }
    return url;
}

// Primary provider first; if that fails and the job names a fallback, the
// same bars are asked of TMX Money. A fallback result is valid with
// `source` set and the primary's failure kept in `error` for the UI.
bool Fetcher::Fetch(const FetchJob& job, QuoteData& out) {
    assert(!job.url.empty());
    HttpResult   res;
    std::wstring err;
    out = QuoteData{};
    bool ok = Get(Endpoint::Chart, job.url, res, err);
    if (ok) {
        // Non-200 replies usually still carry a JSON error description.
        ok = ParseChartJson(buf_.get(), res.length, out, err);
        if (!ok) {
            err = (res.status == 200) ? err : L"HTTP " + std::to_wstring(res.status) + L": " + err;
            Record(Endpoint::Chart, false, res, err);
        }
    }
    if (ok) {
        assert(out.valid);
        return true;
    }
    const std::wstring primaryErr = err;
    if (job.fallback.empty() || job.spec == nullptr) {
        out = QuoteData{};
        out.error = primaryErr;
        return false;
    }
    std::wstring tmxErr;
    if (FetchTmx(job, out, tmxErr)) {
        out.error = L"Yahoo: " + primaryErr;
        assert(out.valid && !out.source.empty());
        return true;
    }
    out = QuoteData{};
    out.error = primaryErr + L" | " + tmxErr;
    return false;
}

namespace {

// The GraphQL request TMX Money's own site sends for a chart.
std::string TmxBody(const std::wstring& symbol, const RangeSpec& spec) {
    const std::wstring r = spec.range;
    const char* freq = "day";
    int64_t days = 10;                       // 1d / 5d: enough daily bars for a summary
    if (r == L"1mo")      { days = 35; }
    else if (r == L"6mo") { days = 190; }
    else if (r == L"ytd") { days = -1; }
    else if (r == L"1y")  { days = 370; }
    else if (r == L"5y")  { days = 5 * 366; freq = "week"; }
    else if (r == L"max") { days = 40 * 366; freq = "month"; }
    const int64_t now = static_cast<int64_t>(_time64(nullptr));
    std::wstring start;
    if (days < 0) {
        start = FormatIsoDate(now).substr(0, 4) + L"-01-01";   // year to date
    } else {
        start = FormatIsoDate(now - days * 86400);
    }
    const std::wstring end = FormatIsoDate(now + 86400);   // tomorrow, so today's bar is included
    auto narrow = [](const std::wstring& w) {
        std::string s;
        for (const wchar_t c : w) { s += (c < 0x80) ? static_cast<char>(c) : '?'; }
        return s;
    };
    nlohmann::json body;
    body["operationName"] = "getTimeSeriesData";
    body["variables"] = { { "symbol", narrow(symbol) }, { "freq", freq }, { "interval", 1 },
                         { "start", narrow(start) }, { "end", narrow(end) } };
    body["query"] =
        "query getTimeSeriesData($symbol: String!, $freq: String, $interval: Int, $start: String, $end: String) "
        "{ getTimeSeriesData(symbol: $symbol, freq: $freq, interval: $interval, start: $start, end: $end) "
        "{ dateTime open high low close volume } }";
    return body.dump();
}

} // namespace

bool Fetcher::FetchTmx(const FetchJob& job, QuoteData& out, std::wstring& err) {
    assert(!job.fallback.empty() && job.spec != nullptr);
    const std::wstring tmxSymbol = TmxSymbol(job.symbol);
    if (tmxSymbol.empty()) {
        err = L"TMX: no equivalent symbol";
        return false;
    }
    const std::string body = TmxBody(tmxSymbol, *job.spec);
    HttpResult res;
    const bool sent = http_.Post(job.fallback, L"application/json",
                                 L"locale: en\r\nOrigin: https://money.tmx.com\r\nReferer: https://money.tmx.com/\r\n",
                                 body, buf_.get(), kHttpBufSize, res, err);
    if (!sent) {
        Record(Endpoint::Tmx, false, res, err);
        err = L"TMX: " + err;
        return false;
    }
    if (res.status != 200) {
        err = L"TMX: HTTP " + std::to_wstring(res.status);
        Record(Endpoint::Tmx, false, res, err);
        return false;
    }
    if (!ParseTmxJson(buf_.get(), res.length, out, err)) {
        Record(Endpoint::Tmx, false, res, err);
        return false;
    }
    Record(Endpoint::Tmx, true, res, L"");
    out.meta.currency = (tmxSymbol.size() > 3 && tmxSymbol.compare(tmxSymbol.size() - 3, 3, L":US") == 0) ? L"USD" : L"CAD";
    return true;
}

void Fetcher::Process(const FetchJob& job) {
    assert(job.stock < kMaxStocks);
    assert(job.range < kRanges.size());
    assert(!job.symbol.empty() && !job.url.empty());
    const bool ok = Fetch(job, *scratch_);
    (void)ok;  // failure is reported through QuoteData::error
    scratch_->symbol = job.symbol;
    UINT msg = WM_APP_SUMMARY_READY;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        switch (job.kind) {
        case JobKind::Summary:
            (*summaries_)[job.stock] = *scratch_;
            break;
        case JobKind::Chart:
            (*charts_)[job.stock]  = *scratch_;
            chartRange_[job.stock] = job.range;
            chartValid_[job.stock] = true;
            msg = WM_APP_CHART_READY;
            break;
        case JobKind::Inset:
            (*insets_)[job.stock]  = *scratch_;
            insetRange_[job.stock] = job.range;
            insetValid_[job.stock] = true;
            msg = WM_APP_INSET_READY;
            break;
        default:
            assert(false);  // other kinds have their own Process* functions
            break;
        }
    }
    const BOOL posted = PostMessageW(notify_, msg, static_cast<WPARAM>(job.stock),
                                     static_cast<LPARAM>(job.range));
    assert(posted);
    (void)posted;
}

bool Fetcher::EnsureCrumb(std::wstring& err) {
    if (!crumb_.empty()) { return true; }
    HttpResult res;
    // Any status is fine here; the point is the Set-Cookie the session keeps.
    if (!http_.Get(kCookieUrl, buf_.get(), kHttpBufSize, res, err)) { return false; }
    if (!Get(Endpoint::Crumb, kCrumbUrl, res, err)) { return false; }
    if (res.status != 200 || res.length == 0 || res.length > 64) {
        err = L"crumb request returned HTTP " + std::to_wstring(res.status);
        return false;
    }
    std::wstring crumb;
    for (size_t i = 0; i < res.length; ++i) {
        const char c = buf_[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '"') { continue; }
        crumb += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    if (crumb.empty() || crumb.find(L'<') != std::wstring::npos) {
        err = L"crumb request returned unexpected content";
        return false;
    }
    crumb_ = crumb;
    assert(!crumb_.empty());
    return true;
}

void Fetcher::ProcessQuote(const FetchJob& job) {
    assert(job.kind == JobKind::Quote && !job.url.empty());
    std::wstring err;
    size_t       count = 0;
    bool         ok    = false;
    std::array<QuoteStats, kMaxStocks>& out = *quotes_;
    // One retry with a fresh crumb if the first attempt is rejected.
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (!EnsureCrumb(err)) { break; }
        std::wstring url = job.url;
        ReplaceAll(url, L"{crumb}", UrlEncode(crumb_));
        HttpResult res;
        if (!Get(Endpoint::Quote, url, res, err)) { break; }
        if (res.status == 401 || res.status == 403) {
            crumb_.clear();
            err = L"HTTP " + std::to_wstring(res.status) + L" from fundamentals endpoint";
            continue;
        }
        std::lock_guard<std::mutex> lock(dataMutex_);
        ok = ParseQuoteBatchJson(buf_.get(), res.length, out, count, err);
        if (!ok && res.status != 200) { err = L"HTTP " + std::to_wstring(res.status) + L": " + err; }
    }
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        quoteCount_ = ok ? count : 0;
        quoteError_ = ok ? L"" : err;
    }
    const BOOL posted = PostMessageW(notify_, WM_APP_QUOTE_READY, 0, 0);
    assert(posted);
    (void)posted;
}

void Fetcher::ProcessSearch(const FetchJob& job) {
    assert(job.kind == JobKind::Search && !job.url.empty() && job.notify != nullptr);
    std::wstring err;
    size_t       count = 0;
    HttpResult   res;
    bool ok = Get(Endpoint::Search, job.url, res, err);
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (ok) {
            ok = ParseSearchJson(buf_.get(), res.length, *search_, count, err);
            if (!ok && res.status != 200) { err = L"HTTP " + std::to_wstring(res.status) + L": " + err; }
        }
        searchCount_ = ok ? count : 0;
        searchQuery_ = job.symbol;
        searchError_ = ok ? L"" : err;
    }
    // The dialog may already be gone; a failed post is harmless.
    const BOOL posted = PostMessageW(job.notify, WM_APP_SEARCH_READY, 0, 0);
    (void)posted;
}

void Fetcher::ProcessFx(const FetchJob& job) {
    assert(job.kind == JobKind::Fx && !job.url.empty());
    const size_t bar = job.aux.find(L'|');
    assert(bar != std::wstring::npos);
    if (bar == std::wstring::npos) { return; }
    FxRate r;
    r.from = job.aux.substr(0, bar);
    r.to   = job.aux.substr(bar + 1);
    FetchJob primary = job;
    primary.fallback.clear();            // TMX has no FX pairs; the Bank of Canada is tried below
    const bool ok = Fetch(primary, *scratch_);
    {
        HttpResult none;
        Record(Endpoint::Fx, ok, none, ok ? L"" : scratch_->error);
    }
    if (ok && scratch_->meta.price > 0.0) {
        r.rate  = scratch_->meta.price;
        r.valid = true;
    } else if (ok && scratch_->series.count > 0) {
        r.rate  = scratch_->series.pts[scratch_->series.count - 1].close;
        r.valid = r.rate > 0.0;
    }
    if (!r.valid && !job.fallback.empty()) {
        std::wstring err;
        double rate = 0.0;
        if (FetchBocRate(r.from, r.to, rate, err)) {
            r.rate  = rate;
            r.valid = true;
        }
        HttpResult none;
        Record(Endpoint::Fx, r.valid, none, r.valid ? L"via Bank of Canada (Yahoo: " + scratch_->error + L")" : scratch_->error + L" | " + err);
        if (r.valid) {
            std::lock_guard<std::mutex> lock(dataMutex_);
            health_.endpoints[static_cast<size_t>(Endpoint::Fx)].lastError = L"via Bank of Canada";
        }
    }
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        // Replace an existing entry for this pair, else take a free/oldest slot.
        size_t slot = kMaxFx;
        for (size_t i = 0; i < kMaxFx; ++i) {
            if (fx_[i].from == r.from && fx_[i].to == r.to) { slot = i; break; }
        }
        if (slot == kMaxFx) {
            for (size_t i = 0; i < kMaxFx; ++i) {
                if (fx_[i].from.empty()) { slot = i; break; }
            }
        }
        if (slot == kMaxFx) { slot = 0; }
        fx_[slot] = r;
    }
    const BOOL posted = PostMessageW(notify_, WM_APP_FX_READY, 0, 0);
    assert(posted);
    (void)posted;
}

// Bank of Canada Valet: daily average rate for a CAD pair. Works for
// XXX->CAD directly and CAD->XXX by inversion; anything else is refused.
bool Fetcher::FetchBocRate(const std::wstring& from, const std::wstring& to, double& rate, std::wstring& err) {
    assert(!from.empty() && !to.empty());
    const bool toCad   = to == L"CAD";
    const bool fromCad = from == L"CAD";
    if (toCad == fromCad) {
        err = L"BoC: only CAD pairs";
        return false;
    }
    const std::wstring series = L"FX" + (toCad ? from : to) + L"CAD";
    const std::wstring url = L"https://www.bankofcanada.ca/valet/observations/" + series + L"/json?recent=1";
    HttpResult res;
    if (!http_.Get(url, buf_.get(), kHttpBufSize, res, err)) {
        err = L"BoC: " + err;
        return false;
    }
    if (res.status != 200) {
        err = L"BoC: HTTP " + std::to_wstring(res.status);
        return false;
    }
    const nlohmann::json doc = nlohmann::json::parse(buf_.get(), buf_.get() + res.length, nullptr, false);
    if (doc.is_discarded() || !doc.contains("observations") || !doc["observations"].is_array() || doc["observations"].empty()) {
        err = L"BoC: no observations";
        return false;
    }
    std::string key;
    for (const wchar_t c : series) { key += static_cast<char>(c); }
    const nlohmann::json& obs = doc["observations"].back();
    if (!obs.contains(key) || !obs[key].contains("v") || !obs[key]["v"].is_string()) {
        err = L"BoC: unexpected layout";
        return false;
    }
    const double v = std::strtod(obs[key]["v"].get<std::string>().c_str(), nullptr);
    if (!(v > 0.0)) {
        err = L"BoC: bad rate";
        return false;
    }
    rate = toCad ? v : 1.0 / v;
    assert(rate > 0.0);
    return true;
}

void Fetcher::ProcessNews(const FetchJob& job) {
    assert(job.kind == JobKind::News && !job.url.empty() && job.stock < kMaxStocks);
    std::wstring err;
    size_t       count = 0;
    HttpResult   res;
    bool ok = Get(Endpoint::News, job.url, res, err);
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        NewsSlot& slot = (*news_)[job.stock];
        if (ok) {
            ok = (job.aux == L"rss") ? ParseNewsRss(buf_.get(), res.length, slot.items, count, err)
                                     : ParseNewsJson(buf_.get(), res.length, slot.items, count, err);
            if (!ok && res.status != 200) { err = L"HTTP " + std::to_wstring(res.status) + L": " + err; }
        }
        slot.count  = ok ? count : 0;
        slot.symbol = job.symbol;
        slot.error  = ok ? L"" : err;
        slot.valid  = true;
    }
    const BOOL posted = PostMessageW(notify_, WM_APP_NEWS_READY, static_cast<WPARAM>(job.stock), 0);
    assert(posted);
    (void)posted;
}

void Fetcher::ProcessBench(const FetchJob& job) {
    assert(job.kind == JobKind::Bench && !job.url.empty());
    const bool ok = Fetch(job, *scratch_);
    (void)ok;
    scratch_->symbol = job.symbol;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        *bench_     = *scratch_;
        benchRange_ = job.range;
        benchValid_ = true;
    }
    const BOOL posted = PostMessageW(notify_, WM_APP_BENCH_READY, 0, static_cast<LPARAM>(job.range));
    assert(posted);
    (void)posted;
}

// A GET that honours the provider's rate limiting and keeps the health
// table current. HTTP 429 starts a pause that doubles on repeats (1, 2, 4,
// 8 minutes, capped) and clears on the next success.
bool Fetcher::Get(Endpoint ep, const std::wstring& url, HttpResult& res, std::wstring& err) {
    std::wstring why;
    if (InBackoff(why)) {
        err = why;
        res = HttpResult{};
        return false;
    }
    const bool ok = http_.Get(url, buf_.get(), kHttpBufSize, res, err);
    if (ok && res.status == 429) {
        const int64_t now = static_cast<int64_t>(_time64(nullptr));
        backoffSteps_ = (backoffSteps_ < 3) ? backoffSteps_ + 1 : 3;
        const int64_t seconds = 60 * (1 << (backoffSteps_ - 1));
        err = L"rate limited (HTTP 429); pausing " + std::to_wstring(seconds / 60) + L" min";
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            health_.backoffUntil = now + seconds;
        }
        Record(ep, false, res, err);
        const BOOL posted = PostMessageW(notify_, WM_APP_BACKOFF, static_cast<WPARAM>(seconds), 0);
        (void)posted;
        return false;
    }
    Record(ep, ok && res.status < 400, res, ok ? (res.status < 400 ? L"" : L"HTTP " + std::to_wstring(res.status)) : err);
    if (ok && res.status < 400) { backoffSteps_ = 0; }
    return ok;
}

bool Fetcher::InBackoff(std::wstring& why) {
    const int64_t now = static_cast<int64_t>(_time64(nullptr));
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (health_.backoffUntil > now) {
        why = L"rate limited; retrying in " + std::to_wstring(health_.backoffUntil - now) + L" s";
        return true;
    }
    return false;
}

void Fetcher::Record(Endpoint ep, bool ok, const HttpResult& res, const std::wstring& err) {
    const int64_t now = static_cast<int64_t>(_time64(nullptr));
    std::lock_guard<std::mutex> lock(dataMutex_);
    EndpointHealth& h = health_.endpoints[static_cast<size_t>(ep)];
    h.lastStatus = res.status;
    if (res.elapsedMs > 0) { h.lastMs = res.elapsedMs; }
    if (ok) {
        h.lastOk   = now;
        h.failures = 0;
        h.lastError.clear();
    } else {
        h.lastFail = now;
        ++h.failures;
        h.lastError = err;
    }
}

bool Fetcher::CopyBench(size_t range, QuoteData& out) {
    if (!bench_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (!benchValid_ || benchRange_ != range) { return false; }
    out = *bench_;
    return true;
}

void Fetcher::CopyHealth(Health& out) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    out = health_;
    {
        std::lock_guard<std::mutex> qlock(qMutex_);
        out.pending = qCount_;
    }
}

bool Fetcher::CopySummary(size_t stock, QuoteData& out) {
    assert(stock < kMaxStocks);
    if (stock >= kMaxStocks || !summaries_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    out = (*summaries_)[stock];
    return true;
}

bool Fetcher::CopyChart(size_t stock, size_t range, QuoteData& out) {
    assert(stock < kMaxStocks);
    if (stock >= kMaxStocks || !charts_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (!chartValid_[stock] || chartRange_[stock] != range) { return false; }
    out = (*charts_)[stock];
    return true;
}

bool Fetcher::CopyInset(size_t stock, size_t range, QuoteData& out) {
    assert(stock < kMaxStocks);
    if (stock >= kMaxStocks || !insets_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (!insetValid_[stock] || insetRange_[stock] != range) { return false; }
    out = (*insets_)[stock];
    return true;
}

bool Fetcher::CopyQuotes(std::array<QuoteStats, kMaxStocks>& out, size_t& count, std::wstring& err) {
    if (!quotes_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    out   = *quotes_;
    count = quoteCount_;
    err   = quoteError_;
    return true;
}

bool Fetcher::CopySearch(std::array<SearchHit, kMaxSearchHits>& out, size_t& count,
                         std::wstring& query, std::wstring& err) {
    if (!search_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    out   = *search_;
    count = searchCount_;
    query = searchQuery_;
    err   = searchError_;
    return true;
}

bool Fetcher::CopyFx(std::array<FxRate, kMaxFx>& out) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    out = fx_;
    return true;
}

bool Fetcher::CopyNews(size_t stock, std::array<NewsItem, kMaxNews>& out, size_t& count,
                       std::wstring& symbol, std::wstring& err) {
    assert(stock < kMaxStocks);
    if (stock >= kMaxStocks || !news_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    const NewsSlot& slot = (*news_)[stock];
    if (!slot.valid) { return false; }
    out    = slot.items;
    count  = slot.count;
    symbol = slot.symbol;
    err    = slot.error;
    return true;
}

} // namespace st
