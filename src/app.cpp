#include "app.h"

#include "dialogs.h"
#include "resource.h"
#include "textfmt.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cassert>
#include <cmath>

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
constexpr int IDC_RANGE_BASE = 200;   // + range index
constexpr int IDC_STYLE      = 300;
constexpr int IDC_REFRESH    = 301;
constexpr int IDC_ADD        = 302;
constexpr int IDC_REMOVE     = 303;
constexpr int IDC_RELOAD     = 304;
constexpr int IDC_COMPARE    = 305;

constexpr UINT     WM_APP_TRAY   = WM_APP + 10;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT     kTrayId       = 1;

// Layout constants in device-independent pixels.
constexpr int kMargin      = 10;
constexpr int kListW       = 250;
constexpr int kListItemH   = 44;
constexpr int kPortfolioH  = 46;
constexpr int kHeaderH     = 76;
constexpr int kButtonH     = 26;
constexpr int kRangeBtnW   = 46;
constexpr int kWideBtnW    = 84;
constexpr int kBtnGap      = 4;
constexpr int kStatsH      = 112;
constexpr int kStatusH     = 20;
constexpr int kListBtnGap  = 6;
constexpr int kMinWinW     = 960;
constexpr int kMinWinH     = 620;

constexpr DWORD kDwmUseImmersiveDarkMode = 20;

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

bool SameSymbol(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

Font MakeFont(float pt, float scale, INT style = FontStyleRegular) {
    return Font(L"Segoe UI", FontPx(pt, scale), style, UnitPixel);
}

} // namespace

App::~App() {
    fetcher_.Stop();
    if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE, &tray_); }
    FreeBackBuffer();
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
    if (!LoadConfig(cfgPath, cfg_, err)) { return false; }
    LoadViewState(cfgPath, state_);
    range_ = (state_.range < kRanges.size()) ? state_.range : cfg_.defaultRange;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (SameSymbol(cfg_.stocks[i].symbol, state_.selected)) { selected_ = i; }
    }
    summaries_ = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    charts_    = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    inset_     = std::make_unique<QuoteData>();
    quotes_    = std::make_unique<std::array<QuoteStats, kMaxStocks>>();
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

    const ACCEL accel[] = {
        { FVIRTKEY, VK_F5, static_cast<WORD>(IDM_REFRESH) },
        { FVIRTKEY | FCONTROL, 'N', static_cast<WORD>(IDM_ADD) },
        { FVIRTKEY | FCONTROL, VK_UP, static_cast<WORD>(IDM_MOVEUP) },
        { FVIRTKEY | FCONTROL, VK_DOWN, static_cast<WORD>(IDM_MOVEDOWN) },
    };
    hAccel_ = CreateAcceleratorTableW(const_cast<ACCEL*>(accel), 4);
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
    RequestChart(true);
    RequestInset();
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
    const HWND controls[] = { hList_, hStyleBtn_, hCompareBtn_, hRefreshBtn_, hAddBtn_, hRemoveBtn_, hReloadBtn_ };
    for (HWND h : controls) {
        if (h != nullptr) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE); }
    }
    for (HWND h : hRangeBtns_) {
        if (h != nullptr) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hUiFont_), TRUE); }
    }
    if (hList_ != nullptr) { SendMessageW(hList_, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(Px(kListItemH))); }
}

void App::CreateControls() {
    assert(cfg_.stockCount > 0);
    auto button = [this](const wchar_t* text, DWORD style, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 hInst_, nullptr);
        assert(h != nullptr);
        return h;
    };

    hList_ = CreateWindowExW(0, L"LISTBOX", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                             hInst_, nullptr);
    assert(hList_ != nullptr);
    RebuildList();

    for (size_t i = 0; i < kRanges.size(); ++i) {
        hRangeBtns_[i] = button(kRanges[i].label, BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0),
                                IDC_RANGE_BASE + static_cast<int>(i));
    }
    SendMessageW(hRangeBtns_[range_], BM_SETCHECK, BST_CHECKED, 0);
    hStyleBtn_   = button(L"Candles", WS_GROUP | BS_AUTOCHECKBOX | BS_PUSHLIKE, IDC_STYLE);
    hCompareBtn_ = button(L"Compare", WS_GROUP | BS_AUTOCHECKBOX | BS_PUSHLIKE, IDC_COMPARE);
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
    SendMessageW(hList_, LB_SETCURSEL, static_cast<WPARAM>(selected_), 0);
}

