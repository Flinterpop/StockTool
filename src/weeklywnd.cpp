// View > Weekly 3D window. See weeklywnd.h.
#include "weeklywnd.h"

#include "resource.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <cassert>

namespace st {

namespace {

constexpr wchar_t kClass[]          = L"StockToolWeekly";
constexpr DWORD   kDwmDarkMode      = 20;   // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr int     kRangeId          = 100;  // + index into kWeeklyRanges
constexpr int     kWaterfallId      = 120;
constexpr int     kSurfaceId        = 121;
constexpr int     kWeek2dId         = 122;
constexpr int     kPriceId          = 130;
constexpr int     kPctId            = 131;
constexpr int     kVolumeId         = 132;
constexpr int     kRefreshId        = 140;

void FreeDib(HDC& dc, HBITMAP& bmp, HGDIOBJ& old) {
    if (dc == nullptr) { return; }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    dc = nullptr; bmp = nullptr; old = nullptr;
}

bool MakeDib(HDC ref, int w, int h, HDC& dc, HBITMAP& bmp, HGDIOBJ& old) {
    assert(w > 0 && h > 0);
    dc  = CreateCompatibleDC(ref);
    bmp = CreateCompatibleBitmap(ref, w, h);
    if (dc == nullptr || bmp == nullptr) {
        if (bmp != nullptr) { DeleteObject(bmp); }
        if (dc != nullptr)  { DeleteDC(dc); }
        dc = nullptr; bmp = nullptr;
        return false;
    }
    old = SelectObject(dc, bmp);
    return true;
}

} // namespace

WeeklyWindow::WeeklyWindow(HINSTANCE inst, Fetcher& fetcher)
    : inst_(inst), fetcher_(fetcher),
      data_(std::make_unique<QuoteData>()),
      grid_(std::make_unique<WeekGrid>()),
      scratch_(std::make_unique<WeeklyScratch>()) {
    assert(inst_ != nullptr);
}

WeeklyWindow::~WeeklyWindow() {
    if (hwnd_ != nullptr) { DestroyWindow(hwnd_); }
    FreeBuffers();
    if (font_ != nullptr)    { DeleteObject(font_); }
    if (bgBrush_ != nullptr) { DeleteObject(bgBrush_); }
}

int WeeklyWindow::Px(int dip) const { return static_cast<int>(static_cast<float>(dip) * scale_ + 0.5f); }

// ---------------------------------------------------------------------------
// Public

void WeeklyWindow::Show(HWND owner, const std::wstring& symbol, const std::wstring& name, const Theme& theme) {
    assert(owner != nullptr && !symbol.empty());
    theme_ = &theme;
    if (hwnd_ == nullptr) {
        symbol_.clear();
        Create(owner);
        if (hwnd_ == nullptr) { return; }
    }
    ApplyTheme();
    if (symbol != symbol_ || !haveData_) {
        symbol_ = symbol;
        name_   = name;
        Request();
    }
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void WeeklyWindow::SetSymbol(const std::wstring& symbol, const std::wstring& name) {
    if (hwnd_ == nullptr || symbol.empty()) { return; }
    if (symbol == symbol_ && haveData_) { return; }
    symbol_ = symbol;
    name_   = name;
    Request();
}

void WeeklyWindow::SetTheme(const Theme& theme) {
    theme_ = &theme;
    if (hwnd_ != nullptr) { ApplyTheme(); }
}

// ---------------------------------------------------------------------------
// Creation and layout

void WeeklyWindow::Create(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = &WeeklyWindow::Proc;
        wc.hInstance     = inst_;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(inst_, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) { return; }
    }
    scale_ = static_cast<float>(GetDpiForWindow(owner)) / 96.0f;
    RECT own{};
    GetWindowRect(owner, &own);
    const int w = max(Px(1000), static_cast<int>((own.right - own.left) * 0.75));
    const int h = max(Px(520), static_cast<int>((own.bottom - own.top) * 0.75));
    const int x = own.left + ((own.right - own.left) - w) / 2;
    const int y = own.top + ((own.bottom - own.top) - h) / 2;
    hwnd_ = CreateWindowExW(0, kClass, L"StockTool - weekly 3D", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            x, y, w, h, owner, nullptr, inst_, this);
    if (hwnd_ == nullptr) { return; }
    scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    CreateControls();
    Layout();
}

void WeeklyWindow::CreateControls() {
    assert(hwnd_ != nullptr);
    auto button = [this](const wchar_t* text, DWORD style, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10,
                                 hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        assert(h != nullptr);
        return h;
    };
    const DWORD radio = BS_AUTORADIOBUTTON | BS_PUSHLIKE;
    for (size_t i = 0; i < kWeeklyRanges.size(); ++i) {
        rangeBtns_[i] = button(kWeeklyRanges[i].label, radio | (i == 0 ? WS_GROUP : 0), kRangeId + static_cast<int>(i));
    }
    waterfallBtn_ = button(L"Waterfall", radio | WS_GROUP, kWaterfallId);
    surfaceBtn_   = button(L"Surface", radio, kSurfaceId);
    week2dBtn_    = button(L"Week 2D", radio, kWeek2dId);
    priceBtn_     = button(L"Price", radio | WS_GROUP, kPriceId);
    pctBtn_       = button(L"% of week", radio, kPctId);
    volumeBtn_    = button(L"Volume", radio, kVolumeId);
    refreshBtn_   = button(L"Refresh (F5)", BS_PUSHBUTTON | WS_GROUP, kRefreshId);
    if (font_ != nullptr) { DeleteObject(font_); }
    font_ = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    EnumChildWindows(hwnd_, [](HWND child, LPARAM lp) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font_));
    SyncButtons();
}

