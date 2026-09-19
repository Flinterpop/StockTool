// Main window: stock list, header, chart, stats and status line.
#pragma once

#include "chart.h"
#include "common.h"
#include "config.h"
#include "fetcher.h"

#include <memory>

namespace st {

class App {
public:
    App() = default;
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Create(HINSTANCE hInst, int nCmdShow, std::wstring& err);
    int  Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // message handlers
    void OnCreate();
    void OnDestroy();
    void OnSize();
    void OnPaint();
    void OnCommand(WPARAM wp);
    void OnTimer();
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnDpiChanged(WPARAM wp, LPARAM lp);
    void OnMeasureItem(MEASUREITEMSTRUCT* mis);
    void OnDrawItem(const DRAWITEMSTRUCT* dis);
    void OnSummaryReady(size_t stock);
    void OnChartReady(size_t stock, size_t range);
    void OnAddTicker();
    void OnRemoveTicker();

    // helpers
    void CreateControls();
    void CreateFonts();
    void Layout();
    void EnsureBackBuffer(HDC hdc, int w, int h);
    void FreeBackBuffer();
    void SelectStock(size_t index);
    void SelectRange(size_t index);
    void RequestChart(bool clearCurrent);
    void RequestAllSummaries();
    void SetStatus(const std::wstring& text);
    int  Px(int dip) const;

    // painting
    void PaintHeader(Gdiplus::Graphics& g);
    void PaintStats(Gdiplus::Graphics& g);
    void PaintStatus(Gdiplus::Graphics& g);
    void PaintListItem(Gdiplus::Graphics& g, const RECT& rc, size_t index, bool selected);

    // layout rectangles (client px)
    RECT listRect_{};
    RECT headerRect_{};
    RECT chartRect_{};
    RECT statsRect_{};
    RECT statusRect_{};

    HINSTANCE hInst_ = nullptr;
    HWND      hwnd_  = nullptr;
    HWND      hList_ = nullptr;
    std::array<HWND, kRanges.size()> hRangeBtns_{};
    HWND      hStyleBtn_   = nullptr;
    HWND      hRefreshBtn_ = nullptr;
    HWND      hAddBtn_     = nullptr;
    HWND      hRemoveBtn_  = nullptr;
    HFONT     hUiFont_     = nullptr;
    HACCEL    hAccel_      = nullptr;

    // back buffer
    HDC     memDC_  = nullptr;
    HBITMAP memBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int     bufW_   = 0;
    int     bufH_   = 0;

    Config  cfg_;
    Fetcher fetcher_;
    std::unique_ptr<std::array<QuoteData, kMaxStocks>> summaries_;
    std::unique_ptr<QuoteData> chart_;
    bool    chartLoading_ = true;

    size_t selected_ = 0;
    size_t range_    = 5;
    bool   candles_  = false;
    int    hoverX_   = -1;
    int    hoverY_   = -1;
    bool   tracking_ = false;
    float  scale_    = 1.0f;
    std::wstring status_;
};

} // namespace st