void App::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    const int m = Px(kMargin);

    const bool portfolio = ComputePortfolio().any;
    int listTop = m;
    portfolioRect_ = { m, m, m + Px(kListW), m + (portfolio ? Px(kPortfolioH) : 0) };
    if (portfolio) { listTop = portfolioRect_.bottom + Px(kListBtnGap); }

    const int listBtnH = Px(kButtonH);
    listRect_ = { m, listTop, m + Px(kListW), h - m - listBtnH - Px(kListBtnGap) };
    MoveWindow(hList_, listRect_.left, listRect_.top,
               listRect_.right - listRect_.left, listRect_.bottom - listRect_.top, TRUE);
    const int listBtnY = h - m - listBtnH;
    const int listBtnW = (Px(kListW) - 2 * Px(kBtnGap)) / 3;
    MoveWindow(hAddBtn_, listRect_.left, listBtnY, listBtnW, listBtnH, TRUE);
    MoveWindow(hRemoveBtn_, listRect_.left + listBtnW + Px(kBtnGap), listBtnY, listBtnW, listBtnH, TRUE);
    MoveWindow(hReloadBtn_, listRect_.right - listBtnW, listBtnY, listBtnW, listBtnH, TRUE);

    const int rightX = listRect_.right + m;
    const int rightR = w - m;
    headerRect_ = { rightX, m, rightR, m + Px(kHeaderH) };

    const int btnY = headerRect_.bottom + Px(4);
    const int btnH = Px(kButtonH);
    int x = rightX;
    for (size_t i = 0; i < kRanges.size(); ++i) {
        MoveWindow(hRangeBtns_[i], x, btnY, Px(kRangeBtnW), btnH, TRUE);
        x += Px(kRangeBtnW) + Px(kBtnGap);
    }
    MoveWindow(hStyleBtn_, x + m, btnY, Px(kWideBtnW), btnH, TRUE);
    MoveWindow(hCompareBtn_, x + m + Px(kWideBtnW + kBtnGap), btnY, Px(kWideBtnW), btnH, TRUE);
    MoveWindow(hRefreshBtn_, rightR - Px(kWideBtnW + 16), btnY, Px(kWideBtnW + 16), btnH, TRUE);

    statusRect_ = { rightX, h - m - Px(kStatusH), rightR, h - m };
    statsRect_  = { rightX, statusRect_.top - Px(kStatsH), rightR, statusRect_.top };
    chartRect_  = { rightX, btnY + btnH + m, rightR, statsRect_.top - m };
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
    const HWND controls[] = { hList_, hStyleBtn_, hCompareBtn_, hRefreshBtn_, hAddBtn_, hRemoveBtn_, hReloadBtn_ };
    for (HWND h : controls) {
        if (h != nullptr) { SetWindowTheme(h, sub, nullptr); }
    }
    for (HWND h : hRangeBtns_) {
        if (h != nullptr) { SetWindowTheme(h, sub, nullptr); }
    }
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
    check(IDM_MINTRAY,   cfg_.minimizeToTray);
    const int themeId = (cfg_.theme == ThemeMode::Light) ? IDM_THEME_LIGHT
                      : (cfg_.theme == ThemeMode::Dark)  ? IDM_THEME_DARK : IDM_THEME_SYSTEM;
    CheckMenuRadioItem(hMenu_, IDM_THEME_SYSTEM, IDM_THEME_DARK, static_cast<UINT>(themeId), MF_BYCOMMAND);
    SendMessageW(hStyleBtn_, BM_SETCHECK, state_.candles ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hCompareBtn_, BM_SETCHECK, state_.compare ? BST_CHECKED : BST_UNCHECKED, 0);
    DrawMenuBar(hwnd_);
}

