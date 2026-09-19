#include "app.h"

#include "textfmt.h"

#include <windowsx.h>

#include <cassert>
#include <cmath>

namespace st {
namespace {

using namespace Gdiplus;

constexpr wchar_t kClassName[] = L"StockToolMainWindow";

#define ST_WIDE_(s) L##s
#define ST_WIDE(s)  ST_WIDE_(s)
constexpr wchar_t kWindowTitle[] = L"StockTool v" ST_WIDE(STOCKTOOL_VERSION);

constexpr int IDC_LIST       = 100;
constexpr int IDC_RANGE_BASE = 200;   // + range index
constexpr int IDC_STYLE      = 300;
constexpr int IDC_REFRESH    = 301;

constexpr UINT_PTR kRefreshTimer = 1;

// Layout constants in device-independent pixels.
constexpr int kMargin     = 10;
constexpr int kListW      = 250;
constexpr int kListItemH  = 44;
constexpr int kHeaderH    = 60;
constexpr int kButtonH    = 26;
constexpr int kRangeBtnW  = 46;
constexpr int kWideBtnW   = 84;
constexpr int kBtnGap     = 4;
constexpr int kStatsH     = 78;
constexpr int kStatusH    = 20;
constexpr int kMinWinW    = 900;
constexpr int kMinWinH    = 560;

const Color kBgColor(255, 250, 250, 250);
const Color kTextDark(255, 32, 33, 36);
const Color kTextGrey(255, 95, 99, 104);
const Color kUp(255, 30, 142, 62);
const Color kDown(255, 217, 48, 37);
const Color kListSel(255, 226, 236, 252);
const Color kListBg(255, 255, 255, 255);
const Color kListDivider(255, 238, 238, 238);

RectF ToRectF(const RECT& r) {
    return RectF(static_cast<REAL>(r.left), static_cast<REAL>(r.top),
                 static_cast<REAL>(r.right - r.left), static_cast<REAL>(r.bottom - r.top));
}

// Day number in exchange-local time, for "is this bar today's bar?" checks.
int64_t LocalDay(int64_t unixTime, int32_t gmtOffset) {
    return (unixTime + gmtOffset) / 86400;
}

struct PriceChange {
    bool   valid  = false;
    double last   = 0.0;
    double prev   = 0.0;
    double change = 0.0;
    double pct    = 0.0;
};

// Derives last price and day change from a summary (daily bars) fetch.
PriceChange ComputeChange(const QuoteData& q) {
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

std::wstring TimeNow() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::array<wchar_t, 32> b{};
    swprintf_s(b.data(), b.size(), L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return b.data();
}

} // namespace

App::~App() {
    fetcher_.Stop();
    FreeBackBuffer();
    if (hUiFont_ != nullptr) { DeleteObject(hUiFont_); }
    if (hAccel_ != nullptr)  { DestroyAcceleratorTable(hAccel_); }
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
    range_     = cfg_.defaultRange;
    summaries_ = std::make_unique<std::array<QuoteData, kMaxStocks>>();
    chart_     = std::make_unique<QuoteData>();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &App::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
        err = L"RegisterClassEx failed";
        return false;
    }

    const float sysScale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    const int w = static_cast<int>(1150.0f * sysScale);
    const int h = static_cast<int>(720.0f * sysScale);
    hwnd_ = CreateWindowExW(0, kClassName, kWindowTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, hInst, this);
    if (hwnd_ == nullptr) {
        err = L"CreateWindowEx failed";
        return false;
    }

