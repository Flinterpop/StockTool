#include "app.h"

#include "brokerimport.h"
#include "dialogs.h"
#include "history.h"
#include "help.h"
#include "resource.h"
#include "textfmt.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cassert>
#include <cmath>
#include <ctime>
#include <limits>

namespace st {
namespace {

using namespace Gdiplus;

constexpr wchar_t kClassName[] = L"StockToolMainWindow";

#define ST_WIDE_(s) L##s
#define ST_WIDE(s)  ST_WIDE_(s)
constexpr wchar_t kVersionText[] = ST_WIDE(STOCKTOOL_VERSION);
constexpr wchar_t kWindowTitle[] = L"StockTool v" ST_WIDE(STOCKTOOL_VERSION);

// Child-control IDs (menu commands are IDM_* from resource.h, 1000+).
constexpr int IDC_LIST       = 100;
constexpr int IDC_TABS       = 101;
constexpr int IDC_RANGE_BASE = 200;   // + range index
constexpr int IDC_REFRESH    = 301;
constexpr int IDC_ADD        = 302;
constexpr int IDC_REMOVE     = 303;
constexpr int IDC_RELOAD     = 304;

// Toolbar: one toggle per View-menu command that changes the plot. The
// buttons use the menu command IDs, so one handler serves both.
struct ToolSpec { int id; const wchar_t* label; int widthDip; };
constexpr std::array<ToolSpec, 10> kTools = {{
    { IDM_CANDLES,   L"Candles",   70 },
    { IDM_COMPARE,   L"Compare",   72 },
    { IDM_SMA20,     L"SMA 20",    62 },
    { IDM_SMA50,     L"SMA 50",    62 },
    { IDM_BOLLINGER, L"Bollinger", 76 },
    { IDM_RSI,       L"RSI",       50 },
    { IDM_INSET,     L"Inset",     58 },
    { IDM_NEWS,      L"News",      58 },
    { IDM_BENCHMARK, L"Bench",     58 },
    { IDM_PORTFOLIO, L"History",   62 },
}};
static_assert(kTools.size() == App::kToolCount, "toolbar table and button array differ");

constexpr UINT     WM_APP_TRAY   = WM_APP + 10;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT     kTrayId       = 1;

// Layout constants in device-independent pixels.
constexpr int kMargin      = 10;
constexpr int kListW       = 250;
constexpr int kListItemH   = 44;
constexpr int kTabsH       = 26;
constexpr int kPortfolioH  = 78;
constexpr int kToolbarH    = 26;
constexpr int kHeaderH     = 76;
constexpr int kButtonH     = 26;
constexpr int kRangeBtnW   = 46;
constexpr int kWideBtnW    = 84;
constexpr int kBtnGap      = 4;
constexpr int kNewsH       = 150;
constexpr int kNewsRowH    = 20;
constexpr int kStatsH      = 112;
constexpr int kStatusH     = 20;
constexpr int kListBtnGap  = 6;
constexpr int kMinWinW     = 960;
constexpr int kMinWinH     = 620;

constexpr DWORD  kDwmUseImmersiveDarkMode = 20;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top),
                 static_cast<REAL>(r.right - r.left), static_cast<REAL>(r.bottom - r.top));
}

// Day number in exchange-local time, for "is this bar today's bar?" checks.
int64_t LocalDay(int64_t unixTime, int32_t gmtOffset) {
    return (unixTime + gmtOffset) / 86400;
}

std::wstring TimeNow() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::array<wchar_t, 32> b{};
    swprintf_s(b.data(), b.size(), L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return b.data();
}

int64_t UnixNow() {
    return static_cast<int64_t>(_time64(nullptr));
}

// This machine's offset from UTC right now, for showing exchange times locally.
int32_t LocalOffsetSec() {
    TIME_ZONE_INFORMATION tz{};
    const DWORD kind = GetTimeZoneInformation(&tz);
    LONG bias = tz.Bias;   // minutes, UTC = local + bias
    if (kind == TIME_ZONE_ID_DAYLIGHT) { bias += tz.DaylightBias; }
    else if (kind == TIME_ZONE_ID_STANDARD) { bias += tz.StandardBias; }
    return static_cast<int32_t>(-bias * 60);
}

bool SameSymbol(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// True when a refreshed fetch would draw exactly what is already on screen,
// so the (expensive) chart re-render can be skipped.
bool SameBars(const QuoteData& a, const QuoteData& b) {
    if (a.valid != b.valid || a.error != b.error) { return false; }
    if (!a.valid) { return true; }
    const Series& x = a.series;
    const Series& y = b.series;
    if (x.count != y.count || a.meta.price != b.meta.price || a.meta.marketTime != b.meta.marketTime ||
        a.dividendCount != b.dividendCount) { return false; }
    if (x.count == 0) { return true; }
    const Candle& xl = x.pts[x.count - 1];
    const Candle& yl = y.pts[y.count - 1];
    return x.pts[0].time == y.pts[0].time && xl.time == yl.time && xl.close == yl.close &&
           xl.high == yl.high && xl.low == yl.low && xl.volume == yl.volume;
}

Font MakeFont(float pt, float scale, INT style = FontStyleRegular) {
    return Font(L"Segoe UI", FontPx(pt, scale), style, UnitPixel);
}

// CSV helpers: RFC 4180 quoting, UTF-8 output.
std::string Utf8(const std::wstring& s) {
    if (s.empty()) { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return {}; }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string CsvField(const std::wstring& s) {
    std::wstring q;
    q.reserve(s.size() + 2);
    bool needQuotes = false;
    for (wchar_t c : s) {
        if (c == L'"') { q += L'"'; needQuotes = true; }
        if (c == L',' || c == L'\n' || c == L'\r') { needQuotes = true; }
        q += c;
    }
    return Utf8(needQuotes ? L"\"" + q + L"\"" : q);
}

std::string CsvNum(double v, int decimals) {
    if (!std::isfinite(v)) { return {}; }
    std::array<char, 64> b{};
    sprintf_s(b.data(), b.size(), "%.*f", decimals, v);
    return b.data();
}

} // namespace

App::~App() {
    fetcher_.Stop();
    if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE, &tray_); }
    FreeBackBuffer();
    if (chartDC_ != nullptr) {
        SelectObject(chartDC_, chartOld_);
        DeleteDC(chartDC_);
    }
    if (chartBmp_ != nullptr)   { DeleteObject(chartBmp_); }
    if (hUiFont_ != nullptr)    { DeleteObject(hUiFont_); }
    if (hBgBrush_ != nullptr)   { DeleteObject(hBgBrush_); }
    if (hListBrush_ != nullptr) { DeleteObject(hListBrush_); }
    if (hAccel_ != nullptr)     { DestroyAcceleratorTable(hAccel_); }
}

int App::Px(int dip) const {
    assert(scale_ > 0.0f);
    return static_cast<int>(std::lround(static_cast<float>(dip) * scale_));
}

// ---------------------------------------------------------------------------
// Creation / message loop

bool App::Create(HINSTANCE hInst, int nCmdShow, std::wstring& err) {
    assert(hInst != nullptr);
    hInst_ = hInst;

    const std::wstring cfgPath = DefaultConfigPath();
    if (GetFileAttributesW(cfgPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!WriteDefaultConfig(cfgPath, err)) { return false; }
    }
    LoadViewState(cfgPath, state_);
    if (!LoadConfig(cfgPath, state_.list, cfg_, err)) { return false; }
    state_.list = cfg_.listName;
    range_ = (state_.range < kRanges.size()) ? state_.range : cfg_.defaultRange;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (SameSymbol(cfg_.stocks[i].symbol, state_.selected)) { selected_ = i; }
    }
    summaries_ = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    charts_    = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    insets_    = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    incoming_  = std::make_unique<QuoteData>();
    quotes_    = std::make_unique<std::array<QuoteStats, kMaxStocks>>();
    news_      = std::make_unique<std::array<NewsCache, kMaxStocks>>();
    bench_     = std::make_unique<QuoteData>();
    history_   = std::make_unique<History>();
    LoadPortfolioHistory();
    theme_     = &ThemeFor(cfg_.theme == ThemeMode::Dark ||
                           (cfg_.theme == ThemeMode::System && SystemPrefersDark()));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &App::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // Large icon for Alt-Tab/taskbar, small one for the title bar; both scaled
    // from the multi-size .ico by the system.
    const int bigCx   = GetSystemMetrics(SM_CXICON);
    const int smallCx = GetSystemMetrics(SM_CXSMICON);
    wc.hIcon   = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               bigCx, bigCx, LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                               smallCx, smallCx, LR_DEFAULTCOLOR));
    assert(wc.hIcon != nullptr && wc.hIconSm != nullptr);
    if (wc.hIcon == nullptr) { wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); }
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
        err = L"RegisterClassEx failed";
        return false;
    }

    hMenu_ = LoadMenuW(hInst, MAKEINTRESOURCEW(IDR_MAINMENU));
    assert(hMenu_ != nullptr);
    const float sysScale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    const int w = static_cast<int>(1200.0f * sysScale);
    const int h = static_cast<int>(780.0f * sysScale);
    hwnd_ = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, hMenu_, hInst, this);
    if (hwnd_ == nullptr) {
        err = L"CreateWindowEx failed";
        return false;
    }

    std::array<ACCEL, 7 + kRanges.size()> accel{};
    size_t n = 0;
    accel[n++] = { FVIRTKEY, VK_F5, static_cast<WORD>(IDM_REFRESH) };
    accel[n++] = { FVIRTKEY, VK_F1, static_cast<WORD>(IDM_HELP) };
    accel[n++] = { FVIRTKEY | FCONTROL, 'N', static_cast<WORD>(IDM_ADD) };
    accel[n++] = { FVIRTKEY | FCONTROL, 'F', static_cast<WORD>(IDM_ADD) };
    accel[n++] = { FVIRTKEY, VK_F2, static_cast<WORD>(IDM_EDIT) };
    accel[n++] = { FVIRTKEY | FCONTROL, VK_UP, static_cast<WORD>(IDM_MOVEUP) };
    accel[n++] = { FVIRTKEY | FCONTROL, VK_DOWN, static_cast<WORD>(IDM_MOVEDOWN) };
    for (size_t i = 0; i < kRanges.size(); ++i) {
        accel[n++] = { FVIRTKEY | FCONTROL, static_cast<WORD>('1' + i), static_cast<WORD>(IDM_RANGE_BASE + i) };
    }
    assert(n == accel.size());
    hAccel_ = CreateAcceleratorTableW(accel.data(), static_cast<int>(n));
    assert(hAccel_ != nullptr);

    ApplyStartupState(nCmdShow);
    return true;
}

// Restores saved placement and honours start_minimized / minimize_to_tray.
void App::ApplyStartupState(int nCmdShow) {
    assert(hwnd_ != nullptr);
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (state_.hasWindow && GetWindowPlacement(hwnd_, &wp)) {
        // Only trust a saved rectangle that still lands on a monitor.
        if (MonitorFromRect(&state_.window, MONITOR_DEFAULTTONULL) != nullptr) {
            wp.rcNormalPosition = state_.window;
            wp.showCmd          = SW_HIDE;
            SetWindowPlacement(hwnd_, &wp);
        }
    }
    int show = nCmdShow;
    if (cfg_.startMinimized)   { show = SW_SHOWMINNOACTIVE; }
    else if (state_.maximized) { show = SW_SHOWMAXIMIZED; }
    if (cfg_.startMinimized && cfg_.minimizeToTray) {
        show = SW_HIDE;  // tray only; the icon was added in OnCreate
        SetStatus(L"Started in the tray");
    }
    ShowWindow(hwnd_, show);
    if (show != SW_HIDE) { UpdateWindow(hwnd_); }
}

int App::Run() {
    assert(hwnd_ != nullptr);
    MSG msg{};
    // Message pump: runs until WM_QUIT.
    for (;;) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0)  { break; }
        if (r == -1) { return 1; }
        if (hAccel_ != nullptr && TranslateAcceleratorW(hwnd_, hAccel_, &msg) != 0) { continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = static_cast<App*>(cs->lpCreateParams);
        assert(self != nullptr);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) { return DefWindowProcW(hwnd, msg, wp, lp); }
    return self->Handle(msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:       OnCreate();  return 0;
    case WM_CLOSE:        SaveState(); DestroyWindow(hwnd_); return 0;
    case WM_QUERYENDSESSION: SaveState(); return TRUE;
    case WM_DESTROY:      OnDestroy(); return 0;
    case WM_SIZE:         OnSize(wp);  return 0;
    case WM_PAINT:        OnPaint();   return 0;
    case WM_ERASEBKGND:   return 1;
    case WM_COMMAND:      OnCommand(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_NOTIFY:       return OnNotify(reinterpret_cast<const NMHDR*>(lp));
    case WM_CONTEXTMENU:  OnContextMenu(reinterpret_cast<HWND>(wp), GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_TIMER:        if (wp == kRefreshTimer) { OnTimer(); } return 0;
    case WM_MOUSEMOVE:    OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_LBUTTON) != 0); return 0;
    case WM_MOUSELEAVE:   OnMouseLeave(); return 0;
    case WM_LBUTTONDOWN:  OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP:    OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && OnSetCursor()) { return TRUE; }
        break;
    case WM_DPICHANGED:   OnDpiChanged(wp, lp); return 0;
    case WM_SETTINGCHANGE: OnSettingChange(lp); return 0;
    case WM_MEASUREITEM:  OnMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp)); return TRUE;
    case WM_DRAWITEM:     OnDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), theme_->bgRef);
        SetTextColor(reinterpret_cast<HDC>(wp), theme_->textRef);
        return reinterpret_cast<LRESULT>(hBgBrush_);
    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wp), theme_->listBgRef);
        return reinterpret_cast<LRESULT>(hListBrush_);
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = Px(kMinWinW);
        mmi->ptMinTrackSize.y = Px(kMinWinH);
        return 0;
    }
    case WM_APP_TRAY:          OnTray(lp, wp); return 0;
    case WM_APP_SUMMARY_READY: OnSummaryReady(static_cast<size_t>(wp)); return 0;
    case WM_APP_CHART_READY:   OnChartReady(static_cast<size_t>(wp), static_cast<size_t>(lp)); return 0;
    case WM_APP_INSET_READY:   OnInsetReady(static_cast<size_t>(wp), static_cast<size_t>(lp)); return 0;
    case WM_APP_QUOTE_READY:   OnQuoteReady(); return 0;
    case WM_APP_FX_READY:      OnFxReady(); return 0;
    case WM_APP_NEWS_READY:    OnNewsReady(static_cast<size_t>(wp)); return 0;
    case WM_APP_BENCH_READY:   OnBenchReady(static_cast<size_t>(lp)); return 0;
    case WM_APP_BACKOFF:       OnBackoff(static_cast<size_t>(wp)); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Setup