void App::ToggleOption(bool& flag) {
    flag = !flag;
    SyncViewMenu();
    InvalidateRect(hwnd_, &chartRect_, FALSE);
}

// ---------------------------------------------------------------------------
// Data requests

void App::RequestAllSummaries() {
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (!fetcher_.Enqueue(JobKind::Summary, i, 0)) { SetStatus(L"Fetch queue is full"); }
    }
}

void App::RequestQuotes() {
    const bool queued = fetcher_.EnqueueQuote();
    (void)queued;  // disabled by config, or already pending
}

void App::RequestChart(bool clearCurrent) {
    assert(selected_ < cfg_.stockCount);
    assert(range_ < kRanges.size());
    if (clearCurrent) {
        chartValid_[selected_] = false;
        InvalidateRect(hwnd_, &chartRect_, FALSE);
    }
    if (!fetcher_.Enqueue(JobKind::Chart, selected_, range_)) { SetStatus(L"Fetch queue is full"); }
    if (state_.compare) {
        for (size_t i = 0; i < cfg_.stockCount; ++i) {
            if (i != selected_) { fetcher_.Enqueue(JobKind::Chart, i, range_); }
        }
    }
}

void App::RequestInset() {
    if (!state_.inset) { return; }
    insetValid_ = false;
    if (!fetcher_.Enqueue(JobKind::Inset, selected_, cfg_.insetRange)) { SetStatus(L"Fetch queue is full"); }
}