    const ACCEL accel[] = { { FVIRTKEY, VK_F5, static_cast<WORD>(IDC_REFRESH) } };
    hAccel_ = CreateAcceleratorTableW(const_cast<ACCEL*>(accel), 1);
    assert(hAccel_ != nullptr);

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);
    return true;
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
    case WM_DESTROY:      OnDestroy(); return 0;
    case WM_SIZE:         OnSize();    return 0;
    case WM_PAINT:        OnPaint();   return 0;
    case WM_ERASEBKGND:   return 1;
    case WM_COMMAND:      OnCommand(wp); return 0;
    case WM_TIMER:        if (wp == kRefreshTimer) { OnTimer(); } return 0;
    case WM_MOUSEMOVE:    OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSELEAVE:   OnMouseLeave(); return 0;
    case WM_DPICHANGED:   OnDpiChanged(wp, lp); return 0;
    case WM_MEASUREITEM:  OnMeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp)); return TRUE;
    case WM_DRAWITEM:     OnDrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
        SetBkColor(reinterpret_cast<HDC>(wp), RGB(250, 250, 250));
        static HBRUSH bg = CreateSolidBrush(RGB(250, 250, 250));
        return reinterpret_cast<LRESULT>(bg);
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = Px(kMinWinW);
        mmi->ptMinTrackSize.y = Px(kMinWinH);
        return 0;
    }
    case WM_APP_SUMMARY_READY: OnSummaryReady(static_cast<size_t>(wp)); return 0;
    case WM_APP_CHART_READY:   OnChartReady(static_cast<size_t>(wp), static_cast<size_t>(lp)); return 0;
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
    Layout();

    std::wstring err;
    if (!fetcher_.Start(hwnd_, cfg_, err)) {
        SetStatus(err);
        return;
    }
    RequestAllSummaries();
    RequestChart(true);
    SetTimer(hwnd_, kRefreshTimer, cfg_.refreshSeconds * 1000u, nullptr);
    SetStatus(L"Loading " + std::to_wstring(cfg_.stockCount) + L" symbols from " + cfg_.path);
}

void App::OnDestroy() {
    KillTimer(hwnd_, kRefreshTimer);
    fetcher_.Stop();
    PostQuitMessage(0);
}

void App::CreateFonts() {
    if (hUiFont_ != nullptr) { DeleteObject(hUiFont_); }
    hUiFont_ = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    assert(hUiFont_ != nullptr);
    const HWND controls[] = { hList_, hStyleBtn_, hRefreshBtn_ };
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
    hList_ = CreateWindowExW(0, L"LISTBOX", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER | LBS_NOTIFY |
                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                             hInst_, nullptr);
    assert(hList_ != nullptr);
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        SendMessageW(hList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg_.stocks[i].symbol.c_str()));
    }
    SendMessageW(hList_, LB_SETCURSEL, 0, 0);

    for (size_t i = 0; i < kRanges.size(); ++i) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE;
        if (i == 0) { style |= WS_GROUP; }
        hRangeBtns_[i] = CreateWindowExW(0, L"BUTTON", kRanges[i].label, style, 0, 0, 10, 10, hwnd_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RANGE_BASE + static_cast<int>(i))),
                                         hInst_, nullptr);
        assert(hRangeBtns_[i] != nullptr);
    }
    SendMessageW(hRangeBtns_[range_], BM_SETCHECK, BST_CHECKED, 0);

    hStyleBtn_ = CreateWindowExW(0, L"BUTTON", L"Candles",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STYLE)),
                                 hInst_, nullptr);
    hRefreshBtn_ = CreateWindowExW(0, L"BUTTON", L"Refresh (F5)",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON,
                                   0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)),
                                   hInst_, nullptr);
    assert(hStyleBtn_ != nullptr && hRefreshBtn_ != nullptr);
    CreateFonts();  // apply font + item height to the controls just made
}

void App::Layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    const int m = Px(kMargin);

    listRect_ = { m, m, m + Px(kListW), h - m };
    MoveWindow(hList_, listRect_.left, listRect_.top,
               listRect_.right - listRect_.left, listRect_.bottom - listRect_.top, TRUE);

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
    MoveWindow(hRefreshBtn_, rightR - Px(kWideBtnW + 16), btnY, Px(kWideBtnW + 16), btnH, TRUE);

    statusRect_ = { rightX, h - m - Px(kStatusH), rightR, h - m };
    statsRect_  = { rightX, statusRect_.top - Px(kStatsH), rightR, statusRect_.top };
    chartRect_  = { rightX, btnY + btnH + m, rightR, statsRect_.top - m };
    assert(chartRect_.bottom >= chartRect_.top);
}

// ---------------------------------------------------------------------------
// Data requests

void App::RequestAllSummaries() {
    for (size_t i = 0; i < cfg_.stockCount; ++i) {
        if (!fetcher_.Enqueue({ JobKind::Summary, i, 0 })) { SetStatus(L"Fetch queue is full"); }
    }
}