void App::OnCreate() {
    scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    CreateFonts();
    CreateControls();
    ApplyTheme();
    SyncViewMenu();
    RebuildListMenu();
    Layout();

    tray_.cbSize           = sizeof(tray_);
    tray_.hWnd             = hwnd_;
    tray_.uID              = kTrayId;
    tray_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = WM_APP_TRAY;
    tray_.hIcon            = static_cast<HICON>(LoadImageW(hInst_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                           LR_DEFAULTCOLOR));
    wcscpy_s(tray_.szTip, kWindowTitle);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
    if (trayAdded_) {
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    std::wstring err;
    if (!fetcher_.Start(hwnd_, cfg_, err)) {
        SetStatus(err);
        return;
    }
    RequestAllSummaries();
    RequestQuotes();
    if (HasStocks()) {
        RequestChart(true);
        RequestInset(true);
        RequestNews(true);
    }
    RequestBench();
    SetTimer(hwnd_, kRefreshTimer, cfg_.refreshSeconds * 1000u, nullptr);
    SetStatus(L"Loading " + std::to_wstring(cfg_.stockCount) + L" symbols from " + cfg_.path);
}

void App::OnDestroy() {
    KillTimer(hwnd_, kRefreshTimer);
    fetcher_.Stop();
    if (trayAdded_) {
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        trayAdded_ = false;
    }
    PostQuitMessage(0);
}

void App::SaveState() {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        state_.window    = wp.rcNormalPosition;
        state_.hasWindow = true;
        state_.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    }
    state_.range    = range_;
    state_.selected = (selected_ < cfg_.stockCount) ? cfg_.stocks[selected_].symbol : L"";
    state_.list     = cfg_.listName;
    std::wstring err;
    const bool ok = SaveViewState(cfg_.path, state_, err);
    (void)ok;  // nothing useful to do at shutdown if the file is read-only
}

void App::CreateFonts() {
    if (hUiFont_ != nullptr) { DeleteObject(hUiFont_); }
    hUiFont_ = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    assert(hUiFont_ != nullptr);
    const HWND controls[] = { hList_, hTabs_, hRefreshBtn_, hAddBtn_, hRemoveBtn_, hReloadBtn_ };
    for (HWND h : controls) {
        if (h != nullptr) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE); }
    }
    for (HWND h : hRangeBtns_) {
        if (h != nullptr) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE); }
    }
    for (HWND h : hToolBtns_) {
        if (h != nullptr) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE); }
    }
    if (hList_ != nullptr) { SendMessageW(hList_, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(Px(kListItemH))); }
}

void App::CreateControls() {
    auto button = [this](const wchar_t* text, DWORD style, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 hInst_, nullptr);
        assert(h != nullptr);
        return h;
    };

    hTabs_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_TABSTOP | TCS_FOCUSNEVER,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TABS)),
                             hInst_, nullptr);
    assert(hTabs_ != nullptr);
    hList_ = CreateWindowExW(0, L"LISTBOX", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                             hInst_, nullptr);
    assert(hList_ != nullptr);
    RebuildList();
    RebuildTabs();

    for (size_t i = 0; i < kRanges.size(); ++i) {
        hRangeBtns_[i] = button(kRanges[i].label, BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0),
                                IDC_RANGE_BASE + static_cast<int>(i));
    }
    SendMessageW(hRangeBtns_[range_], BM_SETCHECK, BST_CHECKED, 0);
    for (size_t i = 0; i < kTools.size(); ++i) {
        hToolBtns_[i] = button(kTools[i].label, WS_GROUP | BS_AUTOCHECKBOX | BS_PUSHLIKE, kTools[i].id);
    }
    hRefreshBtn_ = button(L"Refresh (F5)", WS_GROUP | BS_PUSHBUTTON, IDC_REFRESH);
    hAddBtn_     = button(L"Add…", WS_GROUP | BS_PUSHBUTTON, IDC_ADD);
    hRemoveBtn_  = button(L"Remove", BS_PUSHBUTTON, IDC_REMOVE);
    hReloadBtn_  = button(L"Reload cfg", BS_PUSHBUTTON, IDC_RELOAD);
    CreateFonts();  // apply font + item height to the controls just made
}

void App::RebuildList() {
    SendMessageW(hList_, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        SendMessageW(hList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg_.stocks[i].symbol.c_str()));
    }
    if (selected_ >= cfg_.stockCount) { selected_ = 0; }
    if (HasStocks()) { SendMessageW(hList_, LB_SETCURSEL, static_cast<WPARAM>(selected_), 0); }
}

// One tab per watch list; hidden while there is only the default list.
void App::RebuildTabs() {
    if (hTabs_ == nullptr) { return; }
    TabCtrl_DeleteAllItems(hTabs_);
    int active = 0;
    for (size_t i = 0; i < cfg_.listCount; ++i) {
        std::wstring label = cfg_.lists[i].empty() ? L"Watch list" : cfg_.lists[i];
        TCITEMW item{};
        item.mask    = TCIF_TEXT;
        item.pszText = label.data();
        TabCtrl_InsertItem(hTabs_, static_cast<int>(i), &item);
        if (SameSymbol(cfg_.lists[i], cfg_.listName)) { active = static_cast<int>(i); }
    }
    TabCtrl_SetCurSel(hTabs_, active);
    ShowWindow(hTabs_, cfg_.listCount > 1 ? SW_SHOW : SW_HIDE);
}

// The List menu ends with one radio item per watch list.
void App::RebuildListMenu() {
    HMENU menu = GetSubMenu(hMenu_, 3);
    assert(menu != nullptr);
    if (menu == nullptr) { return; }
    // Remove any previous list items (everything after the separator).
    for (int guard = 0; guard < static_cast<int>(kMaxLists) + 1; ++guard) {
        const int count = GetMenuItemCount(menu);
        if (count <= 4) { break; }
        DeleteMenu(menu, static_cast<UINT>(count - 1), MF_BYPOSITION);
    }
    UINT activeId = IDM_LIST_BASE;
    for (size_t i = 0; i < cfg_.listCount; ++i) {
        const std::wstring label = cfg_.lists[i].empty() ? L"Watch list" : cfg_.lists[i];
        const UINT id = IDM_LIST_BASE + static_cast<UINT>(i);
        AppendMenuW(menu, MF_STRING, id, label.c_str());
        if (SameSymbol(cfg_.lists[i], cfg_.listName)) { activeId = id; }
    }
    CheckMenuRadioItem(menu, IDM_LIST_BASE, IDM_LIST_BASE + static_cast<UINT>(kMaxLists), activeId, MF_BYCOMMAND);
    const bool named = !cfg_.listName.empty();
    EnableMenuItem(menu, IDM_LIST_RENAME, MF_BYCOMMAND | (named ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_LIST_DELETE, MF_BYCOMMAND | (named ? MF_ENABLED : MF_GRAYED));
    DrawMenuBar(hwnd_);
}

void App::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    const int m = Px(kMargin);

    int top = m;
    const bool tabs = cfg_.listCount > 1;
    tabsRect_ = { m, top, m + Px(kListW), top + (tabs ? Px(kTabsH) : 0) };
    if (tabs) {
        MoveWindow(hTabs_, tabsRect_.left, tabsRect_.top, tabsRect_.right - tabsRect_.left,
                   tabsRect_.bottom - tabsRect_.top, TRUE);
        top = tabsRect_.bottom + Px(kListBtnGap);
    }
    const bool portfolio = ComputePortfolio().any;
    portfolioRect_ = { m, top, m + Px(kListW), top + (portfolio ? Px(kPortfolioH) : 0) };
    if (portfolio) { top = portfolioRect_.bottom + Px(kListBtnGap); }

    const int listBtnH = Px(kButtonH);
    listRect_ = { m, top, m + Px(kListW), h - m - listBtnH - Px(kListBtnGap) };
    MoveWindow(hList_, listRect_.left, listRect_.top,
               listRect_.right - listRect_.left, listRect_.bottom - listRect_.top, TRUE);
    const int listBtnY = h - m - listBtnH;
    const int listBtnW = (Px(kListW) - 2 * Px(kBtnGap)) / 3;
    MoveWindow(hAddBtn_, listRect_.left, listBtnY, listBtnW, listBtnH, TRUE);
    MoveWindow(hRemoveBtn_, listRect_.left + listBtnW + Px(kBtnGap), listBtnY, listBtnW, listBtnH, TRUE);
    MoveWindow(hReloadBtn_, listRect_.right - listBtnW, listBtnY, listBtnW, listBtnH, TRUE);

    const int rightX = listRect_.right + m;
    const int rightR = w - m;
    const int btnH = Px(kButtonH);

    // Toolbar across the top: the plot toggles.
    toolbarRect_ = { rightX, m, rightR, m + Px(kToolbarH) };
    int tx = rightX;
    for (size_t i = 0; i < kTools.size(); ++i) {
        MoveWindow(hToolBtns_[i], tx, toolbarRect_.top, Px(kTools[i].widthDip), btnH, TRUE);
        tx += Px(kTools[i].widthDip) + Px(kBtnGap);
    }

    headerRect_ = { rightX, toolbarRect_.bottom + Px(kListBtnGap), rightR, toolbarRect_.bottom + Px(kListBtnGap) + Px(kHeaderH) };

    const int btnY = headerRect_.bottom + Px(4);
    int x = rightX;
    for (size_t i = 0; i < kRanges.size(); ++i) {
        MoveWindow(hRangeBtns_[i], x, btnY, Px(kRangeBtnW), btnH, TRUE);
        x += Px(kRangeBtnW) + Px(kBtnGap);
    }
    MoveWindow(hRefreshBtn_, rightR - Px(kWideBtnW + 16), btnY, Px(kWideBtnW + 16), btnH, TRUE);

    statusRect_ = { rightX, h - m - Px(kStatusH), rightR, h - m };
    statsRect_  = { rightX, statusRect_.top - Px(kStatsH), rightR, statusRect_.top };
    const int newsH = (state_.news && fetcher_.NewsEnabled()) ? Px(kNewsH) : 0;
    newsRect_   = { rightX, statsRect_.top - newsH, rightR, statsRect_.top };
    chartRect_  = { rightX, btnY + btnH + m, rightR, newsRect_.top - m };
    assert(chartRect_.bottom >= chartRect_.top);
}

// ---------------------------------------------------------------------------
// Theme

void App::ApplyTheme() {
    const bool dark = (cfg_.theme == ThemeMode::Dark) ||
                      (cfg_.theme == ThemeMode::System && SystemPrefersDark());
    theme_ = &ThemeFor(dark);
    if (hBgBrush_ != nullptr)   { DeleteObject(hBgBrush_); }
    if (hListBrush_ != nullptr) { DeleteObject(hListBrush_); }
    hBgBrush_   = CreateSolidBrush(theme_->bgRef);
    hListBrush_ = CreateSolidBrush(theme_->listBgRef);

    const BOOL useDark = dark ? TRUE : FALSE;
    const HRESULT hr = DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &useDark, sizeof(useDark));
    (void)hr;  // pre-20H1 builds ignore the attribute; the client area is still themed

    const wchar_t* sub = dark ? L"DarkMode_Explorer" : L"Explorer";
    const HWND controls[] = { hList_, hTabs_, hRefreshBtn_, hAddBtn_, hRemoveBtn_, hReloadBtn_ };
    for (HWND h : controls) {
        if (h != nullptr) { SetWindowTheme(h, sub, nullptr); }
    }
    for (HWND h : hRangeBtns_) {
        if (h != nullptr) { SetWindowTheme(h, sub, nullptr); }
    }
    for (HWND h : hToolBtns_) {
        if (h != nullptr) { SetWindowTheme(h, sub, nullptr); }
    }
    chartDirty_ = true;
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void App::SetThemeMode(ThemeMode mode) {
    cfg_.theme = mode;
    const wchar_t* text = (mode == ThemeMode::Light) ? L"light" : (mode == ThemeMode::Dark) ? L"dark" : L"system";
    std::wstring err;
    if (!WriteSetting(cfg_.path, L"theme", text, err)) { SetStatus(err); }
    ApplyTheme();
    SyncViewMenu();
}