void App::SelectStock(size_t index) {
    if (index >= cfg_.stockCount) { return; }
    selected_ = index;
    if (static_cast<size_t>(SendMessageW(hList_, LB_GETCURSEL, 0, 0)) != index) {
        SendMessageW(hList_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
    RequestChart(!chartValid_[index]);
    RequestInset();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SelectRange(size_t index) {
    if (index >= kRanges.size()) { return; }
    range_ = index;
    for (size_t i = 0; i < kMaxStocks; ++i) { chartValid_[i] = false; }
    RequestChart(true);
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

App::Portfolio App::ComputePortfolio() const {
    Portfolio p;
    bool mixed = false;
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        const Holding& h = cfg_.stocks[i].holding;
        if (h.qty <= 0.0) { continue; }
        p.any = true;
        const QuoteData& q = (*summaries_)[i];
        const PriceChange pc = ComputeChange(q);
        if (!pc.valid) { continue; }
        p.value += h.qty * pc.last;
        p.cost  += h.qty * h.cost;
        p.day   += h.qty * pc.change;
        if (p.currency.empty() && !mixed) { p.currency = q.meta.currency; }
        else if (!SameSymbol(p.currency, q.meta.currency)) { mixed = true; }
    }
    if (mixed) { p.currency.clear(); }
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
    StockEntry entry;
    if (!RunAddTickerDialog(hInst_, hwnd_, cfg_, fetcher_, entry)) { return; }
    std::wstring err;
    if (!WriteStockEntry(cfg_.path, entry, err)) {
        SetStatus(err);
        return;
    }
    const size_t index = cfg_.stockCount;
    cfg_.stocks[index] = entry;
    ++cfg_.stockCount;
    (*summaries_)[index] = QuoteData{};
    chartValid_[index]   = false;
    alerts_[index]       = AlertState{};
    SendMessageW(hList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.symbol.c_str()));

    fetcher_.UpdateConfig(cfg_);   // drops pending jobs, so re-request everything
    RequestAllSummaries();
    RequestQuotes();
    SelectStock(index);
    SetStatus(L"Added " + entry.symbol);
    assert(cfg_.stockCount <= kMaxStocks);
}

void App::OnRemoveTicker() {
    assert(selected_ < cfg_.stockCount);
    if (cfg_.stockCount <= 1) {
        SetStatus(L"Keep at least one ticker");
        return;
    }
    const size_t       index  = selected_;
    const std::wstring symbol = cfg_.stocks[index].symbol;
    std::wstring err;
    if (!DeleteStockEntry(cfg_.path, symbol, err)) {
        SetStatus(err);
        return;
    }
    // Shift the tail down by one (bounded by kMaxStocks).
    for (size_t i = index; i + 1 < cfg_.stockCount && i + 1 < kMaxStocks; ++i) {
        cfg_.stocks[i]     = cfg_.stocks[i + 1];
        (*summaries_)[i]   = (*summaries_)[i + 1];
        (*charts_)[i]      = (*charts_)[i + 1];
        chartValid_[i]     = chartValid_[i + 1];
        alerts_[i]         = alerts_[i + 1];
    }
    --cfg_.stockCount;
    cfg_.stocks[cfg_.stockCount]   = StockEntry{};
    (*summaries_)[cfg_.stockCount] = QuoteData{};
    chartValid_[cfg_.stockCount]   = false;
    SendMessageW(hList_, LB_DELETESTRING, static_cast<WPARAM>(index), 0);

    fetcher_.UpdateConfig(cfg_);
    RequestAllSummaries();
    RequestQuotes();
    SelectStock((index < cfg_.stockCount) ? index : cfg_.stockCount - 1);
    Layout();
    SetStatus(L"Removed " + symbol);
    assert(cfg_.stockCount >= 1);
}

void App::OnMoveTicker(int delta) {
    assert(delta == 1 || delta == -1);
    const size_t i = selected_;
    if ((delta < 0 && i == 0) || (delta > 0 && i + 1 >= cfg_.stockCount)) { return; }
    const size_t j = (delta < 0) ? i - 1 : i + 1;
    std::swap(cfg_.stocks[i], cfg_.stocks[j]);
    std::wstring err;
    if (!WriteStockOrder(cfg_.path, cfg_, err)) {
        std::swap(cfg_.stocks[i], cfg_.stocks[j]);
        SetStatus(err);
        return;
    }
    std::swap((*summaries_)[i], (*summaries_)[j]);
    std::swap((*charts_)[i], (*charts_)[j]);
    std::swap(chartValid_[i], chartValid_[j]);
    std::swap(alerts_[i], alerts_[j]);
    selected_ = j;
    RebuildList();
    fetcher_.UpdateConfig(cfg_);
    RequestAllSummaries();
    SelectStock(j);
}

void App::OnEditHolding() {
    assert(selected_ < cfg_.stockCount);
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
    Layout();
    InvalidateRect(hwnd_, nullptr, TRUE);
    SetStatus((h.qty > 0.0) ? L"Holding saved for " + e.symbol : L"Holding cleared for " + e.symbol);
}

void App::OnEditAlerts() {
    assert(selected_ < cfg_.stockCount);
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
    InvalidateRect(hList_, nullptr, TRUE);
    SetStatus(L"Alerts saved for " + e.symbol);
}

// Re-reads stocktool.cfg (tickers, refresh interval, URL templates) without
// restarting. Keeps the current selection if its symbol is still listed.
void App::OnReloadConfig() {
    assert(selected_ < cfg_.stockCount);
    Config fresh;
    std::wstring err;
    if (!LoadConfig(cfg_.path, fresh, err)) {
        SetStatus(L"Reload failed: " + err);   // keep running on the old config
        return;
    }
    const std::wstring current = cfg_.stocks[selected_].symbol;
    size_t newSel = 0;
    for (size_t i = 0; i < fresh.stockCount; ++i) {
        if (SameSymbol(fresh.stocks[i].symbol, current)) { newSel = i; }
    }

    cfg_ = fresh;
    for (size_t i = 0; i < kMaxStocks; ++i) {
        (*summaries_)[i] = QuoteData{};
        chartValid_[i]   = false;
        alerts_[i]       = AlertState{};
    }
    quoteCount_ = 0;
    selected_   = newSel;
    RebuildList();
    KillTimer(hwnd_, kRefreshTimer);
    SetTimer(hwnd_, kRefreshTimer, cfg_.refreshSeconds * 1000u, nullptr);

    fetcher_.UpdateConfig(cfg_);
    ApplyTheme();
    SyncViewMenu();
    Layout();
    RequestAllSummaries();
    RequestQuotes();
    SelectStock(newSel);
    SetStatus(L"Reloaded " + std::to_wstring(cfg_.stockCount) + L" tickers from " + cfg_.path);
    assert(cfg_.stockCount >= 1 && newSel < cfg_.stockCount);
}

// ---------------------------------------------------------------------------
// Message handlers

void App::OnSize(WPARAM wp) {
    if (wp == SIZE_MINIMIZED) {
        if (cfg_.minimizeToTray) { ShowWindow(hwnd_, SW_HIDE); }
        return;
    }
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnCommand(int id, UINT code) {
    if (id == IDC_LIST && code == LBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(hList_, LB_GETCURSEL, 0, 0);
        if (sel != LB_ERR) { SelectStock(static_cast<size_t>(sel)); }
        return;
    }
    if (id >= IDC_RANGE_BASE && id < IDC_RANGE_BASE + static_cast<int>(kRanges.size()) && code == BN_CLICKED) {
        SelectRange(static_cast<size_t>(id - IDC_RANGE_BASE));
        return;
    }
    switch (id) {
    case IDC_STYLE:
    case IDM_CANDLES:   ToggleOption(state_.candles); break;
    case IDC_COMPARE:
    case IDM_COMPARE:   ToggleOption(state_.compare); RequestChart(false); break;
    case IDM_SMA20:     ToggleOption(state_.sma20); break;
    case IDM_SMA50:     ToggleOption(state_.sma50); break;
    case IDM_BOLLINGER: ToggleOption(state_.bollinger); break;
    case IDM_RSI:       ToggleOption(state_.rsi); break;
    case IDM_INSET:     ToggleOption(state_.inset); RequestInset(); break;
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
    case IDM_REFRESH:   OnTimer(); break;
    case IDC_RELOAD:
    case IDM_RELOAD:    OnReloadConfig(); break;
    case IDC_ADD:
    case IDM_ADD:       OnAddTicker(); break;
    case IDC_REMOVE:
    case IDM_REMOVE:    OnRemoveTicker(); break;
    case IDM_MOVEUP:    OnMoveTicker(-1); break;
    case IDM_MOVEDOWN:  OnMoveTicker(+1); break;
    case IDM_HOLDING:   OnEditHolding(); break;
    case IDM_ALERTS:    OnEditAlerts(); break;
    case IDM_TRAY_SHOW: ShowFromTray(); break;
    case IDM_TRAY_EXIT:
    case IDM_EXIT:      PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
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

void App::OnTimer() {
    RequestAllSummaries();
    RequestQuotes();
    RequestChart(false);
    if (state_.inset && !insetValid_) { RequestInset(); }
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
            InvalidateRect(hwnd_, &chartRect_, FALSE);
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

void App::OnDpiChanged(WPARAM wp, LPARAM lp) {
    scale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
    assert(scale_ > 0.0f);
    CreateFonts();
    const RECT* r = reinterpret_cast<const RECT*>(lp);
    assert(r != nullptr);
    SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    if (q.valid) { SetStatus(L"Updated " + TimeNow()); }
    else         { SetStatus(cfg_.stocks[stock].symbol + L": " + q.error); }
    CheckAlerts(stock, ComputeChange(q));
    UpdateTrayTip();
    InvalidateRect(hList_, nullptr, TRUE);
    if (ComputePortfolio().any) {
        if (portfolioRect_.bottom == portfolioRect_.top) { Layout(); }
        InvalidateRect(hwnd_, &portfolioRect_, FALSE);
    }
    if (stock == selected_) {
        InvalidateRect(hwnd_, &headerRect_, FALSE);
        InvalidateRect(hwnd_, &statsRect_, FALSE);
    }
}

void App::OnChartReady(size_t stock, size_t range) {
    if (stock >= cfg_.stockCount || range != range_) { return; }  // stale
    QuoteData& q = (*charts_)[stock];
    if (!fetcher_.CopyChart(stock, range, q)) { return; }
    if (!SameSymbol(q.symbol, cfg_.stocks[stock].symbol)) {
        q = QuoteData{};  // fetched before the list changed; a fresh request is queued
        return;
    }
    chartValid_[stock] = true;
    if (stock == selected_ && !q.valid) { SetStatus(cfg_.stocks[stock].symbol + L": " + q.error); }
    if (stock == selected_ || state_.compare) { InvalidateRect(hwnd_, &chartRect_, FALSE); }
}

void App::OnInsetReady(size_t stock, size_t range) {
    if (stock != selected_ || range != cfg_.insetRange) { return; }
    if (!fetcher_.CopyInset(stock, range, *inset_)) { return; }
    if (!SameSymbol(inset_->symbol, cfg_.stocks[stock].symbol)) {
        *inset_ = QuoteData{};
        return;
    }
    insetValid_ = true;
    InvalidateRect(hwnd_, &chartRect_, FALSE);
}

void App::OnQuoteReady() {
    std::wstring err;
    if (!fetcher_.CopyQuotes(*quotes_, quoteCount_, err)) { return; }
    if (!err.empty()) { SetStatus(L"Fundamentals: " + err); }
    InvalidateRect(hwnd_, &statsRect_, FALSE);
}

// ---------------------------------------------------------------------------
// Chart input / inset dragging

ChartInput App::BuildChartInput() {
    assert(selected_ < cfg_.stockCount);
    ChartInput in;
    in.data           = chartValid_[selected_] ? &(*charts_)[selected_] : nullptr;
    in.range          = &kRanges[range_];
    in.opts.candles   = state_.candles;
    in.opts.sma20     = state_.sma20;
    in.opts.sma50     = state_.sma50;
    in.opts.bollinger = state_.bollinger;
    in.opts.rsi       = state_.rsi;
    in.opts.inset     = state_.inset;
    in.inset          = insetValid_ ? inset_.get() : nullptr;
    in.insetLabel     = kRanges[cfg_.insetRange].label;
    in.insetX         = state_.insetX;
    in.insetY         = state_.insetY;
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

// `box` comes back in client coordinates, like everything the chart draws.
bool App::InsetHit(int x, int y, RectF& box) {
    const POINT p{ x, y };
    if (!PtInRect(&chartRect_, p)) { return false; }
    if (!ChartInsetRect(ToRectF(chartRect_), BuildChartInput(), scale_, box)) { return false; }
    return box.Contains(static_cast<float>(x), static_cast<float>(y)) != FALSE;
}

void App::OnLButtonDown(int x, int y) {
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
    InvalidateRect(hwnd_, &chartRect_, FALSE);
}

bool App::OnSetCursor() {
    POINT p{};
    if (!GetCursorPos(&p) || !ScreenToClient(hwnd_, &p)) { return false; }
    RectF box;
    if (!dragging_ && !InsetHit(p.x, p.y, box)) { return false; }
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
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

            const ChartInput in = BuildChartInput();
            DrawChart(g, ToRectF(chartRect_), in, scale_);
            PaintStats(g);
            PaintStatus(g);
        }
        BitBlt(hdc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

void App::PaintHeader(Graphics& g) {
    assert(selected_ < cfg_.stockCount);
    const StockEntry& entry = cfg_.stocks[selected_];
    const QuoteData&  sum   = (*summaries_)[selected_];
    const Theme&      th    = *theme_;
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

    const std::wstring name = (sum.valid && !sum.meta.longName.empty()) ? sum.meta.longName : entry.name;
    const float nameW = rc.Width * 0.62f;
    g.DrawString(name.c_str(), -1, &nameFont, RectF(rc.X, rc.Y, nameW, 26.0f * scale_), &left, &dark);

    std::wstring sub = entry.symbol;
    if (sum.valid) {
        if (!sum.meta.exchange.empty()) { sub += L"  ·  " + sum.meta.exchange; }
        if (sum.meta.marketTime > 0) {
            sub += L"  ·  As of " + FormatDate(sum.meta.marketTime, sum.meta.gmtOffsetSec, DateStyle::FullTime);
        }
    }
    g.DrawString(sub.c_str(), -1, &subFont, RectF(rc.X, rc.Y + 30.0f * scale_, nameW, 20.0f * scale_), &left, &grey);

    const PriceChange pc = ComputeChange(sum);
    if (!pc.valid) { return; }

    // Holding line (third row) when a position exists.
    const Holding& hd = entry.holding;
    if (hd.qty > 0.0) {
        const double value = hd.qty * pc.last;
        const double cost  = hd.qty * hd.cost;
        std::wstring line = L"Holding " + FormatMoney(hd.qty) + L" sh";
        if (hd.cost > 0.0) { line += L" @ " + FormatPrice(hd.cost); }
        line += L"  ·  Value " + FormatMoney(value);
        if (cost > 0.0) {
            line += L"  ·  Gain " + FormatSignedMoney(value - cost) + L" (" + FormatPct((value - cost) / cost * 100.0) + L")";
        }
        line += L"  ·  Day " + FormatSignedMoney(hd.qty * pc.change);
        SolidBrush gainBrush((cost > 0.0 && value < cost) ? th.down : th.up);
        g.DrawString(line.c_str(), -1, &subFont, RectF(rc.X, rc.Y + 50.0f * scale_, rc.Width, 20.0f * scale_), &left, &gainBrush);
    }

    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    const std::wstring price = FormatPrice(pc.last) + L" " + sum.meta.currency;
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
    const std::wstring value = FormatMoney(p.value) + (p.currency.empty() ? L"" : L" " + p.currency);
    g.DrawString(value.c_str(), -1, &valueFont, RectF(rc.X, rc.Y + 1.0f * scale_, rc.Width - pad, 20.0f * scale_), &right, &dark);

    std::wstring line = L"Day " + FormatSignedMoney(p.day);
    if (p.value > 0.0) { line += L" (" + FormatPct(p.day / (p.value - p.day) * 100.0) + L")"; }
    if (p.cost > 0.0) {
        line += L"   Total " + FormatSignedMoney(p.value - p.cost) + L" (" + FormatPct((p.value - p.cost) / p.cost * 100.0) + L")";
    }
    SolidBrush dayBrush(p.day >= 0.0 ? th.up : th.down);
    g.DrawString(line.c_str(), -1, &subFont, PointF(rc.X + pad, rc.Y + 24.0f * scale_), &dayBrush);
}

void App::PaintStats(Graphics& g) {
    assert(selected_ < cfg_.stockCount);
    const QuoteData& sum = (*summaries_)[selected_];
    const Theme& th = *theme_;
    const RectF rc = ToRectF(statsRect_);
    Pen divider(th.listDivider, 1.0f);
    g.DrawLine(&divider, rc.X, rc.Y, rc.X + rc.Width, rc.Y);
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
        { L"Avg vol (3M)", qs ? FormatVolume(qs->avgVolume3M) : dash },
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
    text += L"   ·   auto-refresh every " + std::to_wstring(cfg_.refreshSeconds) + L" s";
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
    if (entry.holding.qty > 0.0) { sym += L"  · " + FormatMoney(entry.holding.qty) + L" sh"; }
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