void App::RequestChart(bool clearCurrent) {
    assert(selected_ < cfg_.stockCount);
    assert(range_ < kRanges.size());
    if (clearCurrent) {
        *chart_       = QuoteData{};
        chartLoading_ = true;
        InvalidateRect(hwnd_, &chartRect_, FALSE);
    }
    if (!fetcher_.Enqueue({ JobKind::Chart, selected_, range_ })) { SetStatus(L"Fetch queue is full"); }
}

void App::SelectStock(size_t index) {
    if (index >= cfg_.stockCount) { return; }
    selected_ = index;
    if (static_cast<size_t>(SendMessageW(hList_, LB_GETCURSEL, 0, 0)) != index) {
        SendMessageW(hList_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
    }
    RequestChart(true);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SelectRange(size_t index) {
    if (index >= kRanges.size()) { return; }
    range_ = index;
    RequestChart(true);
}

void App::SetStatus(const std::wstring& text) {
    status_ = text;
    InvalidateRect(hwnd_, &statusRect_, FALSE);
}

// ---------------------------------------------------------------------------
// Message handlers

void App::OnSize() {
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnCommand(WPARAM wp) {
    const int  id   = LOWORD(wp);
    const UINT code = HIWORD(wp);
    if (id == IDC_LIST && code == LBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(hList_, LB_GETCURSEL, 0, 0);
        if (sel != LB_ERR) { SelectStock(static_cast<size_t>(sel)); }
    } else if (id >= IDC_RANGE_BASE && id < IDC_RANGE_BASE + static_cast<int>(kRanges.size()) && code == BN_CLICKED) {
        SelectRange(static_cast<size_t>(id - IDC_RANGE_BASE));
    } else if (id == IDC_STYLE && code == BN_CLICKED) {
        candles_ = (SendMessageW(hStyleBtn_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        InvalidateRect(hwnd_, &chartRect_, FALSE);
    } else if (id == IDC_REFRESH) {
        OnTimer();
    }
}

void App::OnTimer() {
    RequestAllSummaries();
    RequestChart(false);
}

void App::OnMouseMove(int x, int y) {
    if (!tracking_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        tracking_ = (TrackMouseEvent(&tme) != FALSE);
    }
    const POINT p{ x, y };
    const bool inChart = PtInRect(&chartRect_, p) != FALSE;
    const int nx = inChart ? x - chartRect_.left : -1;
    const int ny = inChart ? y - chartRect_.top : -1;
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
    if (q.valid) { SetStatus(L"Updated " + TimeNow()); }
    else         { SetStatus(cfg_.stocks[stock].symbol + L": " + q.error); }
    InvalidateRect(hList_, nullptr, TRUE);
    if (stock == selected_) {
        InvalidateRect(hwnd_, &headerRect_, FALSE);
        InvalidateRect(hwnd_, &statsRect_, FALSE);
    }
}

void App::OnChartReady(size_t stock, size_t range) {
    if (stock != selected_ || range != range_) { return; }  // stale
    if (!fetcher_.CopyChart(stock, range, *chart_)) { return; }
    chartLoading_ = false;
    if (!chart_->valid) { SetStatus(cfg_.stocks[stock].symbol + L": " + chart_->error); }
    InvalidateRect(hwnd_, &chartRect_, FALSE);
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
            g.Clear(kBgColor);
            PaintHeader(g);
            ChartInput in;
            in.data    = chartLoading_ ? nullptr : chart_.get();
            in.range   = &kRanges[range_];
            in.candles = candles_;
            in.hoverX  = hoverX_;
            in.hoverY  = hoverY_;
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
    const RectF rc = ToRectF(headerRect_);

    const Font nameFont(L"Segoe UI", FontPx(15.0f, scale_), FontStyleBold, UnitPixel);
    const Font subFont(L"Segoe UI", FontPx(9.5f, scale_), FontStyleRegular, UnitPixel);
    const Font priceFont(L"Segoe UI", FontPx(22.0f, scale_), FontStyleBold, UnitPixel);
    const Font chgFont(L"Segoe UI", FontPx(10.5f, scale_), FontStyleRegular, UnitPixel);
    SolidBrush dark(kTextDark);
    SolidBrush grey(kTextGrey);
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
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    const std::wstring price = FormatPrice(pc.last) + L" " + sum.meta.currency;
    g.DrawString(price.c_str(), -1, &priceFont, RectF(rc.X, rc.Y - 2.0f * scale_, rc.Width, 34.0f * scale_), &right, &dark);
    SolidBrush chgBrush(pc.change >= 0.0 ? kUp : kDown);
    const std::wstring chg = FormatChange(pc.change, pc.pct);
    g.DrawString(chg.c_str(), -1, &chgFont, RectF(rc.X, rc.Y + 32.0f * scale_, rc.Width, 20.0f * scale_), &right, &chgBrush);
}

void App::PaintStats(Graphics& g) {
    assert(selected_ < cfg_.stockCount);
    const QuoteData& sum = (*summaries_)[selected_];
    const RectF rc = ToRectF(statsRect_);
    Pen divider(kListDivider, 1.0f);
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
    const std::wstring dash = L"-";

    struct Cell { const wchar_t* label; std::wstring value; };
    const std::array<Cell, 8> cells = {{
        { L"Open",       today ? FormatPrice(today->open) : dash },
        { L"High",       m.dayHigh > 0.0 ? FormatPrice(m.dayHigh) : (today ? FormatPrice(today->high) : dash) },
        { L"Low",        m.dayLow  > 0.0 ? FormatPrice(m.dayLow)  : (today ? FormatPrice(today->low)  : dash) },
        { L"Prev close", pc.valid ? FormatPrice(pc.prev) : dash },
        { L"Volume",     m.dayVolume > 0.0 ? FormatVolume(m.dayVolume) : (today ? FormatVolume(today->volume) : dash) },
        { L"52W high",   m.wk52High > 0.0 ? FormatPrice(m.wk52High) : dash },
        { L"52W low",    m.wk52Low  > 0.0 ? FormatPrice(m.wk52Low)  : dash },
        { L"Currency",   m.currency.empty() ? dash : m.currency },
    }};

    const Font labelFont(L"Segoe UI", FontPx(8.5f, scale_), FontStyleRegular, UnitPixel);
    const Font valueFont(L"Segoe UI", FontPx(11.0f, scale_), FontStyleBold, UnitPixel);
    SolidBrush grey(kTextGrey);
    SolidBrush dark(kTextDark);
    const float cellW = rc.Width / 4.0f;
    const float rowH  = (rc.Height - 8.0f * scale_) / 2.0f;
    for (size_t i = 0; i < cells.size(); ++i) {
        const float cx = rc.X + static_cast<float>(i % 4) * cellW;
        const float cy = rc.Y + 8.0f * scale_ + static_cast<float>(i / 4) * rowH;
        g.DrawString(cells[i].label, -1, &labelFont, PointF(cx, cy), &grey);
        g.DrawString(cells[i].value.c_str(), -1, &valueFont, PointF(cx, cy + 14.0f * scale_), &dark);
    }
}

void App::PaintStatus(Graphics& g) {
    const Font font(L"Segoe UI", FontPx(8.5f, scale_), FontStyleRegular, UnitPixel);
    SolidBrush grey(kTextGrey);
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
    const RectF rc = ToRectF(rcItem);

    SolidBrush bg(selected ? kListSel : kListBg);
    g.FillRectangle(&bg, rc);
    Pen divider(kListDivider, 1.0f);
    g.DrawLine(&divider, rc.X, rc.Y + rc.Height - 1, rc.X + rc.Width, rc.Y + rc.Height - 1);

    const Font symFont(L"Segoe UI", FontPx(10.5f, scale_), FontStyleBold, UnitPixel);
    const Font subFont(L"Segoe UI", FontPx(8.5f, scale_), FontStyleRegular, UnitPixel);
    const Font priceFont(L"Segoe UI", FontPx(10.5f, scale_), FontStyleRegular, UnitPixel);
    SolidBrush dark(kTextDark);
    SolidBrush grey(kTextGrey);
    const float pad = 8.0f * scale_;
    StringFormat left;
    left.SetFormatFlags(StringFormatFlagsNoWrap);
    left.SetTrimming(StringTrimmingEllipsisCharacter);
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    right.SetFormatFlags(StringFormatFlagsNoWrap);

    const float textW = rc.Width * 0.6f;
    g.DrawString(entry.symbol.c_str(), -1, &symFont, RectF(rc.X + pad, rc.Y + 5.0f * scale_, textW, 18.0f * scale_), &left, &dark);
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
    SolidBrush chgBrush(pc.change >= 0.0 ? kUp : kDown);
    g.DrawString(FormatChange(pc.change, pc.pct).c_str(), -1, &subFont, chgRc, &right, &chgBrush);
}

} // namespace st