void App::SyncViewMenu() {
    assert(hMenu_ != nullptr);
    auto check = [this](int id, bool on) {
        CheckMenuItem(hMenu_, static_cast<UINT>(id), MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    };
    check(IDM_CANDLES,   state_.candles);
    check(IDM_COMPARE,   state_.compare);
    check(IDM_SMA20,     state_.sma20);
    check(IDM_SMA50,     state_.sma50);
    check(IDM_BOLLINGER, state_.bollinger);
    check(IDM_RSI,       state_.rsi);
    check(IDM_INSET,     state_.inset);
    check(IDM_NEWS,      state_.news);
    check(IDM_BENCHMARK, state_.benchmark);
    check(IDM_PORTFOLIO, state_.portfolio);
    EnableMenuItem(hMenu_, IDM_BENCHMARK, MF_BYCOMMAND | (cfg_.benchmark.empty() ? MF_GRAYED : MF_ENABLED));
    check(IDM_MINTRAY,   cfg_.minimizeToTray);
    EnableMenuItem(hMenu_, IDM_NEWS, MF_BYCOMMAND | (fetcher_.NewsEnabled() || hwnd_ == nullptr ? MF_ENABLED : MF_GRAYED));
    const int themeId = (cfg_.theme == ThemeMode::Light) ? IDM_THEME_LIGHT
                      : (cfg_.theme == ThemeMode::Dark)  ? IDM_THEME_DARK : IDM_THEME_SYSTEM;
    CheckMenuRadioItem(hMenu_, IDM_THEME_SYSTEM, IDM_THEME_DARK, static_cast<UINT>(themeId), MF_BYCOMMAND);
    const bool flags[kTools.size()] = {
        state_.candles, state_.compare, state_.sma20, state_.sma50,
        state_.bollinger, state_.rsi, state_.inset, state_.news, state_.benchmark, state_.portfolio,
    };
    for (size_t i = 0; i < kTools.size(); ++i) {
        if (hToolBtns_[i] != nullptr) {
            SendMessageW(hToolBtns_[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
            const bool off = (kTools[i].id == IDM_NEWS && !fetcher_.NewsEnabled() && hwnd_ != nullptr) ||
                             (kTools[i].id == IDM_BENCHMARK && cfg_.benchmark.empty());
            EnableWindow(hToolBtns_[i], off ? FALSE : TRUE);
        }
    }
    DrawMenuBar(hwnd_);
}

void App::ToggleOption(bool& flag) {
    flag = !flag;
    SyncViewMenu();
    RedrawChart();
}

// ---------------------------------------------------------------------------
// Data requests

void App::RequestAllSummaries() {
    lastRefresh_ = UnixNow();   // the off-hours cadence counts from here
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (!fetcher_.Enqueue(JobKind::Summary, i, 0)) { SetStatus(L"Fetch queue is full"); }
    }
}

void App::RequestQuotes() {
    const bool queued = fetcher_.EnqueueQuote();
    (void)queued;  // disabled by config, or already pending
}

void App::RequestChart(bool clearCurrent) {
    if (!HasStocks()) { return; }
    assert(selected_ < cfg_.stockCount);
    assert(range_ < kRanges.size());
    if (clearCurrent) {
        chartValid_[selected_] = false;
        RedrawChart();
    }
    if (!fetcher_.Enqueue(JobKind::Chart, selected_, range_)) { SetStatus(L"Fetch queue is full"); }
    if (state_.compare) {
        for (size_t i = 0; i < cfg_.stockCount; ++i) {
            if (i != selected_) { fetcher_.Enqueue(JobKind::Chart, i, range_); }
        }
    }
}

void App::RequestInset(bool clearCurrent) {
    if (!state_.inset || !HasStocks()) { return; }
    if (clearCurrent) {
        insetValid_[selected_] = false;
        RedrawChart();
    }
    if (!fetcher_.Enqueue(JobKind::Inset, selected_, cfg_.insetRange)) { SetStatus(L"Fetch queue is full"); }
}

void App::RequestNews(bool clearCurrent) {
    if (!state_.news || !fetcher_.NewsEnabled() || !HasStocks()) { return; }
    if (clearCurrent) {
        (*news_)[selected_] = NewsCache{};
        InvalidateRect(hwnd_, &newsRect_, FALSE);
    }
    fetcher_.Enqueue(JobKind::News, selected_, 0);
}

void App::RequestBench() {
    if (!(state_.benchmark || state_.portfolio) || cfg_.benchmark.empty()) { return; }
    fetcher_.EnqueueBench(range_);
}

// Warms the caches for every other ticker so switching is instant. Runs
// after the selected ticker's own data has arrived, so it never delays it.
void App::PrefetchOthers() {
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (i == selected_) { continue; }
        if (!chartValid_[i]) { fetcher_.Enqueue(JobKind::Chart, i, range_); }
        if (state_.inset && !insetValid_[i]) { fetcher_.Enqueue(JobKind::Inset, i, cfg_.insetRange); }
    }
}

// Queues FX pairs for every holding whose currency differs from the
// portfolio currency and has no rate yet.
void App::EnsureFxRates() {
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (PositionOf(i).qty <= 0.0) { continue; }
        const std::wstring cur = CurrencyOf(i);
        if (cur.empty() || SameSymbol(cur, cfg_.portfolioCurrency)) { continue; }
        if (std::isnan(RateToPortfolio(cur))) { fetcher_.EnqueueFx(cur, cfg_.portfolioCurrency); }
    }
}

void App::SelectStock(size_t index) {
    if (index >= cfg_.stockCount) { return; }
    selected_ = index;
    if (static_cast<size_t>(SendMessageW(hList_, LB_GETCURSEL, 0, 0)) != index) {
        SendMessageW(hList_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
    // Cached data shows immediately; a silent refresh is queued behind it.
    RequestChart(!chartValid_[index]);
    RequestInset(!insetValid_[index]);
    RequestNews(!(*news_)[index].valid);
    RedrawAll();
}

void App::SelectRange(size_t index) {
    if (index >= kRanges.size()) { return; }
    range_ = index;
    SendMessageW(hRangeBtns_[index], BM_SETCHECK, BST_CHECKED, 0);
    for (size_t i = 0; i < kMaxStocks; ++i) { chartValid_[i] = false; }
    benchValid_ = false;
    RequestChart(true);
    RequestBench();
}

void App::RedrawChart() {
    chartDirty_ = true;
    InvalidateRect(hwnd_, &chartRect_, FALSE);
}

void App::RedrawAll() {
    chartDirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::InvalidateListItem(size_t index) {
    RECT rc{};
    if (SendMessageW(hList_, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&rc)) != LB_ERR) {
        InvalidateRect(hList_, &rc, FALSE);   // the item paints its own background
    }
}

void App::SetStatus(const std::wstring& text) {
    status_ = text;
    InvalidateRect(hwnd_, &statusRect_, FALSE);
}

// ---------------------------------------------------------------------------
// Derived figures

App::PriceChange App::ComputeChange(const QuoteData& q) const {
    PriceChange pc;
    if (!q.valid) { return pc; }
    const Series& s = q.series;
    const QuoteMeta& m = q.meta;
    if (s.count == 0 && m.price <= 0.0) { return pc; }

    pc.last = (m.price > 0.0) ? m.price : s.pts[s.count - 1].close;
    if (s.count >= 1) {
        // If the newest bar is today's (partial) bar, the previous close is
        // the bar before it; otherwise the newest bar *is* the previous close.
        const bool lastIsToday = (m.marketTime > 0) &&
            LocalDay(s.pts[s.count - 1].time, m.gmtOffsetSec) == LocalDay(m.marketTime, m.gmtOffsetSec);
        if (lastIsToday) { pc.prev = (s.count >= 2) ? s.pts[s.count - 2].close : m.chartPrevClose; }
        else             { pc.prev = s.pts[s.count - 1].close; }
    } else {
        pc.prev = m.chartPrevClose;
    }
    if (pc.prev <= 0.0 || pc.last <= 0.0) { return pc; }
    pc.change = pc.last - pc.prev;
    pc.pct    = pc.change / pc.prev * 100.0;
    pc.valid  = true;
    return pc;
}

std::wstring App::CurrencyOf(size_t index) const {
    assert(index < cfg_.stockCount);
    const StockEntry& e = cfg_.stocks[index];
    if (!e.currency.empty()) { return e.currency; }
    return (*summaries_)[index].meta.currency;
}

double App::RateToPortfolio(const std::wstring& cur) const {
    if (cur.empty() || SameSymbol(cur, cfg_.portfolioCurrency)) { return 1.0; }
    for (const FxRate& r : fx_) {
        if (r.valid && SameSymbol(r.from, cur) && SameSymbol(r.to, cfg_.portfolioCurrency)) { return r.rate; }
    }
    return kNaN;
}

// Dividends per share over the last 365 days, from the (5Y/1Y) inset data.
double App::TrailingDividends(size_t index) const {
    assert(index < cfg_.stockCount);
    if (!insetValid_[index]) { return kNaN; }
    const QuoteData& q = (*insets_)[index];
    if (!q.valid) { return kNaN; }
    const int64_t since = UnixNow() - 365 * 86400;
    double total = 0.0;
    for (size_t k = 0; k < q.dividendCount && k < kMaxDividends; ++k) {
        if (q.dividends[k].time >= since) { total += q.dividends[k].amount; }
    }
    return total;
}

Position App::PositionOf(size_t index) const {
    assert(index < cfg_.stockCount);
    const StockEntry& e = cfg_.stocks[index];
    if (e.txCount > 0) { return ComputePosition(e.tx.data(), e.txCount); }
    Position p;
    p.qty      = e.holding.qty;
    p.acb      = e.holding.cost;
    p.cost     = e.holding.qty * e.holding.cost;
    p.invested = p.cost;
    return p;
}

double App::DividendsReceivedFor(size_t index) const {
    assert(index < cfg_.stockCount);
    const StockEntry& e = cfg_.stocks[index];
    if (e.txCount == 0 || !insetValid_[index] || !(*insets_)[index].valid) { return kNaN; }
    const QuoteData& q = (*insets_)[index];
    return DividendsReceived(e.tx.data(), e.txCount, q.dividends.data(), q.dividendCount);
}

App::Portfolio App::ComputePortfolio() const {
    Portfolio p;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        const Position pos = PositionOf(i);
        const bool hasTx = cfg_.stocks[i].txCount > 0;
        if (pos.qty <= 0.0 && !(hasTx && pos.realised != 0.0)) { continue; }
        p.any = true;
        const QuoteData& q = (*summaries_)[i];
        const PriceChange pc = ComputeChange(q);
        if (!pc.valid) {
            if (pos.qty > 0.0) { p.incomplete = true; }
            continue;
        }
        const double rate = RateToPortfolio(CurrencyOf(i));
        if (std::isnan(rate)) {
            p.partial = true;
            continue;
        }
        p.value    += pos.qty * pc.last * rate;
        p.cost     += pos.cost * rate;
        p.day      += pos.qty * pc.change * rate;
        p.realised += pos.realised * rate;
        const double div = TrailingDividends(i);
        if (!std::isnan(div)) { p.income += pos.qty * div * rate; }
        const double rec = DividendsReceivedFor(i);
        if (!std::isnan(rec)) { p.received += rec * rate; }
    }
    return p;
}

void App::CheckAlerts(size_t stock, const PriceChange& pc) {
    assert(stock < cfg_.stockCount);
    if (!pc.valid) { return; }
    const StockEntry& e = cfg_.stocks[stock];
    AlertState& st = alerts_[stock];
    if (e.alert.above > 0.0) {
        const bool hit = pc.last >= e.alert.above;
        if (hit && !st.aboveActive) {
            TrayBalloon(e.symbol + L" alert", e.symbol + L" is at " + FormatPrice(pc.last) +
                                              L", above " + FormatPrice(e.alert.above));
            SetStatus(e.symbol + L" rose above " + FormatPrice(e.alert.above));
        }
        st.aboveActive = hit;
    } else {
        st.aboveActive = false;
    }
    if (e.alert.below > 0.0) {
        const bool hit = pc.last <= e.alert.below;
        if (hit && !st.belowActive) {
            TrayBalloon(e.symbol + L" alert", e.symbol + L" is at " + FormatPrice(pc.last) +
                                              L", below " + FormatPrice(e.alert.below));
            SetStatus(e.symbol + L" fell below " + FormatPrice(e.alert.below));
        }
        st.belowActive = hit;
    } else {
        st.belowActive = false;
    }
}

// ---------------------------------------------------------------------------
// Tray

void App::UpdateTrayTip() {
    if (!trayAdded_) { return; }
    std::wstring tip = kWindowTitle;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        const PriceChange pc = ComputeChange((*summaries_)[i]);
        if (!pc.valid) { continue; }
        const std::wstring line = L"\n" + cfg_.stocks[i].symbol + L"  " + FormatPrice(pc.last) + L"  " + FormatPct(pc.pct);
        if (tip.size() + line.size() >= 127) { break; }  // szTip is 128 wchar_t
        tip += line;
    }
    wcscpy_s(tray_.szTip, tip.c_str());
    tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
}

void App::TrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!trayAdded_) { return; }
    NOTIFYICONDATAW n = tray_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void App::ShowFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) { ShowWindow(hwnd_, SW_RESTORE); }
    SetForegroundWindow(hwnd_);
}

void App::OnTray(LPARAM lp, WPARAM wp) {
    const UINT event = LOWORD(lp);
    if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP || event == NIN_BALLOONUSERCLICK) {
        ShowFromTray();
        return;
    }
    if (event != WM_CONTEXTMENU && event != WM_RBUTTONUP) { return; }
    HMENU menu = CreatePopupMenu();
    assert(menu != nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"&Show StockTool");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"&Refresh now");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"E&xit");
    SetForegroundWindow(hwnd_);  // required for the menu to dismiss properly
    const int x = GET_X_LPARAM(wp);
    const int y = GET_Y_LPARAM(wp);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Ticker commands

