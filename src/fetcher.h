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

enum class JobKind : uint8_t { Summary, Chart, Inset, Quote };

struct FetchJob {
    JobKind      kind  = JobKind::Summary;
    size_t       stock = 0;   // index into Config::stocks at enqueue time
    size_t       range = 0;   // index into kRanges (Chart/Inset)
    std::wstring symbol;      // captured at enqueue time; the worker never reads Config
    std::wstring url;         // likewise, built from the template at enqueue time
};

// Posted to the notify window when a job completes.
constexpr UINT WM_APP_SUMMARY_READY = WM_APP + 1;  // wParam = stock
constexpr UINT WM_APP_CHART_READY   = WM_APP + 2;  // wParam = stock, lParam = range
constexpr UINT WM_APP_INSET_READY   = WM_APP + 3;  // wParam = stock, lParam = range
constexpr UINT WM_APP_QUOTE_READY   = WM_APP + 4;  // fundamentals for all symbols

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
    // queue is full or the index is out of range.
    bool Enqueue(JobKind kind, size_t stock, size_t range);

    // Queues one batch fundamentals request for every configured symbol.
    // Returns false (without queuing) when the feature is disabled.
    bool EnqueueQuote();

    // Copy the latest result for the UI thread. False if none / stale. The
    // caller must compare QuoteData::symbol with what it expects at `stock`.
    bool CopySummary(size_t stock, QuoteData& out);
    bool CopyChart(size_t stock, size_t range, QuoteData& out);
    bool CopyInset(size_t stock, size_t range, QuoteData& out);
    bool CopyQuotes(std::array<QuoteStats, kMaxStocks>& out, size_t& count, std::wstring& err);

private:
    static unsigned __stdcall ThreadEntry(void* arg);
    void Run();
    bool Pop(FetchJob& job);
    void Process(const FetchJob& job);
    void ProcessQuote(const FetchJob& job);
    bool Fetch(const std::wstring& url, QuoteData& out);
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
    std::unique_ptr<QuoteData> inset_;
    size_t inset_stock_ = 0;
    size_t inset_range_ = 0;
    bool   inset_valid_ = false;
    std::unique_ptr<std::array<QuoteStats, kMaxStocks>> quotes_;
    size_t       quoteCount_ = 0;
    std::wstring quoteError_;
};

} // namespace st
