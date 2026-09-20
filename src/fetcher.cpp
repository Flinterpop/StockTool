#include "fetcher.h"

#include "quote_parser.h"

#include <process.h>

#include <cassert>

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
    assert(cfg.stockCount > 0 && cfg.stockCount <= kMaxStocks);
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
    stop_      = false;

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
        if (job.kind == JobKind::Quote)       { ProcessQuote(job); }
        else if (job.kind == JobKind::Search) { ProcessSearch(job); }
        else                                  { Process(job); }
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

void Fetcher::UpdateConfig(const Config& cfg) {
    assert(cfg.stockCount <= kMaxStocks);
    cfg_ = cfg;
    std::lock_guard<std::mutex> lock(qMutex_);
    qCount_ = 0;  // drop pending jobs; their indices may have shifted
}

bool Fetcher::Enqueue(JobKind kind, size_t stock, size_t range) {
    assert(thread_ != nullptr);
    assert(kind == JobKind::Summary || kind == JobKind::Chart || kind == JobKind::Inset);
    assert(stock < cfg_.stockCount);
    assert(range < kRanges.size());
    if (stock >= cfg_.stockCount || range >= kRanges.size() ||
        (kind != JobKind::Summary && kind != JobKind::Chart && kind != JobKind::Inset)) { return false; }
    {
        std::lock_guard<std::mutex> lock(qMutex_);
        for (size_t k = 0; k < qCount_; ++k) {
            const FetchJob& q = queue_[(qHead_ + k) % kMaxJobs];
            if (q.kind == kind && q.stock == stock && q.range == range) {
                return true;  // already pending
            }
        }
        if (qCount_ >= kMaxJobs) { return false; }
        const RangeSpec& spec = (kind == JobKind::Summary) ? kSummarySpec : kRanges[range];
        FetchJob& slot = queue_[(qHead_ + qCount_) % kMaxJobs];
        slot.kind   = kind;
        slot.stock  = stock;
        slot.range  = range;
        slot.symbol = cfg_.stocks[stock].symbol;
        slot.url    = BuildUrl(slot.symbol, spec);
        ++qCount_;
    }
    qCv_.notify_one();
    return true;
}

bool Fetcher::EnqueueQuote() {
    assert(thread_ != nullptr);
    if (cfg_.quoteUrlTemplate.empty() || cfg_.stockCount == 0) { return false; }
    std::wstring symbols;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (i > 0) { symbols += L","; }
        symbols += UrlEncode(cfg_.stocks[i].symbol);
    }
    std::wstring url = cfg_.quoteUrlTemplate;
    ReplaceAll(url, L"{symbols}", symbols);  // {crumb} is filled in by the worker
    {
        std::lock_guard<std::mutex> lock(qMutex_);
        for (size_t k = 0; k < qCount_; ++k) {
            if (queue_[(qHead_ + k) % kMaxJobs].kind == JobKind::Quote) { return true; }
        }
        if (qCount_ >= kMaxJobs) { return false; }
        FetchJob& slot = queue_[(qHead_ + qCount_) % kMaxJobs];
        slot.kind   = JobKind::Quote;
        slot.stock  = 0;
        slot.range  = 0;
        slot.symbol = L"*";
        slot.url    = url;
        ++qCount_;
    }
    qCv_.notify_one();
    return true;
}

bool Fetcher::EnqueueSearch(const std::wstring& query, HWND notify) {
    assert(thread_ != nullptr);
    assert(notify != nullptr);
    if (cfg_.searchUrlTemplate.empty() || query.empty() || query.size() > 64) { return false; }
    std::wstring url = cfg_.searchUrlTemplate;
    ReplaceAll(url, L"{query}", UrlEncode(query));
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
        if (qCount_ >= kMaxJobs) { return false; }
        // Insert at the head so the user is not waiting behind refreshes.
        qHead_ = (qHead_ + kMaxJobs - 1) % kMaxJobs;
        FetchJob& slot = queue_[qHead_];
        slot.kind   = JobKind::Search;
        slot.stock  = 0;
        slot.range  = 0;
        slot.symbol = query;
        slot.url    = url;
        slot.notify = notify;
        ++qCount_;
    }
    qCv_.notify_one();
    return true;
}

std::wstring Fetcher::BuildUrl(const std::wstring& symbol, const RangeSpec& spec) const {
    assert(!symbol.empty());
    std::wstring url = cfg_.urlTemplate;
    ReplaceAll(url, L"{symbol}",   UrlEncode(symbol));
    ReplaceAll(url, L"{range}",    spec.range);
    ReplaceAll(url, L"{interval}", spec.interval);
    return url;
}

bool Fetcher::Fetch(const std::wstring& url, QuoteData& out) {
    assert(!url.empty());
    HttpResult   res;
    std::wstring err;
    out = QuoteData{};
    if (!http_.Get(url, buf_.get(), kHttpBufSize, res, err)) {
        out.error = err;
        return false;
    }
    // Non-200 replies usually still carry a JSON error description.
    if (!ParseChartJson(buf_.get(), res.length, out, err)) {
        out.error = (res.status == 200) ? err : L"HTTP " + std::to_wstring(res.status) + L": " + err;
        return false;
    }
    assert(out.valid);
    return true;
}

void Fetcher::Process(const FetchJob& job) {
    assert(job.stock < kMaxStocks);
    assert(job.range < kRanges.size());
    assert(!job.symbol.empty() && !job.url.empty());
    const bool ok = Fetch(job.url, *scratch_);
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
        case JobKind::Quote:
        case JobKind::Search:
            assert(false);  // handled by ProcessQuote / ProcessSearch
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
    if (!http_.Get(kCrumbUrl, buf_.get(), kHttpBufSize, res, err)) { return false; }
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
        if (!http_.Get(url, buf_.get(), kHttpBufSize, res, err)) { break; }
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
    bool ok = http_.Get(job.url, buf_.get(), kHttpBufSize, res, err);
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

} // namespace st