void App::OnAddTicker() {
    if (cfg_.stockCount >= kMaxStocks) {
        SetStatus(L"Limit of " + std::to_wstring(kMaxStocks) + L" tickers reached");
        return;
    }
    // The dialog stays open until closed; each Add calls back into AddTicker.
    const size_t added = RunAddTickerDialog(hInst_, hwnd_, cfg_, fetcher_,
        [this](const StockEntry& entry, std::wstring& err) { return AddTicker(entry, err); });
    if (added > 1) { SetStatus(L"Added " + std::to_wstring(added) + L" tickers"); }
}

bool App::AddTicker(const StockEntry& entry, std::wstring& err) {
    assert(!entry.symbol.empty());
    if (cfg_.stockCount >= kMaxStocks) {
        err = L"Limit of " + std::to_wstring(kMaxStocks) + L" tickers reached";
        return false;
    }
    if (!WriteStockEntry(cfg_, entry, err)) { return false; }
    const size_t index = cfg_.stockCount;
    cfg_.stocks[index] = entry;
    ++cfg_.stockCount;
    (*summaries_)[index] = QuoteData{};
    chartValid_[index]   = false;
    insetValid_[index]   = false;
    (*news_)[index]      = NewsCache{};
    alerts_[index]       = AlertState{};
    SendMessageW(hList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.symbol.c_str()));

    fetcher_.UpdateConfig(cfg_);   // drops pending jobs, so re-request everything
    RequestAllSummaries();
    RequestQuotes();
    SelectStock(index);
    SetStatus(L"Added " + entry.symbol);
    assert(cfg_.stockCount <= kMaxStocks);
    return true;
}

void App::OnEditTicker() {
    if (!HasStocks()) { return; }
    const size_t index = selected_;
    StockEntry edited = cfg_.stocks[index];
    if (!RunEditTickerDialog(hInst_, hwnd_, cfg_, fetcher_, index, edited)) { return; }
    std::wstring err;
    if (!ApplyTickerEdit(index, edited, err)) { SetStatus(err); }
}

// Renames/relabels cfg_.stocks[index] in the file and in memory. Holding and
// alert settings follow the ticker; list order is preserved.
bool App::ApplyTickerEdit(size_t index, const StockEntry& edited, std::wstring& err) {
    assert(index < cfg_.stockCount);
    assert(!edited.symbol.empty());
    const StockEntry old = cfg_.stocks[index];
    const bool symbolChanged = !SameSymbol(old.symbol, edited.symbol);
    const bool changed = symbolChanged || old.name != edited.name || old.currency != edited.currency;
    if (!changed) { return true; }

    StockEntry& e = cfg_.stocks[index];
    e.symbol   = edited.symbol;
    e.name     = edited.name;
    e.currency = edited.currency;
    bool ok = true;
    if (symbolChanged) {
        // Old keys go, then the section is rewritten in order with the new
        // symbol, then holding/alert/currency are re-keyed.
        ok = DeleteStockEntry(cfg_, old.symbol, err) &&
             WriteStockOrder(cfg_, err) &&
             WriteHolding(cfg_.path, e.symbol, e.holding, err) &&
             WriteAlert(cfg_.path, e.symbol, e.alert, err) &&
             WriteCurrency(cfg_.path, e.symbol, e.currency, err);
    } else {
        // Name/currency only: WriteStockEntry never clears a currency
        // override, so write it explicitly (an empty code deletes the key).
        ok = WriteStockEntry(cfg_, e, err) && WriteCurrency(cfg_.path, e.symbol, e.currency, err);
    }
    if (!ok) {
        cfg_.stocks[index] = old;   // keep memory and file consistent
        return false;
    }

    if (symbolChanged) {
        (*summaries_)[index] = QuoteData{};
        chartValid_[index]   = false;
        insetValid_[index]   = false;
        (*news_)[index]      = NewsCache{};
        alerts_[index]       = AlertState{};
    }
    RebuildList();
    fetcher_.UpdateConfig(cfg_);
    RequestAllSummaries();
    RequestQuotes();
    EnsureFxRates();
    SelectStock(index);
    Layout();
    RedrawAll();
    SetStatus(symbolChanged ? old.symbol + L" is now " + e.symbol : L"Updated " + e.symbol);
    return true;
}

void App::OnRemoveTicker() {
    if (!HasStocks()) { return; }
    assert(selected_ < cfg_.stockCount);
    if (cfg_.stockCount <= 1 && cfg_.listName.empty()) {
        SetStatus(L"Keep at least one ticker in the default list");
        return;
    }
    const size_t       index  = selected_;
    const std::wstring symbol = cfg_.stocks[index].symbol;
    std::wstring err;
    if (!DeleteStockEntry(cfg_, symbol, err)) {
        SetStatus(err);
        return;
    }
    // Shift the tail down by one (bounded by kMaxStocks).
    for (size_t i = index; i + 1 < cfg_.stockCount && i + 1 < kMaxStocks; ++i) {
        cfg_.stocks[i]     = cfg_.stocks[i + 1];
        (*summaries_)[i]   = (*summaries_)[i + 1];
        (*charts_)[i]      = (*charts_)[i + 1];
        chartValid_[i]     = chartValid_[i + 1];
        (*insets_)[i]      = (*insets_)[i + 1];
        insetValid_[i]     = insetValid_[i + 1];
        (*news_)[i]        = (*news_)[i + 1];
        alerts_[i]         = alerts_[i + 1];
    }
    --cfg_.stockCount;
    cfg_.stocks[cfg_.stockCount]   = StockEntry{};
    (*summaries_)[cfg_.stockCount] = QuoteData{};
    chartValid_[cfg_.stockCount]   = false;
    insetValid_[cfg_.stockCount]   = false;
    (*news_)[cfg_.stockCount]      = NewsCache{};
    SendMessageW(hList_, LB_DELETESTRING, static_cast<WPARAM>(index), 0);

    fetcher_.UpdateConfig(cfg_);
    RequestAllSummaries();
    RequestQuotes();
    if (HasStocks()) { SelectStock((index < cfg_.stockCount) ? index : cfg_.stockCount - 1); }
    else             { selected_ = 0; }
    Layout();
    RedrawAll();
    SetStatus(L"Removed " + symbol);
}

void App::OnMoveTicker(int delta) {
    assert(delta == 1 || delta == -1);
    if (!HasStocks()) { return; }
    const size_t i = selected_;
    if ((delta < 0 && i == 0) || (delta > 0 && i + 1 >= cfg_.stockCount)) { return; }
    const size_t j = (delta < 0) ? i - 1 : i + 1;
    std::swap(cfg_.stocks[i], cfg_.stocks[j]);
    std::wstring err;
    if (!WriteStockOrder(cfg_, err)) {
        std::swap(cfg_.stocks[i], cfg_.stocks[j]);
        SetStatus(err);
        return;
    }
    std::swap((*summaries_)[i], (*summaries_)[j]);
    std::swap((*charts_)[i], (*charts_)[j]);
    std::swap(chartValid_[i], chartValid_[j]);
    std::swap((*insets_)[i], (*insets_)[j]);
    std::swap(insetValid_[i], insetValid_[j]);
    std::swap((*news_)[i], (*news_)[j]);
    std::swap(alerts_[i], alerts_[j]);
    selected_ = j;
    RebuildList();
    fetcher_.UpdateConfig(cfg_);
    RequestAllSummaries();
    SelectStock(j);
}

void App::OnEditHolding() {
    if (!HasStocks()) { return; }
    StockEntry& e = cfg_.stocks[selected_];
    Holding h = e.holding;
    if (!RunHoldingDialog(hInst_, hwnd_, e.symbol, h)) { return; }
    std::wstring err;
    if (!WriteHolding(cfg_.path, e.symbol, h, err)) {
        SetStatus(err);
        return;
    }
    e.holding = h;
    fetcher_.UpdateConfig(cfg_);
    EnsureFxRates();
    Layout();
    RedrawAll();
    SetStatus((h.qty > 0.0) ? L"Holding saved for " + e.symbol : L"Holding cleared for " + e.symbol);
}

void App::OnEditAlerts() {
    if (!HasStocks()) { return; }
    StockEntry& e = cfg_.stocks[selected_];
    Alert a = e.alert;
    if (!RunAlertsDialog(hInst_, hwnd_, e.symbol, a)) { return; }
    std::wstring err;
    if (!WriteAlert(cfg_.path, e.symbol, a, err)) {
        SetStatus(err);
        return;
    }
    e.alert = a;
    alerts_[selected_] = AlertState{};
    fetcher_.UpdateConfig(cfg_);
    CheckAlerts(selected_, ComputeChange((*summaries_)[selected_]));
    InvalidateListItem(selected_);
    SetStatus(L"Alerts saved for " + e.symbol);
}

// Swaps in a freshly loaded config: every per-ticker cache starts over.
void App::ApplyConfig(const Config& fresh) {
    const std::wstring current = HasStocks() ? cfg_.stocks[selected_].symbol : L"";
    size_t newSel = 0;
    for (size_t i = 0; i < fresh.stockCount; ++i) {
        if (SameSymbol(fresh.stocks[i].symbol, current)) { newSel = i; }
    }
    cfg_ = fresh;
    for (size_t i = 0; i < kMaxStocks; ++i) {
        (*summaries_)[i] = QuoteData{};
        chartValid_[i]   = false;
        insetValid_[i]   = false;
        (*news_)[i]      = NewsCache{};
        alerts_[i]       = AlertState{};
    }
    quoteCount_ = 0;
    selected_   = newSel;
    RebuildList();
    RebuildTabs();
    RebuildListMenu();
    KillTimer(hwnd_, kRefreshTimer);
    SetTimer(hwnd_, kRefreshTimer, cfg_.refreshSeconds * 1000u, nullptr);

    fetcher_.UpdateConfig(cfg_);
    LoadPortfolioHistory();
    ApplyTheme();
    SyncViewMenu();
    Layout();
    RequestAllSummaries();
    RequestQuotes();
    if (HasStocks()) { SelectStock(newSel); }
    else             { RedrawAll(); }
}

// Re-reads stocktool.cfg (tickers, refresh interval, URL templates) without
// restarting. Keeps the current selection if its symbol is still listed.
void App::OnReloadConfig() {
    Config fresh;
    std::wstring err;
    if (!LoadConfig(cfg_.path, cfg_.listName, fresh, err)) {
        SetStatus(L"Reload failed: " + err);   // keep running on the old config
        return;
    }
    ApplyConfig(fresh);
    SetStatus(L"Reloaded " + std::to_wstring(cfg_.stockCount) + L" tickers from " + cfg_.path);
}

// ---------------------------------------------------------------------------
// Watch lists

void App::SwitchList(const std::wstring& name) {
    if (SameSymbol(name, cfg_.listName)) { return; }
    Config fresh;
    std::wstring err;
    if (!LoadConfig(cfg_.path, name, fresh, err)) {
        SetStatus(L"Cannot open list: " + err);
        return;
    }
    ApplyConfig(fresh);
    SaveState();
    SetStatus(L"Watch list: " + (cfg_.listName.empty() ? L"Watch list" : cfg_.listName));
}

void App::OnNewList() {
    if (cfg_.listCount >= kMaxLists) {
        SetStatus(L"Limit of " + std::to_wstring(kMaxLists) + L" watch lists reached");
        return;
    }
    std::wstring name;
    if (!RunListNameDialog(hInst_, hwnd_, L"Name for the new watch list:", name)) { return; }
    for (size_t i = 0; i < cfg_.listCount; ++i) {
        if (SameSymbol(cfg_.lists[i], name)) {
            SetStatus(L"A list called " + name + L" already exists");
            return;
        }
    }
    std::wstring err;
    if (!CreateList(cfg_.path, name, err)) {
        SetStatus(err);
        return;
    }
    SwitchList(name);
}

void App::OnRenameList() {
    if (cfg_.listName.empty()) {
        SetStatus(L"The default watch list cannot be renamed");
        return;
    }
    std::wstring name = cfg_.listName;
    if (!RunListNameDialog(hInst_, hwnd_, L"New name for " + cfg_.listName + L":", name)) { return; }
    if (SameSymbol(name, cfg_.listName)) { return; }
    for (size_t i = 0; i < cfg_.listCount; ++i) {
        if (SameSymbol(cfg_.lists[i], name)) {
            SetStatus(L"A list called " + name + L" already exists");
            return;
        }
    }
    std::wstring err;
    if (!RenameList(cfg_.path, cfg_.listName, name, err)) {
        SetStatus(err);
        return;
    }
    Config fresh;
    if (!LoadConfig(cfg_.path, name, fresh, err)) {
        SetStatus(err);
        return;
    }
    ApplyConfig(fresh);
    SaveState();
    SetStatus(L"Renamed list to " + name);
}

