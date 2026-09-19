// Background worker that fetches quotes and hands results to the UI thread.
#pragma once

#include "common.h"
#include "config.h"

#include <windows.h>

#include <condition_variable>
#include <memory>
#include <mutex>

namespace st {

enum class JobKind : uint8_t { Summary, Chart };

struct FetchJob {
    JobKind kind  = JobKind::Summary;
    size_t  stock = 0;   // index into Config::stocks
    size_t  range = 0;   // index into kRanges (Chart only)
};

// Posted to the notify window when a job completes.
constexpr UINT WM_APP_SUMMARY_READY = WM_APP + 1;  // wParam = stock
constexpr UINT WM_APP_CHART_READY   = WM_APP + 2;  // wParam = stock, lParam = range

class Fetcher {
public:
    Fetcher() = default;
    ~Fetcher();
    Fetcher(const Fetcher&) = delete;
    Fetcher& operator=(const Fetcher&) = delete;

    bool Start(HWND notify, const Config& cfg, std::wstring& err);
    void Stop();

    // Queues a job (duplicates are coalesced). False if the queue is full.
    bool Enqueue(const FetchJob& job);

    // Copy the latest result for the UI thread. False if none / stale.
    bool CopySummary(size_t stock, QuoteData& out);
    bool CopyChart(size_t stock, size_t range, QuoteData& out);

private:
    static unsigned __stdcall ThreadEntry(void* arg);
    void Run();
    bool Pop(FetchJob& job);
    void Process(const FetchJob& job);
    bool Fetch(const std::wstring& url, QuoteData& out);
    std::wstring BuildUrl(size_t stock, const RangeSpec& spec) const;

    HWND   notify_ = nullptr;
    Config cfg_;
    HANDLE thread_ = nullptr;

    // Worker-only scratch space, allocated once in Start().
    std::unique_ptr<char[]>    buf_;
    std::unique_ptr<QuoteData> scratch_;

    std::mutex                       qMutex_;
    std::condition_variable          qCv_;
    std::array<FetchJob, kMaxJobs>   queue_{};
    size_t                           qHead_  = 0;
    size_t                           qCount_ = 0;
    bool                             stop_   = false;

    std::mutex dataMutex_;
    std::unique_ptr<std::array<QuoteData, kMaxStocks>> summaries_;
    std::unique_ptr<QuoteData> chart_;
    size_t chartStock_ = 0;
    size_t chartRange_ = 0;
    bool   chartValid_ = false;
};

} // namespace st
