// Main window: watch list, header, chart, stats, status line, tray icon.
#pragma once

#include "chart.h"
#include "common.h"
#include "config.h"
#include "fetcher.h"
#include "theme.h"

#include <shellapi.h>

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
    // Derived per-stock figures used by the list, header and portfolio strip.
    struct PriceChange {
        bool   valid  = false;
        double last   = 0.0;
        double prev   = 0.0;
        double change = 0.0;
        double pct    = 0.0;
    };
    struct AlertState {
        bool aboveActive = false;
        bool belowActive = false;
    };
    struct Portfolio {
        bool         any = false;
        double       value = 0.0;
        double       cost  = 0.0;
        double       day   = 0.0;
        std::wstring currency;   // "" = mixed
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // message handlers
    void OnCreate();
    void OnDestroy();
    void OnSize(WPARAM wp);
    void OnPaint();
    void OnCommand(int id, UINT code);
    void OnContextMenu(HWND source, int x, int y);
    void OnTimer();
    void OnMouseMove(int x, int y, bool buttonDown);
    void OnMouseLeave();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    bool OnSetCursor();
    void OnDpiChanged(WPARAM wp, LPARAM lp);
    void OnMeasureItem(MEASUREITEMSTRUCT* mis);
    void OnDrawItem(const DRAWITEMSTRUCT* dis);
    void OnSettingChange(LPARAM lp);
    void OnTray(LPARAM lp, WPARAM wp);
    void OnSummaryReady(size_t stock);
    void OnChartReady(size_t stock, size_t range);
    void OnInsetReady(size_t stock, size_t range);
    void OnQuoteReady();

    // ticker commands
    void OnAddTicker();
    bool AddTicker(const StockEntry& entry, std::wstring& err);   // called from the dialog
    void OnEditTicker();
    bool ApplyTickerEdit(size_t index, const StockEntry& edited, std::wstring& err);
    void OnRemoveTicker();
    void OnMoveTicker(int delta);
    void OnEditHolding();
    void OnEditAlerts();
    void OnReloadConfig();

    // view commands
    void ToggleOption(bool& flag);
    void SetThemeMode(ThemeMode mode);
    void ApplyTheme();
    void SyncViewMenu();

    // helpers
    ChartInput BuildChartInput();               // from current state (hover included)
    bool InsetHit(int x, int y, Gdiplus::RectF& box);  // client px -> inset box (chart px)
    void CreateControls();
    void CreateFonts();
    void Layout();
    void EnsureBackBuffer(HDC hdc, int w, int h);
    void FreeBackBuffer();
    void SelectStock(size_t index);
    void SelectRange(size_t index);
    void RequestChart(bool clearCurrent);
    void RequestInset();
    void RequestAllSummaries();
    void RequestQuotes();
    void RebuildList();
    void SetStatus(const std::wstring& text);
    void SaveState();
    void ApplyStartupState(int nCmdShow);
    int  Px(int dip) const;
    PriceChange ComputeChange(const QuoteData& q) const;
    Portfolio   ComputePortfolio() const;
    void CheckAlerts(size_t stock, const PriceChange& pc);
    void UpdateTrayTip();
    void ShowFromTray();
    void TrayBalloon(const std::wstring& title, const std::wstring& text);

    // painting
    void PaintHeader(Gdiplus::Graphics& g);
    void PaintPortfolio(Gdiplus::Graphics& g);
    void PaintStats(Gdiplus::Graphics& g);
    void PaintStatus(Gdiplus::Graphics& g);
    void PaintListItem(Gdiplus::Graphics& g, const RECT& rc, size_t index, bool selected);

    // layout rectangles (client px)
    RECT portfolioRect_{};
    RECT listRect_{};
    RECT headerRect_{};
    RECT chartRect_{};
    RECT statsRect_{};
    RECT statusRect_{};

    HINSTANCE hInst_ = nullptr;
    HWND      hwnd_  = nullptr;
    HWND      hList_ = nullptr;
    HMENU     hMenu_ = nullptr;
    std::array<HWND, kRanges.size()> hRangeBtns_{};
    HWND      hStyleBtn_   = nullptr;
    HWND      hCompareBtn_ = nullptr;
    HWND      hRefreshBtn_ = nullptr;
    HWND      hAddBtn_     = nullptr;
    HWND      hRemoveBtn_  = nullptr;
    HWND      hReloadBtn_  = nullptr;
    HFONT     hUiFont_     = nullptr;
    HACCEL    hAccel_      = nullptr;
    HBRUSH    hBgBrush_    = nullptr;
    HBRUSH    hListBrush_  = nullptr;

    // back buffer
    HDC     memDC_  = nullptr;
    HBITMAP memBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int     bufW_   = 0;
    int     bufH_   = 0;

    // tray
    NOTIFYICONDATAW tray_{};
    bool            trayAdded_ = false;

    Config       cfg_;
    ViewState    state_;
    const Theme* theme_ = nullptr;
    Fetcher      fetcher_;

    // data copied from the fetcher (UI thread owned)
    std::unique_ptr<std::array<QuoteData, kMaxStocks>>  summaries_;
    std::unique_ptr<std::array<QuoteData, kMaxStocks>>  charts_;     // per stock, at range_
    std::array<bool, kMaxStocks>                        chartValid_{};
    std::unique_ptr<QuoteData>                          inset_;
    bool                                                insetValid_ = false;
    std::unique_ptr<std::array<QuoteStats, kMaxStocks>> quotes_;
    size_t                                              quoteCount_ = 0;
    std::array<AlertState, kMaxStocks>                  alerts_{};
    std::array<CompareEntry, kMaxStocks>                compareEntries_{};

    size_t selected_ = 0;
    size_t range_    = kRange1Y;
    int    hoverX_   = -1;
    int    hoverY_   = -1;
    bool   tracking_ = false;
    bool   dragging_ = false;   // inset drag in progress (mouse captured)
    float  dragDX_   = 0.0f;    // grab offset from the inset's top-left (px)
    float  dragDY_   = 0.0f;
    float  scale_    = 1.0f;
    std::wstring status_;
};

} // namespace st