void App::OnDeleteList() {
    if (cfg_.listName.empty()) {
        SetStatus(L"The default watch list cannot be deleted");
        return;
    }
    const std::wstring text = L"Delete the watch list \"" + cfg_.listName + L"\" and its " +
                              std::to_wstring(cfg_.stockCount) + L" ticker(s)?";
    if (MessageBoxW(hwnd_, text.c_str(), L"Delete list", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) { return; }
    std::wstring err;
    const std::wstring gone = cfg_.listName;
    if (!DeleteList(cfg_.path, gone, err)) {
        SetStatus(err);
        return;
    }
    Config fresh;
    if (!LoadConfig(cfg_.path, L"", fresh, err)) {
        SetStatus(err);
        return;
    }
    ApplyConfig(fresh);
    SaveState();
    SetStatus(L"Deleted list " + gone);
}

// ---------------------------------------------------------------------------
// CSV export

bool App::SaveCsvDialog(const wchar_t* suggested, std::wstring& path) {
    std::array<wchar_t, MAX_PATH> file{};
    wcscpy_s(file.data(), file.size(), suggested);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file.data();
    ofn.nMaxFile    = static_cast<DWORD>(file.size());
    ofn.lpstrDefExt = L"csv";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) { return false; }
    path = file.data();
    return !path.empty();
}

bool App::OpenCsvDialog(std::wstring& path) {
    std::array<wchar_t, MAX_PATH> file{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file.data();
    ofn.nMaxFile    = static_cast<DWORD>(file.size());
    ofn.lpstrTitle  = L"Import a WebBroker Holdings or Activity export";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) { return false; }
    path = file.data();
    return !path.empty();
}

bool App::ReadWholeFile(const std::wstring& path, std::string& bytes, std::wstring& err) {
    constexpr DWORD kMaxBytes = 8u * 1024u * 1024u;   // an export is a few hundred KB at most
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"Cannot open " + path;
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > kMaxBytes) {
        CloseHandle(h);
        err = L"File is too large to be a broker export: " + path;
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL ok = bytes.empty() || ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != bytes.size()) {
        err = L"Cannot read " + path;
        return false;
    }
    return true;
}

bool App::WriteTextFile(const std::wstring& path, const std::string& utf8, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"Cannot create " + path;
        return false;
    }
    static const char kBom[] = "\xEF\xBB\xBF";   // so Excel reads UTF-8
    DWORD written = 0;
    const BOOL ok = WriteFile(h, kBom, 3, &written, nullptr) &&
                    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok) {
        err = L"Cannot write " + path;
        return false;
    }
    return true;
}

void App::OnExportList() {
    std::wstring path;
    if (!SaveCsvDialog(L"watchlist.csv", path)) { return; }
    std::string csv = "Symbol,Name,Currency,Last,Change,Change%,PrevClose,Shares,AvgCost,Value,52wHigh,52wLow\r\n";
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        const StockEntry& e  = cfg_.stocks[i];
        const QuoteData&  q  = (*summaries_)[i];
        const PriceChange pc = ComputeChange(q);
        csv += CsvField(e.symbol) + "," + CsvField(e.name) + "," + CsvField(CurrencyOf(i)) + ",";
        if (pc.valid) {
            csv += CsvNum(pc.last, 2) + "," + CsvNum(pc.change, 2) + "," + CsvNum(pc.pct, 2) + "," + CsvNum(pc.prev, 2);
        } else {
            csv += ",,,";
        }
        const Position pos = PositionOf(i);
        csv += "," + (pos.qty > 0.0 ? CsvNum(pos.qty, 4) : "") +
               "," + (pos.acb > 0.0 ? CsvNum(pos.acb, 4) : "") +
               "," + (pos.qty > 0.0 && pc.valid ? CsvNum(pos.qty * pc.last, 2) : "") +
               "," + (q.valid && q.meta.wk52High > 0.0 ? CsvNum(q.meta.wk52High, 2) : "") +
               "," + (q.valid && q.meta.wk52Low > 0.0 ? CsvNum(q.meta.wk52Low, 2) : "") + "\r\n";
    }
    std::wstring err;
    if (!WriteTextFile(path, csv, err)) { SetStatus(err); return; }
    SetStatus(L"Exported " + std::to_wstring(cfg_.stockCount) + L" rows to " + path);
}

void App::OnExportChart() {
    if (!HasStocks() || !chartValid_[selected_] || !(*charts_)[selected_].valid) {
        SetStatus(L"No chart data to export yet");
        return;
    }
    const QuoteData& q = (*charts_)[selected_];
    const std::wstring suggested = cfg_.stocks[selected_].symbol + L"-" + kRanges[range_].label + L".csv";
    std::wstring path;
    if (!SaveCsvDialog(suggested.c_str(), path)) { return; }
    const DateStyle ds = kRanges[range_].intraday ? DateStyle::FullTime : DateStyle::Full;
    std::string csv = "Date,Open,High,Low,Close,Volume,Dividend\r\n";
    for (size_t i = 0; i < q.series.count; ++i) {
        const Candle& c = q.series.pts[i];
        double div = 0.0;
        for (size_t k = 0; k < q.dividendCount; ++k) {
            const int64_t next = (i + 1 < q.series.count) ? q.series.pts[i + 1].time : INT64_MAX;
            if (q.dividends[k].time >= c.time && q.dividends[k].time < next) { div += q.dividends[k].amount; }
        }
        csv += CsvField(FormatDate(c.time, q.meta.gmtOffsetSec, ds)) + "," + CsvNum(c.open, 4) + "," +
               CsvNum(c.high, 4) + "," + CsvNum(c.low, 4) + "," + CsvNum(c.close, 4) + "," +
               CsvNum(c.volume, 0) + "," + (div > 0.0 ? CsvNum(div, 4) : "") + "\r\n";
    }
    std::wstring err;
    if (!WriteTextFile(path, csv, err)) { SetStatus(err); return; }
    SetStatus(L"Exported " + std::to_wstring(q.series.count) + L" bars to " + path);
}

// File > Import from TD: a WebBroker Holdings or Activity CSV becomes
// [holdings] or [transactions] lines, after a summary the user confirms.
void App::OnImportBroker() {
    std::wstring path;
    if (!OpenCsvDialog(path)) { return; }
    std::string bytes;
    std::wstring err;
    if (!ReadWholeFile(path, bytes, err)) { SetStatus(err); return; }
    auto result = std::make_unique<ImportResult>();
    if (!ParseBrokerCsv(DecodeTextFile(bytes), *result, err)) {
        MessageBoxW(hwnd_, err.c_str(), L"Import from TD", MB_OK | MB_ICONWARNING);
        return;
    }
    auto plan = std::make_unique<ImportPlan>();
    BuildImportPlan(*result, cfg_.path, *plan);
    if (plan->count == 0) {
        MessageBoxW(hwnd_, L"No buys, sells or positions were found in that file.", L"Import from TD", MB_OK | MB_ICONINFORMATION);
        return;
    }
    bool addUnknown = false;
    const std::wstring title = L"From " + path;
    if (!RunImportDialog(hInst_, hwnd_, title, DescribeImportPlan(*plan), addUnknown)) { return; }
    size_t written = 0;
    if (!ApplyImportPlan(*plan, cfg_, addUnknown, written, err)) {
        SetStatus(L"Import failed: " + err);
        OnReloadConfig();          // pick up whatever was written before the failure
        return;
    }
    OnReloadConfig();
    SetStatus(L"Imported " + std::to_wstring(written) + L" symbol" + (written == 1 ? L"" : L"s") +
              (plan->holdings ? L" (holdings)" : L" (transactions)") + L" from " + path);
}

// ---------------------------------------------------------------------------
// Message handlers

void App::OnSize(WPARAM wp) {
    if (wp == SIZE_MINIMIZED) {
        if (cfg_.minimizeToTray) { ShowWindow(hwnd_, SW_HIDE); }
        return;
    }
    Layout();
    RedrawAll();
}

LRESULT App::OnNotify(const NMHDR* hdr) {
    assert(hdr != nullptr);
    if (hdr->hwndFrom == hTabs_ && hdr->code == TCN_SELCHANGE) {
        const int sel = TabCtrl_GetCurSel(hTabs_);
        if (sel >= 0 && static_cast<size_t>(sel) < cfg_.listCount) { SwitchList(cfg_.lists[static_cast<size_t>(sel)]); }
        return 0;
    }
    return 0;
}

void App::OnCommand(int id, UINT code) {
    if (id == IDC_LIST && code == LBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(hList_, LB_GETCURSEL, 0, 0);
        if (sel != LB_ERR) { SelectStock(static_cast<size_t>(sel)); }
        return;
    }
    if (id == IDC_LIST && code == LBN_DBLCLK) {
        OnEditTicker();
        return;
    }
    if (id >= IDC_RANGE_BASE && id < IDC_RANGE_BASE + static_cast<int>(kRanges.size()) && code == BN_CLICKED) {
        SelectRange(static_cast<size_t>(id - IDC_RANGE_BASE));
        return;
    }
    if (id >= IDM_RANGE_BASE && id < IDM_RANGE_BASE + static_cast<int>(kRanges.size())) {
        SelectRange(static_cast<size_t>(id - IDM_RANGE_BASE));
        return;
    }
    if (id >= IDM_LIST_BASE && id < IDM_LIST_BASE + static_cast<int>(kMaxLists)) {
        const size_t i = static_cast<size_t>(id - IDM_LIST_BASE);
        if (i < cfg_.listCount) { SwitchList(cfg_.lists[i]); }
        return;
    }
    switch (id) {
    case IDM_CANDLES:   ToggleOption(state_.candles); break;
    case IDM_COMPARE:   ToggleOption(state_.compare); RequestChart(false); break;
    case IDM_SMA20:     ToggleOption(state_.sma20); break;
    case IDM_SMA50:     ToggleOption(state_.sma50); break;
    case IDM_BOLLINGER: ToggleOption(state_.bollinger); break;
    case IDM_RSI:       ToggleOption(state_.rsi); break;
    case IDM_INSET:     ToggleOption(state_.inset); RequestInset(false); break;
    case IDM_NEWS:      ToggleOption(state_.news); Layout(); RequestNews(false); RedrawAll(); break;
    case IDM_BENCHMARK: ToggleOption(state_.benchmark); RequestBench(); break;
    case IDM_PORTFOLIO: ToggleOption(state_.portfolio); RequestBench(); break;
    case IDM_TRANSACTIONS: OnEditTransactions(); break;
    case IDM_HEALTH:    OnShowHealth(); break;
    case IDM_THEME_SYSTEM: SetThemeMode(ThemeMode::System); break;
    case IDM_THEME_LIGHT:  SetThemeMode(ThemeMode::Light); break;
    case IDM_THEME_DARK:   SetThemeMode(ThemeMode::Dark); break;
    case IDM_MINTRAY: {
        cfg_.minimizeToTray = !cfg_.minimizeToTray;
        std::wstring err;
        if (!WriteSetting(cfg_.path, L"minimize_to_tray", cfg_.minimizeToTray ? L"1" : L"0", err)) { SetStatus(err); }
        SyncViewMenu();
        break;
    }
    case IDC_REFRESH:
    case IDM_REFRESH:   Refresh(); break;
    case IDC_RELOAD:
    case IDM_RELOAD:    OnReloadConfig(); break;
    case IDM_EXPORT_LIST:  OnExportList(); break;
    case IDM_EXPORT_CHART: OnExportChart(); break;
    case IDM_IMPORT_BROKER: OnImportBroker(); break;
    case IDC_ADD:
    case IDM_ADD:       OnAddTicker(); break;
    case IDM_EDIT:      OnEditTicker(); break;
    case IDC_REMOVE:
    case IDM_REMOVE:    OnRemoveTicker(); break;
    case IDM_MOVEUP:    OnMoveTicker(-1); break;
    case IDM_MOVEDOWN:  OnMoveTicker(+1); break;
    case IDM_HOLDING:   OnEditHolding(); break;
    case IDM_ALERTS:    OnEditAlerts(); break;
    case IDM_LIST_NEW:    OnNewList(); break;
    case IDM_LIST_RENAME: OnRenameList(); break;
    case IDM_LIST_DELETE: OnDeleteList(); break;
    case IDM_TRAY_SHOW: ShowFromTray(); break;
    case IDM_TRAY_EXIT:
    case IDM_EXIT:      PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    case IDM_HELP:      ShowHelpWindow(hInst_, hwnd_, theme_->dark); break;
    case IDM_ABOUT: {
        const std::wstring text = std::wstring(L"StockTool ") + kVersionText +
            L"\n\nWin32 C++ stock watch list and charts.\nData: Yahoo Finance (unofficial endpoints).\n\n"
            L"Config: " + cfg_.path;
        MessageBoxW(hwnd_, text.c_str(), L"About StockTool", MB_ICONINFORMATION | MB_OK);
        break;
    }
    default: break;
    }
}

void App::OnContextMenu(HWND source, int x, int y) {
    if (source != hList_) { return; }
    if (x == -1 && y == -1) {  // keyboard: anchor at the list
        x = listRect_.left + Px(20);
        y = listRect_.top + Px(20);
        ClientToScreen(hwnd_, reinterpret_cast<POINT*>(&x));
    } else {
        POINT p{ x, y };
        ScreenToClient(hList_, &p);
        const LRESULT hit = SendMessageW(hList_, LB_ITEMFROMPOINT, 0, MAKELPARAM(p.x, p.y));
        if (HIWORD(hit) == 0) { SelectStock(static_cast<size_t>(LOWORD(hit))); }
    }
    HMENU ticker = GetSubMenu(hMenu_, 2);
    assert(ticker != nullptr);
    TrackPopupMenu(ticker, TPM_RIGHTBUTTON, x, y, 0, hwnd_, nullptr);
}

