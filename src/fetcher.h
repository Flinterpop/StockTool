// Background worker that fetches quotes and hands results to the UI thread.
#pragma once

#include "common.h"
#include "config.h"
#include "http.h"

#include <windows.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace st {

enum class JobKind : uint8_t { Summary, Chart, Inset, Quote, Search, Fx, News, Bench };

// Endpoint slots for the health panel.
enum class Endpoint : uint8_t { Chart, Quote, Search, Fx, News, Crumb, Tmx };

struct FetchJob {
    JobKind      kind   = JobKind::Summary;
    size_t       stock  = 0;   // index into Config::stocks at enqueue time
    size_t       range  = 0;   // index into kRanges (Chart/Inset)
    std::wstring symbol;       // captured at enqueue time; the worker never reads Config
    std::wstring url;          // likewise, built from the template at enqueue time
    std::wstring aux;          // Fx: "FROM|TO"
    std::wstring fallback;     // chart jobs: TMX endpoint to try when the primary fails ("" = none)
    const RangeSpec* spec = nullptr;   // chart jobs: what was asked for (drives the fallback request)
    HWND         notify = nullptr;  // Search only: window that receives the result
};

// Posted to the notify window when a job completes.
constexpr UINT WM_APP_SUMMARY_READY = WM_APP + 1;  // wParam = stock
constexpr UINT WM_APP_CHART_READY   = WM_APP + 2;  // wParam = stock, lParam = range
constexpr UINT WM_APP_INSET_READY   = WM_APP + 3;  // wParam = stock, lParam = range
constexpr UINT WM_APP_QUOTE_READY   = WM_APP + 4;  // fundamentals for all symbols
constexpr UINT WM_APP_SEARCH_READY  = WM_APP + 5;  // posted to the search job's notify window
constexpr UINT WM_APP_FX_READY      = WM_APP + 6;  // an FX rate arrived (see CopyFx)
constexpr UINT WM_APP_NEWS_READY    = WM_APP + 7;  // wParam = stock
constexpr UINT WM_APP_BENCH_READY   = WM_APP + 8;  // lParam = range
constexpr UINT WM_APP_BACKOFF       = WM_APP + 9;  // wParam = seconds the provider asked us to wait

class Fetcher {
public:
    Fetcher() = default;
    ~Fetcher();
    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;

    bool Start(HWND notify, const Config& cfg, std::wstring& err);
    void Stop();

    // Replaces the configuration (UI thread). Pending jobs are dropped because
    // their indices may no longer line up; the caller re-requests what it needs.
    void UpdateConfig(const Config& cfg);

    // Queues a job for stock `stock` (duplicates are coalesced). False if the
    // queue is full or the index is out of range. Kinds: Summary, Chart,
    // Inset, News.
    bool Enqueue(JobKind kind, size_t stock, size_t range);

    // Queues one batch fundamentals request for every configured symbol.
    // Returns false (without queuing) when the feature is disabled.
    bool EnqueueQuote();

    // Queues a symbol search; the result goes to `notify` as
    // WM_APP_SEARCH_READY. Replaces any pending search and jumps the queue.
    // False when the feature is disabled or the query is empty.
    bool EnqueueSearch(const std::wstring& query, HWND notify);
    bool SearchEnabled() const { return !cfg_.searchUrlTemplate.empty(); }
    bool NewsEnabled() const { return cfg_.newsSource != NewsSource::None; }

    // Queues an FX rate fetch (from -> to, e.g. USD -> CAD) via the chart
    // endpoint's "FROMTO=X" symbols.
    bool EnqueueFx(const std::wstring& from, const std::wstring& to);

    // Queues the benchmark index (cfg.benchmark) at `range`. False when unset.
    bool EnqueueBench(size_t range);

