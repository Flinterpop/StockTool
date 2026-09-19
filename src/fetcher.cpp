#include "fetcher.h"

#include "http.h"
#include "quote_parser.h"

#include <process.h>

#include <cassert>

namespace st {
namespace {

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
    chart_     = std::make_unique<QuoteData>();
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
        Process(job);
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
    assert(stock < cfg_.stockCount);
    assert(range < kRanges.size());
    if (stock >= cfg_.stockCount || range >= kRanges.size()) { return false; }
    {
        std::lock_guard<std::mutex> lock(qMutex_);
        for (size_t k = 0; k < qCount_; ++k) {
            const FetchJob& q = queue_[(qHead_ + k) % kMaxJobs];
            if (q.kind == kind && q.stock == stock && q.range == range) {
                return true;  // already pending
            }
        }
        if (qCount_ >= kMaxJobs) { return false; }
        const RangeSpec& spec = (kind == JobKind::Chart) ? kRanges[range] : kSummarySpec;
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

std::wstring Fetcher::BuildUrl(const std::wstring& symbol, const RangeSpec& spec) const {
    assert(!symbol.empty());
    std::wstring url = cfg_.urlTemplate;
    ReplaceAll(url, L"{symbol}",   symbol);
    ReplaceAll(url, L"{range}",    spec.range);
    ReplaceAll(url, L"{interval}", spec.interval);
    return url;
}

bool Fetcher::Fetch(const std::wstring& url, QuoteData& out) {
    assert(!url.empty());
    HttpResult   res;
    std::wstring err;
    out = QuoteData{};
    if (!HttpGetUrl(url, buf_.get(), kHttpBufSize, res, err)) {
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
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (job.kind == JobKind::Summary) {
            (*summaries_)[job.stock] = *scratch_;
        } else {
            *chart_     = *scratch_;
            chartStock_ = job.stock;
            chartRange_ = job.range;
            chartValid_ = true;
        }
    }
    const UINT msg = (job.kind == JobKind::Summary) ? WM_APP_SUMMARY_READY : WM_APP_CHART_READY;
    const BOOL posted = PostMessageW(notify_, msg, static_cast<WPARAM>(job.stock),
                                     static_cast<LPARAM>(job.range));
    assert(posted);
    (void)posted;
}

bool Fetcher::CopySummary(size_t stock, QuoteData& out) {
    assert(stock < kMaxStocks);
    if (stock >= kMaxStocks || !summaries_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    out = (*summaries_)[stock];
    return true;
}

bool Fetcher::CopyChart(size_t stock, size_t range, QuoteData& out) {
    if (!chart_) { return false; }
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (!chartValid_ || chartStock_ != stock || chartRange_ != range) { return false; }
    out = *chart_;
    return true;
}

} // namespace st