// Off-hours the prices cannot change, so the timer only refreshes every
// closedRefreshSeconds (still often enough to notice the next session's
// times) and exactly when a known session opens. F5 always refreshes.
void App::OnTimer() {
    int64_t nextOpen = INT64_MAX;
    if (MarketsClosed(nextOpen)) {
        const int64_t now = UnixNow();
        const bool due = now >= nextOpen || now - lastRefresh_ >= static_cast<int64_t>(cfg_.closedRefreshSeconds);
        if (!due) {
            InvalidateRect(hwnd_, &statusRect_, FALSE);   // countdown in the status line
            return;
        }
    }
    Refresh();
}

bool App::MarketsClosed(int64_t& nextOpen) const {
    const int64_t now = UnixNow();
    nextOpen = INT64_MAX;
    bool known = false;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        const QuoteMeta& m = (*summaries_)[i].meta;
        if (!(*summaries_)[i].valid || m.regularStart <= 0) { continue; }
        known = true;
        if (MarketOpen(m, now)) { return false; }
        if (m.regularStart > now && m.regularStart < nextOpen) { nextOpen = m.regularStart; }
    }
    return known;
}

void App::Refresh() {
    RequestAllSummaries();
    RequestQuotes();
    RequestChart(false);
    RequestInset(false);
    RequestNews(false);
    RequestBench();
    for (size_t i = 0; i < kMaxFx; ++i) {
        if (fx_[i].valid) { fetcher_.EnqueueFx(fx_[i].from, fx_[i].to); }
    }
}

void App::OnMouseMove(int x, int y, bool buttonDown) {
    if (dragging_) {
        if (!buttonDown) {          // released outside the window: treat as a drop
            OnLButtonUp(x, y);
            return;
        }
        const float px = static_cast<float>(x) - dragDX_;
        const float py = static_cast<float>(y) - dragDY_;
        float fx = state_.insetX;
        float fy = state_.insetY;
        ChartInsetFractionFor(ToRectF(chartRect_), BuildChartInput(), scale_, px, py, fx, fy);
        if (fx != state_.insetX || fy != state_.insetY) {
            state_.insetX = fx;
            state_.insetY = fy;
            RedrawChart();
        }
        return;
    }
    if (!tracking_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        tracking_ = (TrackMouseEvent(&tme) != FALSE);
    }
    // The chart is drawn in client coordinates, so hover is client coordinates too.
    const POINT p{ x, y };
    const bool inChart = PtInRect(&chartRect_, p) != FALSE;
    const int nx = inChart ? x : -1;
    const int ny = inChart ? y : -1;
    if (nx != hoverX_ || ny != hoverY_) {
        hoverX_ = nx;
        hoverY_ = ny;
        InvalidateRect(hwnd_, &chartRect_, FALSE);
    }
}

void App::OnMouseLeave() {
    tracking_ = false;
    if (hoverX_ != -1) {
        hoverX_ = -1;
        hoverY_ = -1;
        InvalidateRect(hwnd_, &chartRect_, FALSE);
    }
}

// Headline row under the mouse, or -1.
int App::NewsRowAt(int x, int y) const {
    const POINT p{ x, y };
    if (!state_.news || !HasStocks() || !PtInRect(&newsRect_, p)) { return -1; }
    const NewsCache& nc = (*news_)[selected_];
    const int rowH = Px(kNewsRowH);
    const int top  = newsRect_.top + Px(22);
    if (y < top) { return -1; }
    const int row = (y - top) / rowH;
    if (row < 0 || static_cast<size_t>(row) >= nc.count) { return -1; }
    return row;
}