void WeeklyWindow::SyncButtons() {
    for (size_t i = 0; i < kWeeklyRanges.size(); ++i) {
        SendMessageW(rangeBtns_[i], BM_SETCHECK, i == range_ ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    SendMessageW(waterfallBtn_, BM_SETCHECK, style_ == WeekStyle::Waterfall ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(surfaceBtn_, BM_SETCHECK, style_ == WeekStyle::Surface ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(week2dBtn_, BM_SETCHECK, style_ == WeekStyle::Week2D ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(priceBtn_, BM_SETCHECK, mode_ == WeekValue::Price ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(pctBtn_, BM_SETCHECK, mode_ == WeekValue::PctChange ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(volumeBtn_, BM_SETCHECK, mode_ == WeekValue::Volume ? BST_CHECKED : BST_UNCHECKED, 0);
}

void WeeklyWindow::Layout() {
    assert(hwnd_ != nullptr);
    const int h = Px(26), top = Px(6), gap = Px(4), groupGap = Px(16);
    int x = Px(8);
    auto place = [&](HWND b, int w) {
        MoveWindow(b, x, top, w, h, TRUE);
        x += w + gap;
    };
    for (size_t i = 0; i < kWeeklyRanges.size(); ++i) { place(rangeBtns_[i], kWeeklyRanges[i].intraday ? Px(40) : Px(68)); }
    x += groupGap;
    place(waterfallBtn_, Px(76));
    place(surfaceBtn_, Px(66));
    place(week2dBtn_, Px(70));
    x += groupGap;
    place(priceBtn_, Px(56));
    place(pctBtn_, Px(80));
    place(volumeBtn_, Px(64));
    x += groupGap;
    place(refreshBtn_, Px(92));
    toolbarH_ = top + h + Px(4);
    dirty_ = true;
}

Gdiplus::RectF WeeklyWindow::PlotRect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float w = static_cast<float>(max(1L, rc.right));
    const float h = static_cast<float>(max(1L, rc.bottom - toolbarH_));
    return Gdiplus::RectF(0.0f, static_cast<float>(toolbarH_), w, h);
}

void WeeklyWindow::ApplyTheme() {
    assert(hwnd_ != nullptr && theme_ != nullptr);
    if (bgBrush_ != nullptr) { DeleteObject(bgBrush_); }
    bgBrush_ = CreateSolidBrush(theme_->bgRef);
    const BOOL dark = theme_->dark ? TRUE : FALSE;
    const HRESULT hr = DwmSetWindowAttribute(hwnd_, kDwmDarkMode, &dark, sizeof(dark));
    (void)hr;   // older builds ignore it; the client area is still themed
    const wchar_t* sub = theme_->dark ? L"DarkMode_Explorer" : L"Explorer";
    EnumChildWindows(hwnd_, [](HWND child, LPARAM lp) -> BOOL {
        SetWindowTheme(child, reinterpret_cast<const wchar_t*>(lp), nullptr);
        return TRUE;
    }, reinterpret_cast<LPARAM>(sub));
    dirty_ = true;
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// Data

void WeeklyWindow::Request() {
    assert(hwnd_ != nullptr && range_ < kWeeklyRanges.size());
    haveData_     = false;
    grid_->weeks  = 0;
    hoverX_ = hoverY_ = -1;
    const RangeSpec& spec = kWeeklyRanges[range_];
    message_ = L"Loading " + symbol_ + L" (" + spec.label + L", " + spec.interval + L" bars)…";
    if (!fetcher_.EnqueueWeekly(symbol_, range_, hwnd_)) { message_ = L"Could not queue the request; try Refresh"; }
    Rebuild();
}

void WeeklyWindow::OnDataReady(size_t range) {
    if (range != range_) { return; }                                // an older request
    if (!fetcher_.CopyWeekly(symbol_, range_, *data_)) { return; }  // for another symbol
    haveData_ = true;
    Rebuild();
}

// Re-slices the fetched bars (also when only Price / % changes) and retitles.
void WeeklyWindow::Rebuild() {
    const RangeSpec& spec = kWeeklyRanges[range_];
    if (haveData_) {
        std::wstring err;
        if (!BuildWeekGrid(*data_, spec, mode_, *grid_, err)) {
            grid_->weeks = 0;
            message_ = symbol_ + L": " + err;
        }
    }
    title_ = symbol_;
    if (!name_.empty() && name_ != symbol_) { title_ += L"  " + name_; }
    title_ += L"   " + std::wstring(spec.label) + (spec.intraday ? L" · " + std::wstring(spec.interval) + L" bars"
                                                                : std::wstring(L" · daily bars"));
    if (grid_->weeks > 0) {
        title_ += L" · " + std::to_wstring(grid_->weeks) + L" weeks";
        weeksAgo_ = min(weeksAgo_, grid_->weeks - 1);   // a shorter range has fewer weeks to step back through
    }
    if (haveData_ && !data_->source.empty()) { title_ += L" · via " + data_->source; }
    SetWindowTextW(hwnd_, (L"StockTool - weekly 3D - " + symbol_).c_str());
    dirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

WeeklyInput WeeklyWindow::Input() const {
    WeeklyInput in;
    in.grid    = grid_->weeks > 0 ? grid_.get() : nullptr;
    in.style   = style_;
    in.mode    = mode_;
    in.view    = view_;
    in.title   = title_.c_str();
    in.message = message_.c_str();
    in.weeksAgo = weeksAgo_;
    in.theme   = theme_;
    // The plot is drawn at (0, 0) of its own bitmap, so hover is relative to it.
    in.hoverX  = hoverX_ < 0 ? -1 : hoverX_;
    in.hoverY  = hoverY_ < 0 ? -1 : hoverY_ - toolbarH_;
    return in;
}

// ---------------------------------------------------------------------------
// Painting

void WeeklyWindow::FreeBuffers() {
    FreeDib(memDC_, memBmp_, memOld_);
    FreeDib(baseDC_, baseBmp_, baseOld_);
    memW_ = memH_ = baseW_ = baseH_ = 0;
}

void WeeklyWindow::RenderBase(int w, int h) {
    assert(baseDC_ != nullptr && w > 0 && h > 0);
    Gdiplus::Graphics g(baseDC_);
    DrawWeeklyBase(g, Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)), Input(), scale_,
                   *scratch_);
    dirty_ = false;
}

void WeeklyWindow::Paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = max(1L, rc.right), h = max(1L, rc.bottom);
    const Gdiplus::RectF plot = PlotRect();
    const int pw = static_cast<int>(plot.Width), ph = static_cast<int>(plot.Height);
    if (memW_ != w || memH_ != h || baseW_ != pw || baseH_ != ph) {
        FreeBuffers();
        if (!MakeDib(hdc, w, h, memDC_, memBmp_, memOld_) || !MakeDib(hdc, pw, ph, baseDC_, baseBmp_, baseOld_)) {
            FreeBuffers();
            EndPaint(hwnd_, &ps);
            return;
        }
        memW_ = w; memH_ = h; baseW_ = pw; baseH_ = ph;
        dirty_ = true;
    }
    if (dirty_ && theme_ != nullptr) { RenderBase(pw, ph); }
    RECT bar{ 0, 0, w, toolbarH_ };
    FillRect(memDC_, &bar, bgBrush_);
    BitBlt(memDC_, 0, toolbarH_, pw, ph, baseDC_, 0, 0, SRCCOPY);
    if (theme_ != nullptr && hoverX_ >= 0 && !dragging_) {
        Gdiplus::Graphics g(memDC_);
        g.TranslateTransform(0.0f, static_cast<float>(toolbarH_));
        DrawWeeklyOverlay(g, Gdiplus::RectF(0.0f, 0.0f, plot.Width, plot.Height), Input(), scale_);
    }
    BitBlt(hdc, 0, 0, w, h, memDC_, 0, 0, SRCCOPY);
    EndPaint(hwnd_, &ps);
}

// ---------------------------------------------------------------------------
// Input

void WeeklyWindow::OnCommand(int id) {
    if (id >= kRangeId && id < kRangeId + static_cast<int>(kWeeklyRanges.size())) {
        const size_t r = static_cast<size_t>(id - kRangeId);
        if (r != range_) { range_ = r; Request(); }
    } else if (id == kWaterfallId || id == kSurfaceId || id == kWeek2dId) {
        style_ = (id == kSurfaceId) ? WeekStyle::Surface : (id == kWeek2dId) ? WeekStyle::Week2D : WeekStyle::Waterfall;
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (id == kPriceId || id == kPctId || id == kVolumeId) {
        mode_ = (id == kPctId) ? WeekValue::PctChange : (id == kVolumeId) ? WeekValue::Volume : WeekValue::Price;
        Rebuild();
    } else if (id == kRefreshId) {
        Request();
    }
    SyncButtons();
    SetFocus(hwnd_);   // keep Esc / F5 / arrows working after a click
}

void WeeklyWindow::StepWeek(int delta) {
    const size_t oldest = grid_->weeks > 0 ? grid_->weeks - 1 : 0;
    if (delta < 0) {
        const size_t back = static_cast<size_t>(-delta);
        weeksAgo_ = weeksAgo_ > back ? weeksAgo_ - back : 0;
    } else {
        weeksAgo_ = min(oldest, weeksAgo_ + static_cast<size_t>(delta));
    }
    dirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WeeklyWindow::OnMouseMove(int x, int y) {
    if (dragging_) {
        RotateWeekView(view_, static_cast<float>(x - lastX_), static_cast<float>(y - lastY_));
        lastX_ = x;
        lastY_ = y;
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!tracking_) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        tracking_ = TrackMouseEvent(&tme) != FALSE;
    }
    hoverX_ = (y >= toolbarH_) ? x : -1;
    hoverY_ = (y >= toolbarH_) ? y : -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void WeeklyWindow::OnDpiChanged(WPARAM wp, LPARAM lp) {
    scale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
    const RECT* r = reinterpret_cast<const RECT*>(lp);
    SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (font_ != nullptr) { DeleteObject(font_); }
    font_ = CreateFontW(-Px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    EnumChildWindows(hwnd_, [](HWND child, LPARAM f) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font_));
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK WeeklyWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WeeklyWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = static_cast<WeeklyWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<WeeklyWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) { return DefWindowProcW(hwnd, msg, wp, lp); }
    return self->Handle(msg, wp, lp);
}

LRESULT WeeklyWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:         Paint(); return 0;
    case WM_ERASEBKGND:    return 1;
    case WM_SIZE:          if (rangeBtns_[0] != nullptr) { Layout(); } InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_COMMAND:       if (HIWORD(wp) == BN_CLICKED) { OnCommand(LOWORD(wp)); } return 0;
    case WM_APP_WEEKLY_READY: OnDataReady(static_cast<size_t>(lp)); return 0;
    case WM_MOUSEMOVE:     OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        hoverX_ = hoverY_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        if (GET_Y_LPARAM(lp) < toolbarH_ || style_ == WeekStyle::Week2D) { return 0; }   // nothing to rotate in 2D
        dragging_ = true;
        lastX_ = GET_X_LPARAM(lp);
        lastY_ = GET_Y_LPARAM(lp);
        SetCapture(hwnd_);
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) { dragging_ = false; ReleaseCapture(); InvalidateRect(hwnd_, nullptr, FALSE); }
        return 0;
    case WM_CAPTURECHANGED: dragging_ = false; return 0;
    case WM_LBUTTONDBLCLK:
        view_  = WeekView{};
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        if (style_ == WeekStyle::Week2D) {   // wheel up = further back in time
            StepWeek(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
            return 0;
        }
        ZoomWeekView(view_, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA);
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN: {
        const float step = 15.0f / 0.4f;   // 15 degrees of azimuth per press
        if (style_ == WeekStyle::Week2D) {
            if (wp == VK_LEFT || wp == VK_UP)    { StepWeek(1); return 0; }
            if (wp == VK_RIGHT || wp == VK_DOWN) { StepWeek(-1); return 0; }
            if (wp == VK_HOME)                   { StepWeek(-static_cast<int>(kMaxWeeks)); return 0; }
            if (wp == VK_END)                    { StepWeek(static_cast<int>(kMaxWeeks)); return 0; }
        }
        switch (wp) {
        case VK_ESCAPE: DestroyWindow(hwnd_); return 0;
        case VK_F5:     Request(); return 0;
        case VK_LEFT:   RotateWeekView(view_, -step, 0.0f); break;
        case VK_RIGHT:  RotateWeekView(view_, step, 0.0f); break;
        case VK_UP:     RotateWeekView(view_, 0.0f, -step / 2.0f); break;
        case VK_DOWN:   RotateWeekView(view_, 0.0f, step / 2.0f); break;
        default:        return 0;
        }
        dirty_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
        if (theme_ == nullptr) { break; }
        SetBkColor(reinterpret_cast<HDC>(wp), theme_->bgRef);
        SetTextColor(reinterpret_cast<HDC>(wp), theme_->textRef);
        return reinterpret_cast<LRESULT>(bgBrush_);
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = Px(960);
        mmi->ptMinTrackSize.y = Px(420);
        return 0;
    }
    case WM_DPICHANGED:    OnDpiChanged(wp, lp); return 0;
    case WM_NCDESTROY: {
        FreeBuffers();
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        rangeBtns_.fill(nullptr);
        waterfallBtn_ = surfaceBtn_ = week2dBtn_ = priceBtn_ = pctBtn_ = volumeBtn_ = refreshBtn_ = nullptr;
        dragging_ = tracking_ = false;
        haveData_ = false;
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace st
