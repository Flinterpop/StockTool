// Main window: watch lists, header, chart, news, stats, status line, tray icon.
#pragma once

#include "chart.h"
#include "common.h"
#include "config.h"
#include "fetcher.h"
#include "position.h"
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

    static constexpr size_t kToolCount = 10;  // view toggles on the toolbar

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
        bool         any     = false;
        bool         partial = false;   // an FX rate is still missing
        bool         incomplete = false; // a held ticker has no price yet
        double       value   = 0.0;     // in cfg_.portfolioCurrency
        double       cost    = 0.0;
        double       day     = 0.0;
        double       income  = 0.0;     // trailing-12-month dividends x shares
        double       realised = 0.0;    // from sells recorded as transactions
        double       received = 0.0;    // dividends actually received (transactions x ex-dates)
    };
    struct NewsCache {
        std::array<NewsItem, kMaxNews> items{};
        size_t       count = 0;
        std::wstring error;
        bool         valid = false;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // message handlers
    void OnCreate();
    void OnDestroy();
    void OnSize(WPARAM wp);
    void OnPaint();
    void OnCommand(int id, UINT code);
    LRESULT OnNotify(const NMHDR* hdr);
    void OnContextMenu(HWND source, int x, int y);
    void OnTimer();
    void Refresh();                     // re-fetch everything now
    void LoadPortfolioHistory();
    void MaybeRecordHistory();          // one row per day, once the portfolio total is complete
    PortfolioInput BuildPortfolioInput();
    // True when every ticker with a known session is outside it. `nextOpen`
    // = earliest known upcoming open (INT64_MAX if none is known yet).
    bool MarketsClosed(int64_t& nextOpen) const;
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
    void OnFxReady();
    void OnNewsReady(size_t stock);
    void OnBenchReady(size_t range);
    void OnBackoff(size_t seconds);

    // ticker commands
    void OnAddTicker();
    bool AddTicker(const StockEntry& entry, std::wstring& err);   // called from the dialog
    void OnEditTicker();
    bool ApplyTickerEdit(size_t index, const StockEntry& edited, std::wstring& err);
    void OnRemoveTicker();
    void OnMoveTicker(int delta);
    void OnEditHolding();
    void OnEditTransactions();
    void OnEditAlerts();
    void OnShowHealth();
    void OnReloadConfig();
    void OnExportList();
    void OnExportChart();
    void OnImportBroker();

    // watch lists
    void SwitchList(const std::wstring& name);
    void OnNewList();
    void OnRenameList();
    void OnDeleteList();
    void RebuildTabs();
    void RebuildListMenu();
    void ApplyConfig(const Config& fresh);   // swap in a freshly loaded config

    // view commands
    void ToggleOption(bool& flag);
    void SetThemeMode(ThemeMode mode);
    void ApplyTheme();
    void SyncViewMenu();

    // helpers
    ChartInput BuildChartInput();
    bool InsetHit(int x, int y, Gdiplus::RectF& box);
    int  NewsRowAt(int x, int y) const;       // -1 when not over a headline
    void CreateControls();
    void CreateFonts();
    void Layout();
    void EnsureBackBuffer(HDC hdc, int w, int h);
    void FreeBackBuffer();
    void PaintChartLayer(Gdiplus::Graphics& g, const ChartInput& in);
    void RedrawChart();
    void RedrawAll();
    void InvalidateListItem(size_t index);
    bool HasStocks() const { return cfg_.stockCount > 0; }
    void SelectStock(size_t index);
    void SelectRange(size_t index);
    void RequestChart(bool clearCurrent);
    void RequestInset(bool clearCurrent);
    void RequestNews(bool clearCurrent);
    void RequestBench();
    void PrefetchOthers();
    void RequestAllSummaries();
    void RequestQuotes();
    void EnsureFxRates();
    void RebuildList();
    void SetStatus(const std::wstring& text);
    void SaveState();
    void ApplyStartupState(int nCmdShow);
    int  Px(int dip) const;
    PriceChange  ComputeChange(const QuoteData& q) const;
    std::wstring CurrencyOf(size_t index) const;          // override or provider's
    double       RateToPortfolio(const std::wstring& cur) const;   // NaN when unknown
    double       TrailingDividends(size_t index) const;   // per share, last 365 days
    Position     PositionOf(size_t index) const;          // from transactions, else the manual holding
    double       DividendsReceivedFor(size_t index) const; // NaN when no inset data yet
    std::wstring HealthText();
    Portfolio    ComputePortfolio() const;
    void CheckAlerts(size_t stock, const PriceChange& pc);
    void UpdateTrayTip();
    void ShowFromTray();
    void TrayBalloon(const std::wstring& title, const std::wstring& text);
    bool SaveCsvDialog(const wchar_t* suggested, std::wstring& path);
    bool OpenCsvDialog(std::wstring& path);
    static bool ReadWholeFile(const std::wstring& path, std::string& bytes, std::wstring& err);
    bool WriteTextFile(const std::wstring& path, const std::string& utf8, std::wstring& err);

    // painting
    void PaintHeader(Gdiplus::Graphics& g);
    void PaintPortfolio(Gdiplus::Graphics& g);
    void PaintNews(Gdiplus::Graphics& g);
    void PaintStats(Gdiplus::Graphics& g);
    void PaintStatus(Gdiplus::Graphics& g);
    void PaintListItem(Gdiplus::Graphics& g, const RECT& rc, size_t index, bool selected);

    // layout rectangles (client px)
    RECT toolbarRect_{};
    RECT tabsRect_{};
    RECT portfolioRect_{};
    RECT listRect_{};
    RECT headerRect_{};
    RECT chartRect_{};
    RECT newsRect_{};
    RECT statsRect_{};
    RECT statusRect_{};

    HINSTANCE hInst_ = nullptr;
    HWND      hwnd_  = nullptr;
    HWND      hList_ = nullptr;
    HWND      hTabs_ = nullptr;
    HMENU     hMenu_ = nullptr;
    std::array<HWND, kRanges.size()> hRangeBtns_{};
    std::array<HWND, kToolCount> hToolBtns_{};
    HWND      hRefreshBtn_ = nullptr;
    HWND      hAddBtn_     = nullptr;
    HWND      hRemoveBtn_  = nullptr;
    HWND      hReloadBtn_  = nullptr;
    HFONT     hUiFont_     = nullptr;
    HACCEL    hAccel_      = nullptr;
    HBRUSH    hBgBrush_    = nullptr;
    HBRUSH    hListBrush_  = nullptr;

    // back buffer (whole client area)
    HDC     memDC_  = nullptr;
    HBITMAP memBmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int     bufW_   = 0;
    int     bufH_   = 0;

    // cached chart base layer: re-rendered only when chartDirty_ (data,
    // options, size, theme); the hover overlay is drawn on top per paint
    HDC     chartDC_    = nullptr;
    HBITMAP chartBmp_   = nullptr;
    HGDIOBJ chartOld_   = nullptr;
    int     chartW_     = 0;
    int     chartH_     = 0;
    bool    chartDirty_ = true;

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
    std::unique_ptr<std::array<QuoteData, kMaxStocks>>  insets_;     // per stock, at cfg_.insetRange
    std::array<bool, kMaxStocks>                        insetValid_{};
    std::unique_ptr<QuoteData>                          incoming_;   // scratch for result copies
    std::unique_ptr<std::array<QuoteStats, kMaxStocks>> quotes_;
    size_t                                              quoteCount_ = 0;
    std::unique_ptr<std::array<NewsCache, kMaxStocks>>  news_;
    std::unique_ptr<QuoteData>                          bench_;      // benchmark index at range_
    int64_t lastRefresh_ = 0;                                        // Unix time of the last full refresh
    std::unique_ptr<History>                            history_;    // portfolio value per day, active list
    int64_t lastHistoryWrite_ = 0;
    bool                                                benchValid_ = false;
    std::array<FxRate, kMaxFx>                          fx_{};
    std::array<AlertState, kMaxStocks>                  alerts_{};
    std::array<CompareEntry, kMaxStocks>                compareEntries_{};

    size_t selected_ = 0;
    size_t range_    = kRange1Y;
    int    hoverX_   = -1;
    int    hoverY_   = -1;
    bool   tracking_ = false;
    bool   dragging_ = false;   // inset drag in progress
    float  dragDX_   = 0.0f;    // grab offset from the inset's top-left (px)
    float  dragDY_   = 0.0f;
    float  scale_    = 1.0f;
    std::wstring status_;
};

} // namespace st