void App::OnLButtonDown(int x, int y) {
    const int row = NewsRowAt(x, y);
    if (row >= 0) {
        const NewsItem& item = (*news_)[selected_].items[static_cast<size_t>(row)];
        if (!item.link.empty()) {
            // Opens the headline in the default browser (a user-initiated navigation).
            ShellExecuteW(hwnd_, L"open", item.link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    RectF box;
    if (!InsetHit(x, y, box)) { return; }
    dragging_ = true;
    dragDX_   = static_cast<float>(x) - box.X;
    dragDY_   = static_cast<float>(y) - box.Y;
    // Capture keeps moves coming while the cursor is outside the window; it
    // is best effort (refused for a background window) - the drag itself
    // follows the button state in the mouse messages, not the capture.
    SetCapture(hwnd_);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
}

void App::OnLButtonUp(int x, int y) {
    (void)x;
    (void)y;
    if (!dragging_) { return; }
    dragging_ = false;
    if (GetCapture() == hwnd_) { ReleaseCapture(); }
    SaveState();   // remember the new spot right away
    RedrawChart();
}

bool App::OnSetCursor() {
    POINT p{};
    if (!GetCursorPos(&p) || !ScreenToClient(hwnd_, &p)) { return false; }
    if (NewsRowAt(p.x, p.y) >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return true;
    }
    RectF box;
    if (!dragging_ && !InsetHit(p.x, p.y, box)) { return false; }
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
}

void App::OnDpiChanged(WPARAM wp, LPARAM lp) {
    scale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
    assert(scale_ > 0.0f);
    CreateFonts();
    const RECT* r = reinterpret_cast<const RECT*>(lp);
    assert(r != nullptr);
    SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    RedrawAll();
}

void App::OnSettingChange(LPARAM lp) {
    const wchar_t* what = reinterpret_cast<const wchar_t*>(lp);
    if (what != nullptr && cfg_.theme == ThemeMode::System && wcscmp(what, L"ImmersiveColorSet") == 0) {
        ApplyTheme();
    }
}

void App::OnMeasureItem(MEASUREITEMSTRUCT* mis) {
    assert(mis != nullptr);
    if (mis->CtlID == static_cast<UINT>(IDC_LIST)) { mis->itemHeight = static_cast<UINT>(Px(kListItemH)); }
}

void App::OnDrawItem(const DRAWITEMSTRUCT* dis) {
    assert(dis != nullptr);
    if (dis->CtlID != static_cast<UINT>(IDC_LIST) || dis->itemID == static_cast<UINT>(-1)) { return; }
    const size_t index = dis->itemID;
    if (index >= cfg_.stockCount) { return; }
    Graphics g(dis->hDC);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    PaintListItem(g, dis->rcItem, index, (dis->itemState & ODS_SELECTED) != 0);
}

void App::OnSummaryReady(size_t stock) {
    if (stock >= cfg_.stockCount) { return; }
    QuoteData& q = (*summaries_)[stock];
    if (!fetcher_.CopySummary(stock, q)) { return; }
    if (!SameSymbol(q.symbol, cfg_.stocks[stock].symbol)) {
        q = QuoteData{};  // result for a symbol that has since moved or gone
        return;
    }
    if (!q.valid)               { SetStatus(cfg_.stocks[stock].symbol + L": " + q.error); }
    else if (!q.source.empty()) { SetStatus(cfg_.stocks[stock].symbol + L": daily data via " + q.source + L" (" + q.error + L")"); }
    else                        { SetStatus(L"Updated " + TimeNow()); }
    CheckAlerts(stock, ComputeChange(q));
    UpdateTrayTip();
    InvalidateListItem(stock);
    if (PositionOf(stock).qty > 0.0) { EnsureFxRates(); }
    if (ComputePortfolio().any) {
        if (portfolioRect_.bottom == portfolioRect_.top) { Layout(); }
        InvalidateRect(hwnd_, &portfolioRect_, FALSE);
        MaybeRecordHistory();
    }
    if (stock == selected_) {
        InvalidateRect(hwnd_, &headerRect_, FALSE);
        InvalidateRect(hwnd_, &statsRect_, FALSE);
    }
}

void App::OnChartReady(size_t stock, size_t range) {
    if (stock >= cfg_.stockCount || range != range_) { return; }  // stale
    QuoteData& fresh = *incoming_;
    if (!fetcher_.CopyChart(stock, range, fresh)) { return; }
    if (!SameSymbol(fresh.symbol, cfg_.stocks[stock].symbol)) { return; }  // list changed meanwhile
    QuoteData& q = (*charts_)[stock];
    const bool changed = !chartValid_[stock] || !SameBars(q, fresh);
    if (changed) { q = fresh; }
    chartValid_[stock] = true;
    if (stock == selected_ && !q.valid) { SetStatus(cfg_.stocks[stock].symbol + L": " + q.error); }
    if (changed && (stock == selected_ || state_.compare)) { RedrawChart(); }
    if (stock == selected_) { PrefetchOthers(); }
}

void App::OnInsetReady(size_t stock, size_t range) {
    if (stock >= cfg_.stockCount || range != cfg_.insetRange) { return; }
    QuoteData& fresh = *incoming_;
    if (!fetcher_.CopyInset(stock, range, fresh)) { return; }
    if (!SameSymbol(fresh.symbol, cfg_.stocks[stock].symbol)) { return; }
    QuoteData& q = (*insets_)[stock];
    const bool changed = !insetValid_[stock] || !SameBars(q, fresh);
    if (changed) { q = fresh; }
    insetValid_[stock] = true;
    if (stock == selected_) {
        if (changed) {
            RedrawChart();
            InvalidateRect(hwnd_, &headerRect_, FALSE);   // dividend income line
        }
        PrefetchOthers();
    }
    if (PositionOf(stock).qty > 0.0 || cfg_.stocks[stock].txCount > 0) { InvalidateRect(hwnd_, &portfolioRect_, FALSE); }
}

void App::OnQuoteReady() {
    std::wstring err;
    if (!fetcher_.CopyQuotes(*quotes_, quoteCount_, err)) { return; }
    if (!err.empty()) { SetStatus(L"Fundamentals: " + err); }
    InvalidateRect(hwnd_, &statsRect_, FALSE);
}

void App::OnFxReady() {
    if (!fetcher_.CopyFx(fx_)) { return; }
    if (ComputePortfolio().any) {
        if (portfolioRect_.bottom == portfolioRect_.top) { Layout(); }
        InvalidateRect(hwnd_, &portfolioRect_, FALSE);
        MaybeRecordHistory();
    }
    InvalidateRect(hwnd_, &headerRect_, FALSE);
}

void App::OnNewsReady(size_t stock) {
    if (stock >= cfg_.stockCount) { return; }
    NewsCache& nc = (*news_)[stock];
    std::wstring symbol;
    if (!fetcher_.CopyNews(stock, nc.items, nc.count, symbol, nc.error)) { return; }
    if (!SameSymbol(symbol, cfg_.stocks[stock].symbol)) {
        nc = NewsCache{};
        return;
    }
    nc.valid = true;
    if (stock == selected_) { InvalidateRect(hwnd_, &newsRect_, FALSE); }
}

void App::OnBenchReady(size_t range) {
    if (range != range_) { return; }
    QuoteData& fresh = *incoming_;
    if (!fetcher_.CopyBench(range, fresh)) { return; }
    if (!SameSymbol(fresh.symbol, cfg_.benchmark)) { return; }
    const bool changed = !benchValid_ || !SameBars(*bench_, fresh);
    if (changed) { *bench_ = fresh; }
    benchValid_ = true;
    if (changed && state_.benchmark) { RedrawChart(); }
}

void App::OnBackoff(size_t seconds) {
    SetStatus(L"The data provider is rate limiting us (HTTP 429); pausing fetches for " +
              std::to_wstring(seconds / 60) + L" min");
}

void App::OnEditTransactions() {
    if (!HasStocks()) { return; }
    StockEntry& e = cfg_.stocks[selected_];
    std::array<Transaction, kMaxTxPerSymbol> tx = e.tx;
    size_t count = e.txCount;
    if (!RunTransactionsDialog(hInst_, hwnd_, e.symbol, tx, count)) { return; }
    std::wstring err;
    if (!WriteTransactions(cfg_.path, e.symbol, tx.data(), count, err)) {
        SetStatus(err);
        return;
    }
    e.tx      = tx;
    e.txCount = count;
    fetcher_.UpdateConfig(cfg_);
    EnsureFxRates();
    if (count > 0 && !insetValid_[selected_]) { RequestInset(false); }   // for dividends received
    Layout();
    RedrawAll();
    SetStatus(std::to_wstring(count) + L" transaction(s) saved for " + e.symbol);
}

std::wstring App::HealthText() {
    Fetcher::Health h;
    fetcher_.CopyHealth(h);
    const int64_t now = UnixNow();
    auto ago = [now](int64_t t) -> std::wstring {
        if (t <= 0) { return L"never"; }
        const int64_t d = now - t;
        if (d < 60) { return std::to_wstring(d) + L" s ago"; }
        if (d < 3600) { return std::to_wstring(d / 60) + L" min ago"; }
        return FormatDate(t, 0, DateStyle::FullTime) + L" UTC";
    };
    std::wstring out = L"Endpoint       Status  Latency   Last OK          Fails  Last error\r\n";
    out += L"-------------  ------  --------  ---------------  -----  ----------------------------------------\r\n";
    for (const EndpointHealth& e : h.endpoints) {
        std::array<wchar_t, 512> line{};
        swprintf_s(line.data(), line.size(), L"%-13s  %6s  %6s  %-15s  %5u  %s\r\n",
                   e.name,
                   e.lastStatus > 0 ? std::to_wstring(e.lastStatus).c_str() : L"-",
                   e.lastMs > 0 ? (std::to_wstring(e.lastMs) + L" ms").c_str() : L"-",
                   ago(e.lastOk).c_str(),
                   e.failures,
                   e.lastError.c_str());
        out += line.data();
    }
    out += L"\r\n";
    if (h.backoffUntil > now) {
        out += L"Rate-limit backoff: paused for another " + std::to_wstring(h.backoffUntil - now) + L" s (provider sent HTTP 429)\r\n";
    } else {
        out += L"Rate-limit backoff: none\r\n";
    }
    out += L"Jobs waiting in the fetch queue: " + std::to_wstring(h.pending) + L"\r\n";
    out += L"Config: " + cfg_.path + L"\r\n";
    out += L"\r\nRefreshes every 2 s. Esc closes.\r\n";
    return out;
}

void App::OnShowHealth() {
    ShowHealthWindow(hInst_, hwnd_, theme_->dark, [this]() { return HealthText(); });
}

// ---------------------------------------------------------------------------
// Chart input / inset dragging

ChartInput App::BuildChartInput() {
    ChartInput in;
    const bool has = HasStocks();
    in.data           = (has && chartValid_[selected_]) ? &(*charts_)[selected_] : nullptr;
    in.range          = &kRanges[range_];
    in.opts.candles   = state_.candles;
    in.opts.sma20     = state_.sma20;
    in.opts.sma50     = state_.sma50;
    in.opts.bollinger = state_.bollinger;
    in.opts.rsi       = state_.rsi;
    in.opts.inset     = state_.inset;
    in.inset          = (has && insetValid_[selected_]) ? &(*insets_)[selected_] : nullptr;
    in.insetLabel     = kRanges[cfg_.insetRange].label;
    in.insetX         = state_.insetX;
    in.insetY         = state_.insetY;
    in.opts.benchmark = state_.benchmark && !cfg_.benchmark.empty();
    in.bench          = benchValid_ ? bench_.get() : nullptr;
    in.benchLabel     = cfg_.benchmark.c_str();
    in.compare        = state_.compare;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        compareEntries_[i].data   = chartValid_[i] ? &(*charts_)[i] : nullptr;
        compareEntries_[i].symbol = cfg_.stocks[i].symbol.c_str();
    }
    in.entries    = compareEntries_.data();
    in.entryCount = cfg_.stockCount;
    in.hoverX     = dragging_ ? -1 : hoverX_;   // no crosshair while dragging
    in.hoverY     = dragging_ ? -1 : hoverY_;
    in.theme      = theme_;
    return in;
}

PortfolioInput App::BuildPortfolioInput() {
    PortfolioInput in;
    in.history    = history_.get();
    in.range      = range_;
    in.bench      = (benchValid_ && !cfg_.benchmark.empty()) ? bench_.get() : nullptr;
    in.benchLabel = cfg_.benchmark.c_str();
    in.currency   = cfg_.portfolioCurrency.c_str();
    in.hoverX     = hoverX_;
    in.hoverY     = hoverY_;
    in.theme      = theme_;
    return in;
}

void App::LoadPortfolioHistory() {
    assert(history_ != nullptr);
    std::wstring err;
    if (!LoadHistory(HistoryPath(cfg_.path), cfg_.listName, *history_, err)) {
        *history_ = History{};
        SetStatus(err);
    }
    lastHistoryWrite_ = 0;
}

void App::MaybeRecordHistory() {
    assert(history_ != nullptr);
    const Portfolio p = ComputePortfolio();
    if (!p.any || p.partial || p.incomplete || p.value <= 0.0) { return; }
    const int64_t now = UnixNow();
    if (now - lastHistoryWrite_ < 60) { return; }          // a row a minute is plenty
    const int64_t day = LocalCalendarDay(now);
    History& h = *history_;
    const bool sameDay = h.count > 0 && h.pts[h.count - 1].day == day;
    if (sameDay && std::fabs(h.pts[h.count - 1].value - p.value) < 0.005 && std::fabs(h.pts[h.count - 1].cost - p.cost) < 0.005) {
        return;                                             // nothing new to say
    }
    const HistoryPoint pt{ day, p.value, p.cost };
    std::wstring err;
    if (!RecordHistory(HistoryPath(cfg_.path), cfg_.listName, pt, cfg_.portfolioCurrency, err)) {
        SetStatus(err);
        lastHistoryWrite_ = now;                            // do not retry every second
        return;
    }
    lastHistoryWrite_ = now;
    if (sameDay) {
        h.pts[h.count - 1] = pt;
    } else {
        if (h.count == kMaxHistoryPoints) {
            for (size_t i = 1; i < kMaxHistoryPoints; ++i) { h.pts[i - 1] = h.pts[i]; }
            --h.count;
        }
        h.pts[h.count] = pt;
        ++h.count;
    }
    assert(h.count <= kMaxHistoryPoints);
    if (state_.portfolio) { InvalidateRect(hwnd_, &chartRect_, FALSE); }
}

// `box` comes back in client coordinates, like everything the chart draws.
bool App::InsetHit(int x, int y, RectF& box) {
    if (state_.portfolio) { return false; }
    const POINT p{ x, y };
    if (!PtInRect(&chartRect_, p)) { return false; }
    if (!ChartInsetRect(ToRectF(chartRect_), BuildChartInput(), scale_, box)) { return false; }
    return box.Contains(static_cast<float>(x), static_cast<float>(y)) != FALSE;
}

// ---------------------------------------------------------------------------
// Painting

void App::EnsureBackBuffer(HDC hdc, int w, int h) {
    assert(w > 0 && h > 0);
    if (memDC_ != nullptr && bufW_ == w && bufH_ == h) { return; }
    FreeBackBuffer();
    memDC_  = CreateCompatibleDC(hdc);
    memBmp_ = CreateCompatibleBitmap(hdc, w, h);
    assert(memDC_ != nullptr && memBmp_ != nullptr);
    oldBmp_ = SelectObject(memDC_, memBmp_);
    bufW_ = w;
    bufH_ = h;
}

void App::FreeBackBuffer() {
    if (memDC_ != nullptr) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (memBmp_ != nullptr) {
        DeleteObject(memBmp_);
        memBmp_ = nullptr;
    }
    bufW_ = 0;
    bufH_ = 0;
}

// Blits the cached chart base (re-rendering it first if anything but the
// mouse changed) and draws the hover overlay on top.
void App::PaintChartLayer(Graphics& g, const ChartInput& in) {
    const int w = chartRect_.right - chartRect_.left;
    const int h = chartRect_.bottom - chartRect_.top;
    if (w <= 0 || h <= 0) { return; }
    if (state_.portfolio) {
        // Few points and no indicators: cheap enough to draw whole on every paint.
        chartDirty_ = true;   // the cached price chart is stale once we come back
        DrawPortfolioChart(g, ToRectF(chartRect_), BuildPortfolioInput(), scale_);
        return;
    }
    if (chartDC_ == nullptr || chartW_ != w || chartH_ != h) {
        if (chartDC_ != nullptr) {
            SelectObject(chartDC_, chartOld_);
            DeleteDC(chartDC_);
            chartDC_ = nullptr;
        }
        if (chartBmp_ != nullptr) { DeleteObject(chartBmp_); }
        chartDC_  = CreateCompatibleDC(memDC_);
        chartBmp_ = CreateCompatibleBitmap(memDC_, w, h);
        assert(chartDC_ != nullptr && chartBmp_ != nullptr);
        chartOld_ = SelectObject(chartDC_, chartBmp_);
        chartW_ = w;
        chartH_ = h;
        chartDirty_ = true;
    }
    if (chartDirty_) {
        Graphics cg(chartDC_);
        cg.SetSmoothingMode(SmoothingModeAntiAlias);
        cg.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        cg.Clear(theme_->bg);
        if (HasStocks()) {
            DrawChartBase(cg, RectF(0.0f, 0.0f, static_cast<REAL>(w), static_cast<REAL>(h)), in, scale_);
        }
        chartDirty_ = false;
    }
    const HDC hdc = g.GetHDC();
    BitBlt(hdc, chartRect_.left, chartRect_.top, w, h, chartDC_, 0, 0, SRCCOPY);
    g.ReleaseHDC(hdc);
    if (HasStocks()) { DrawChartOverlay(g, ToRectF(chartRect_), in, scale_); }
}

void App::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w > 0 && h > 0 && hdc != nullptr) {
        EnsureBackBuffer(hdc, w, h);
        {
            Graphics g(memDC_);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.Clear(theme_->bg);
            PaintHeader(g);
            PaintPortfolio(g);
            PaintChartLayer(g, BuildChartInput());
            PaintNews(g);
            PaintStats(g);
            PaintStatus(g);
        }
        BitBlt(hdc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

void App::PaintHeader(Graphics& g) {
    const Theme& th = *theme_;
    const RectF rc = ToRectF(headerRect_);
    const Font nameFont  = MakeFont(15.0f, scale_, FontStyleBold);
    const Font subFont   = MakeFont(9.5f, scale_);
    const Font priceFont = MakeFont(22.0f, scale_, FontStyleBold);
    const Font chgFont   = MakeFont(10.5f, scale_);
    SolidBrush dark(th.text);
    SolidBrush grey(th.textMuted);
    StringFormat left;
    left.SetFormatFlags(StringFormatFlagsNoWrap);
    left.SetTrimming(StringTrimmingEllipsisCharacter);

    if (!HasStocks()) {
        const std::wstring title = cfg_.listName.empty() ? L"Watch list" : cfg_.listName;
        g.DrawString(title.c_str(), -1, &nameFont, RectF(rc.X, rc.Y, rc.Width, 26.0f * scale_), &left, &dark);
        g.DrawString(L"This list is empty. Use Add… (Ctrl+N) to put tickers in it.", -1, &subFont,
                     RectF(rc.X, rc.Y + 30.0f * scale_, rc.Width, 20.0f * scale_), &left, &grey);
        return;
    }
    assert(selected_ < cfg_.stockCount);
    const StockEntry& entry = cfg_.stocks[selected_];
    const QuoteData&  sum   = (*summaries_)[selected_];

    const std::wstring name = (sum.valid && !sum.meta.longName.empty()) ? sum.meta.longName : entry.name;
    const float nameW = rc.Width * 0.62f;
    g.DrawString(name.c_str(), -1, &nameFont, RectF(rc.X, rc.Y, nameW, 26.0f * scale_), &left, &dark);

    std::wstring sub = entry.symbol;
    if (sum.valid) {
        if (!sum.meta.exchange.empty()) { sub += L"  ·  " + sum.meta.exchange; }
        if (sum.meta.marketTime > 0) {
            sub += L"  ·  As of " + FormatDate(sum.meta.marketTime, sum.meta.gmtOffsetSec, DateStyle::FullTime);
        }
        if (sum.meta.regularStart > 0) {
            sub += MarketOpen(sum.meta, UnixNow()) ? L"  ·  Market open" : L"  ·  Market closed";
        }
    }
    g.DrawString(sub.c_str(), -1, &subFont, RectF(rc.X, rc.Y + 30.0f * scale_, nameW, 20.0f * scale_), &left, &grey);

    const PriceChange pc = ComputeChange(sum);
    if (!pc.valid) { return; }

    // Holding line (third row) when a position exists.
    const Position pos = PositionOf(selected_);
    const bool hasTx = entry.txCount > 0;
    if (pos.qty > 0.0 || (hasTx && pos.realised != 0.0)) {
        const double value = pos.qty * pc.last;
        const double cost  = pos.cost;
        std::wstring line = L"Holding " + FormatMoney(pos.qty) + L" sh";
        if (pos.acb > 0.0) { line += (hasTx ? L" @ ACB " : L" @ ") + FormatPrice(pos.acb); }
        line += L"  ·  Value " + FormatMoney(value);
        if (cost > 0.0) {
            line += (hasTx ? L"  ·  Unrealised " : L"  ·  Gain ") + FormatSignedMoney(value - cost) + L" (" + FormatPct((value - cost) / cost * 100.0) + L")";
        }
        if (hasTx && pos.realised != 0.0) { line += L"  ·  Realised " + FormatSignedMoney(pos.realised); }
        line += L"  ·  Day " + FormatSignedMoney(pos.qty * pc.change);
        const double rec = DividendsReceivedFor(selected_);
        if (hasTx && !std::isnan(rec) && rec > 0.0) { line += L"  ·  Dividends received " + FormatMoney(rec); }
        const double div = TrailingDividends(selected_);
        if (!std::isnan(div) && div > 0.0 && pos.qty > 0.0) { line += L"  ·  Income ~" + FormatMoney(pos.qty * div) + L"/yr"; }
        SolidBrush gainBrush((cost > 0.0 && value < cost) ? th.down : th.up);
        g.DrawString(line.c_str(), -1, &subFont, RectF(rc.X, rc.Y + 50.0f * scale_, rc.Width, 20.0f * scale_), &left, &gainBrush);
    }

    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    const std::wstring price = FormatPrice(pc.last) + L" " + CurrencyOf(selected_);
    g.DrawString(price.c_str(), -1, &priceFont, RectF(rc.X, rc.Y - 2.0f * scale_, rc.Width, 34.0f * scale_), &right, &dark);
    SolidBrush chgBrush(pc.change >= 0.0 ? th.up : th.down);
    const std::wstring chg = FormatChange(pc.change, pc.pct);
    g.DrawString(chg.c_str(), -1, &chgFont, RectF(rc.X, rc.Y + 32.0f * scale_, rc.Width, 20.0f * scale_), &right, &chgBrush);
}

void App::PaintPortfolio(Graphics& g) {
    const Portfolio p = ComputePortfolio();
    if (!p.any || portfolioRect_.bottom <= portfolioRect_.top) { return; }
    const Theme& th = *theme_;
    const RectF rc = ToRectF(portfolioRect_);
    SolidBrush bg(th.listBg);
    Pen border(th.listDivider, 1.0f);
    g.FillRectangle(&bg, rc);
    g.DrawRectangle(&border, rc);

    const Font labelFont = MakeFont(8.5f, scale_);
    const Font valueFont = MakeFont(12.0f, scale_, FontStyleBold);
    const Font subFont   = MakeFont(8.5f, scale_);
    SolidBrush grey(th.textMuted);
    SolidBrush dark(th.text);
    const float pad = 8.0f * scale_;
    g.DrawString(L"Portfolio", -1, &labelFont, PointF(rc.X + pad, rc.Y + 3.0f * scale_), &grey);
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    std::wstring value = FormatMoney(p.value) + L" " + cfg_.portfolioCurrency;
    if (p.partial) { value = L"… " + value; }   // an FX rate is still on its way
    g.DrawString(value.c_str(), -1, &valueFont, RectF(rc.X, rc.Y + 1.0f * scale_, rc.Width - pad, 20.0f * scale_), &right, &dark);

    // Line 2: today's move on the left, gain since cost on the right.
    std::wstring line = L"Day " + FormatSignedMoney(p.day);
    if (p.value > 0.0) { line += L" (" + FormatPct(p.day / (p.value - p.day) * 100.0) + L")"; }
    SolidBrush dayBrush(p.day >= 0.0 ? th.up : th.down);
    StringFormat nowrap;
    nowrap.SetFormatFlags(StringFormatFlagsNoWrap);
    nowrap.SetTrimming(StringTrimmingEllipsisCharacter);
    const RectF line2(rc.X + pad, rc.Y + 24.0f * scale_, rc.Width - 2 * pad, 16.0f * scale_);
    std::wstring total;
    if (p.cost > 0.0) {
        const double gain = p.value - p.cost;
        total = L"Total " + FormatSignedMoney(gain) + L" (" + FormatPct(gain / p.cost * 100.0) + L")";
        // Both on one line: drop the percentages if they would collide.
        auto width = [&](const std::wstring& t) { RectF b; g.MeasureString(t.c_str(), -1, &subFont, PointF(0, 0), &b); return b.Width; };
        const float room = line2.Width - 12.0f * scale_;
        if (width(line) + width(total) > room) { line = L"Day " + FormatSignedMoney(p.day); }
        if (width(line) + width(total) > room) { total = L"Total " + FormatSignedMoney(gain); }
    }
    g.DrawString(line.c_str(), -1, &subFont, line2, &nowrap, &dayBrush);
    if (!total.empty()) {
        const double gain = p.value - p.cost;
        SolidBrush gainBrush(gain >= 0.0 ? th.up : th.down);
        StringFormat rightNoWrap;
        rightNoWrap.SetAlignment(StringAlignmentFar);
        rightNoWrap.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(total.c_str(), -1, &subFont, line2, &rightNoWrap, &gainBrush);
    }
    if (p.income > 0.0) {
        const std::wstring third = L"Dividend income ~" + FormatMoney(p.income) + L" " + cfg_.portfolioCurrency + L"/yr";
        g.DrawString(third.c_str(), -1, &subFont, RectF(rc.X + pad, rc.Y + 40.0f * scale_, rc.Width - 2 * pad, 16.0f * scale_), &nowrap, &grey);
    }
    std::wstring fourth;
    if (p.realised != 0.0) { fourth += L"Realised " + FormatSignedMoney(p.realised); }
    if (p.received > 0.0) { fourth += (fourth.empty() ? L"" : L"   ") + std::wstring(L"Dividends received ") + FormatMoney(p.received); }
    if (!fourth.empty()) {
        g.DrawString(fourth.c_str(), -1, &subFont, RectF(rc.X + pad, rc.Y + 56.0f * scale_, rc.Width - 2 * pad, 16.0f * scale_), &nowrap, &grey);
    }
}

void App::PaintNews(Graphics& g) {
    if (newsRect_.bottom <= newsRect_.top) { return; }
    const Theme& th = *theme_;
    const RectF rc = ToRectF(newsRect_);
    Pen divider(th.listDivider, 1.0f);
    g.DrawLine(&divider, rc.X, rc.Y, rc.X + rc.Width, rc.Y);
    const Font labelFont = MakeFont(8.5f, scale_);
    const Font rowFont   = MakeFont(9.5f, scale_);
    SolidBrush grey(th.textMuted);
    SolidBrush dark(th.text);
    StringFormat left;
    left.SetFormatFlags(StringFormatFlagsNoWrap);
    left.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!HasStocks()) { return; }
    const NewsCache& nc = (*news_)[selected_];
    std::wstring title = L"News · " + cfg_.stocks[selected_].symbol;
    if (!nc.valid) { title += L"  (loading…)"; }
    else if (!nc.error.empty()) { title += L"  (" + nc.error + L")"; }
    else if (nc.count == 0) { title += L"  (no headlines)"; }
    g.DrawString(title.c_str(), -1, &labelFont, RectF(rc.X, rc.Y + 5.0f * scale_, rc.Width, 16.0f * scale_), &left, &grey);
    const float rowH = static_cast<float>(Px(kNewsRowH));
    float y = rc.Y + 22.0f * scale_;
    for (size_t i = 0; i < nc.count && i < kMaxNews; ++i) {
        if (y + rowH > rc.Y + rc.Height) { break; }
        const NewsItem& item = nc.items[i];
        std::wstring line;
        if (item.time > 0) { line += FormatDate(item.time, 0, DateStyle::DayMonth) + L"  "; }
        if (!item.publisher.empty()) { line += item.publisher + L"  —  "; }
        line += item.title;
        g.DrawString(line.c_str(), -1, &rowFont, RectF(rc.X, y, rc.Width, rowH), &left, &dark);
        y += rowH;
    }
}

void App::PaintStats(Graphics& g) {
    const Theme& th = *theme_;
    const RectF rc = ToRectF(statsRect_);
    Pen divider(th.listDivider, 1.0f);
    g.DrawLine(&divider, rc.X, rc.Y, rc.X + rc.Width, rc.Y);
    if (!HasStocks()) { return; }
    assert(selected_ < cfg_.stockCount);
    const QuoteData& sum = (*summaries_)[selected_];
    if (!sum.valid) { return; }

    const QuoteMeta& m = sum.meta;
    const Series&    s = sum.series;
    const Candle*    today = nullptr;
    if (s.count > 0 && m.marketTime > 0 &&
        LocalDay(s.pts[s.count - 1].time, m.gmtOffsetSec) == LocalDay(m.marketTime, m.gmtOffsetSec)) {
        today = &s.pts[s.count - 1];
    }
    const PriceChange pc = ComputeChange(sum);
    const QuoteStats* qs = nullptr;
    for (size_t i = 0; i < quoteCount_ && i < kMaxStocks; ++i) {
        if (SameSymbol((*quotes_)[i].symbol, cfg_.stocks[selected_].symbol)) { qs = &(*quotes_)[i]; }
    }
    const std::wstring dash = L"-";
    std::wstring yield = dash;
    if (qs != nullptr && qs->dividendYieldPct > 0.0) {
        yield = FormatPrice(qs->dividendYieldPct) + L"%";
        if (qs->dividendRate > 0.0) { yield += L" (" + FormatPrice(qs->dividendRate) + L")"; }
    } else {
        const double div = TrailingDividends(selected_);
        if (!std::isnan(div) && div > 0.0 && pc.valid && pc.last > 0.0) {
            yield = FormatPrice(div / pc.last * 100.0) + L"% (" + FormatPrice(div) + L")";
        }
    }
    std::wstring open = today ? FormatPrice(today->open) : dash;
    if (open == dash && qs != nullptr && qs->open > 0.0) { open = FormatPrice(qs->open); }

    struct Cell { const wchar_t* label; std::wstring value; };
    const std::array<Cell, 12> cells = {{
        { L"Open",         open },
        { L"High",         m.dayHigh > 0.0 ? FormatPrice(m.dayHigh) : (today ? FormatPrice(today->high) : dash) },
        { L"Low",          m.dayLow  > 0.0 ? FormatPrice(m.dayLow)  : (today ? FormatPrice(today->low)  : dash) },
        { L"Prev close",   pc.valid ? FormatPrice(pc.prev) : dash },
        { L"Volume",       m.dayVolume > 0.0 ? FormatVolume(m.dayVolume) : (today ? FormatVolume(today->volume) : dash) },
        { L"Avg vol (3M)", (qs && qs->avgVolume3M > 0.0) ? FormatVolume(qs->avgVolume3M) : dash },
        { L"52W high",     m.wk52High > 0.0 ? FormatPrice(m.wk52High) : dash },
        { L"52W low",      m.wk52Low  > 0.0 ? FormatPrice(m.wk52Low)  : dash },
        { L"Market cap",   qs ? FormatCompact(qs->marketCap) : dash },
        { L"P/E (TTM)",    qs ? FormatRatio(qs->trailingPE) : dash },
        { L"Fwd P/E",      qs ? FormatRatio(qs->forwardPE) : dash },
        { L"Div yield",    yield },
    }};

    const Font labelFont = MakeFont(8.5f, scale_);
    const Font valueFont = MakeFont(11.0f, scale_, FontStyleBold);
    SolidBrush grey(th.textMuted);
    SolidBrush dark(th.text);
    const float cellW = rc.Width / 4.0f;
    const float rowH  = (rc.Height - 8.0f * scale_) / 3.0f;
    for (size_t i = 0; i < cells.size(); ++i) {
        const float cx = rc.X + static_cast<float>(i % 4) * cellW;
        const float cy = rc.Y + 6.0f * scale_ + static_cast<float>(i / 4) * rowH;
        g.DrawString(cells[i].label, -1, &labelFont, PointF(cx, cy), &grey);
        g.DrawString(cells[i].value.c_str(), -1, &valueFont, PointF(cx, cy + 13.0f * scale_), &dark);
    }
}

void App::PaintStatus(Graphics& g) {
    const Font font = MakeFont(8.5f, scale_);
    SolidBrush grey(theme_->textMuted);
    StringFormat fmt;
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    std::wstring text = status_;
    int64_t nextOpen = INT64_MAX;
    if (MarketsClosed(nextOpen)) {
        const int64_t now  = UnixNow();
        const int64_t wait = static_cast<int64_t>(cfg_.closedRefreshSeconds) - (now - lastRefresh_);
        int64_t next = (wait > 0) ? wait : 0;
        if (nextOpen != INT64_MAX && nextOpen - now < next) { next = nextOpen - now; }
        text += L"   ·   markets closed, next check in ";
        text += (next >= 120) ? std::to_wstring(next / 60) + L" min" : std::to_wstring(next > 0 ? next : 0) + L" s";
        if (nextOpen != INT64_MAX) { text += L" (opens " + FormatDate(nextOpen, LocalOffsetSec(), DateStyle::DayTime) + L")"; }
    } else {
        text += L"   ·   auto-refresh every " + std::to_wstring(cfg_.refreshSeconds) + L" s";
    }
    g.DrawString(text.c_str(), -1, &font, ToRectF(statusRect_), &fmt, &grey);
}

void App::PaintListItem(Graphics& g, const RECT& rcItem, size_t index, bool selected) {
    assert(index < cfg_.stockCount);
    const StockEntry& entry = cfg_.stocks[index];
    const QuoteData&  sum   = (*summaries_)[index];
    const Theme&      th    = *theme_;
    const RectF rc = ToRectF(rcItem);
    const bool alerting = alerts_[index].aboveActive || alerts_[index].belowActive;

    SolidBrush bg(selected ? th.listSel : (alerting ? th.alertBg : th.listBg));
    g.FillRectangle(&bg, rc);
    Pen divider(th.listDivider, 1.0f);
    g.DrawLine(&divider, rc.X, rc.Y + rc.Height - 1, rc.X + rc.Width, rc.Y + rc.Height - 1);

    const Font symFont   = MakeFont(10.5f, scale_, FontStyleBold);
    const Font subFont   = MakeFont(8.5f, scale_);
    const Font priceFont = MakeFont(10.5f, scale_);
    SolidBrush dark(th.text);
    SolidBrush grey(th.textMuted);
    const float pad = 8.0f * scale_;
    StringFormat left;
    left.SetFormatFlags(StringFormatFlagsNoWrap);
    left.SetTrimming(StringTrimmingEllipsisCharacter);
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    right.SetFormatFlags(StringFormatFlagsNoWrap);

    const float textW = rc.Width * 0.6f;
    std::wstring sym = entry.symbol;
    const double held = PositionOf(index).qty;
    if (held > 0.0) { sym += L"  · " + FormatMoney(held) + L" sh"; }
    if (alerting) { sym += L"  ⚠"; }
    g.DrawString(sym.c_str(), -1, &symFont, RectF(rc.X + pad, rc.Y + 5.0f * scale_, textW, 18.0f * scale_), &left, &dark);
    g.DrawString(entry.name.c_str(), -1, &subFont, RectF(rc.X + pad, rc.Y + 24.0f * scale_, textW, 16.0f * scale_), &left, &grey);

    const RectF priceRc(rc.X, rc.Y + 5.0f * scale_, rc.Width - pad, 18.0f * scale_);
    const RectF chgRc(rc.X, rc.Y + 24.0f * scale_, rc.Width - pad, 16.0f * scale_);
    if (!sum.valid) {
        const std::wstring msg = sum.error.empty() ? L"…" : L"error";
        g.DrawString(msg.c_str(), -1, &subFont, chgRc, &right, &grey);
        return;
    }
    const PriceChange pc = ComputeChange(sum);
    if (!pc.valid) { return; }
    g.DrawString(FormatPrice(pc.last).c_str(), -1, &priceFont, priceRc, &right, &dark);
    SolidBrush chgBrush(pc.change >= 0.0 ? th.up : th.down);
    g.DrawString(FormatChange(pc.change, pc.pct).c_str(), -1, &subFont, chgRc, &right, &chgBrush);
}

} // namespace st