    // Copy the latest result for the UI thread. False if none / stale. The
    // caller must compare QuoteData::symbol with what it expects at `stock`.
    bool CopySummary(size_t stock, QuoteData& out);
    bool CopyChart(size_t stock, size_t range, QuoteData& out);
    bool CopyInset(size_t stock, size_t range, QuoteData& out);
    bool CopyQuotes(std::array<QuoteStats, kMaxStocks>& out, size_t& count, std::wstring& err);
    bool CopySearch(std::array<SearchHit, kMaxSearchHits>& out, size_t& count,
                    std::wstring& query, std::wstring& err);
    bool CopyFx(std::array<FxRate, kMaxFx>& out);
    bool CopyNews(size_t stock, std::array<NewsItem, kMaxNews>& out, size_t& count,
                  std::wstring& symbol, std::wstring& err);
    bool CopyBench(size_t range, QuoteData& out);

    // Diagnostics: per-endpoint health, rate-limit backoff and queue depth.
    struct Health {
        std::array<EndpointHealth, kEndpointCount> endpoints{};
        int64_t backoffUntil = 0;   // Unix time; 0 = not backing off
        size_t  pending      = 0;
    };
    void CopyHealth(Health& out);

private:
    static unsigned __stdcall ThreadEntry(void* arg);
    void Run();
    bool Pop(FetchJob& job);
    bool Push(const FetchJob& job, bool front);
    void Process(const FetchJob& job);
    void ProcessQuote(const FetchJob& job);
    void ProcessSearch(const FetchJob& job);
    void ProcessFx(const FetchJob& job);
    void ProcessNews(const FetchJob& job);
    void ProcessBench(const FetchJob& job);
    bool Get(Endpoint ep, const std::wstring& url, HttpResult& res, std::wstring& err);   // http_.Get + health/backoff
    bool InBackoff(std::wstring& why);
    void Record(Endpoint ep, bool ok, const HttpResult& res, const std::wstring& err);
    bool Fetch(const FetchJob& job, QuoteData& out);
    bool FetchTmx(const FetchJob& job, QuoteData& out, std::wstring& err);
    bool FetchBocRate(const std::wstring& from, const std::wstring& to, double& rate, std::wstring& err);
    bool EnsureCrumb(std::wstring& err);
    std::wstring BuildUrl(const std::wstring& symbol, const RangeSpec& spec) const;

    HWND   notify_ = nullptr;
    Config cfg_;                 // UI-thread only; the worker reads jobs, never this
    HANDLE thread_ = nullptr;

    // Worker-only state, allocated once in Start().
    HttpClient                 http_;
    std::wstring               crumb_;
    std::unique_ptr<char[]>    buf_;
    std::unique_ptr<QuoteData> scratch_;

    std::mutex                       qMutex_;
    std::condition_variable          qCv_;
    std::array<FetchJob, kMaxJobs>   queue_{};
    size_t                           qHead_  = 0;
    size_t                           qCount_ = 0;
    bool                             stop_   = false;

    // Mailboxes, guarded by dataMutex_.
    std::mutex dataMutex_;
    std::unique_ptr<std::array<QuoteData, kMaxStocks>> summaries_;
    std::unique_ptr<std::array<QuoteData, kMaxStocks>> charts_;
    std::array<size_t, kMaxStocks>                     chartRange_{};
    std::array<bool, kMaxStocks>                       chartValid_{};
    std::unique_ptr<std::array<QuoteData, kMaxStocks>> insets_;
    std::array<size_t, kMaxStocks>                     insetRange_{};
    std::array<bool, kMaxStocks>                       insetValid_{};
    std::unique_ptr<std::array<QuoteStats, kMaxStocks>> quotes_;
    size_t       quoteCount_ = 0;
    std::wstring quoteError_;
    std::unique_ptr<std::array<SearchHit, kMaxSearchHits>> search_;
    size_t       searchCount_ = 0;
    std::wstring searchQuery_;
    std::wstring searchError_;
    std::array<FxRate, kMaxFx> fx_{};
    struct NewsSlot {
        std::array<NewsItem, kMaxNews> items{};
        size_t       count = 0;
        std::wstring symbol;
        std::wstring error;
        bool         valid = false;
    };
    std::unique_ptr<std::array<NewsSlot, kMaxStocks>> news_;
    std::unique_ptr<QuoteData> bench_;
    size_t benchRange_ = 0;
    bool   benchValid_ = false;

    Health   health_;            // guarded by dataMutex_
    unsigned backoffSteps_ = 0;  // doubles the pause on repeated 429s
};

} // namespace st
