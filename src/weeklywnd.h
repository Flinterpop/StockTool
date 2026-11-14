// View > Weekly 3D: a second top-level window showing the selected ticker's
// weeks as a waterfall or a surface (weekly.h does the slicing and drawing).
#pragma once

#include "fetcher.h"
#include "theme.h"
#include "weekly.h"

#include <windows.h>

#include <memory>
#include <string>

namespace st {

class WeeklyWindow {
public:
    WeeklyWindow(HINSTANCE inst, Fetcher& fetcher);
    ~WeeklyWindow();
    WeeklyWindow(const WeeklyWindow&) = delete;
    WeeklyWindow& operator=(const WeeklyWindow&) = delete;

    // Opens (or raises) the window on `symbol`.
    void Show(HWND owner, const std::wstring& symbol, const std::wstring& name, const Theme& theme);
    // Follows the main window's selection; ignored while closed.
    void SetSymbol(const std::wstring& symbol, const std::wstring& name);
    void SetTheme(const Theme& theme);
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Create(HWND owner);
    void CreateControls();
    void Layout();
    void ApplyTheme();
    void OnCommand(int id);
    void OnDataReady(size_t range);
    void Request();
    void Rebuild();
    void Paint();
    void RenderBase(int w, int h);
    void FreeBuffers();
    void OnMouseMove(int x, int y);
    void OnDpiChanged(WPARAM wp, LPARAM lp);
    void SyncButtons();
    void StepWeek(int delta);   // Week2D: + = further back
    int  Px(int dip) const;
    Gdiplus::RectF PlotRect() const;
    WeeklyInput Input() const;

    HINSTANCE inst_    = nullptr;
    Fetcher&  fetcher_;
    HWND      hwnd_    = nullptr;
    HFONT     font_    = nullptr;
    HBRUSH    bgBrush_ = nullptr;
    const Theme* theme_ = nullptr;
    float     scale_   = 1.0f;
    int       toolbarH_ = 0;

    std::array<HWND, kWeeklyRanges.size()> rangeBtns_{};
    HWND waterfallBtn_ = nullptr;
    HWND surfaceBtn_   = nullptr;
    HWND week2dBtn_    = nullptr;
    HWND priceBtn_     = nullptr;
    HWND pctBtn_       = nullptr;
    HWND volumeBtn_    = nullptr;
    HWND refreshBtn_   = nullptr;

    std::wstring symbol_;
    std::wstring name_;
    std::wstring title_;
    std::wstring message_;
    size_t    range_  = kWeeklyDefaultRange;
    WeekStyle style_  = WeekStyle::Waterfall;
    WeekValue mode_   = WeekValue::Price;
    WeekView  view_;
    size_t    weeksAgo_ = 0;   // Week2D: which week (0 = the newest)
    bool      haveData_ = false;

    std::unique_ptr<QuoteData>     data_;
    std::unique_ptr<WeekGrid>      grid_;
    std::unique_ptr<WeeklyScratch> scratch_;

    // Mouse: drag rotates, hover shows a cell.
    bool dragging_ = false;
    bool tracking_ = false;
    int  lastX_    = 0;
    int  lastY_    = 0;
    int  hoverX_   = -1;
    int  hoverY_   = -1;

    // Back buffer (whole client) and the cached base layer (plot area).
    HDC     memDC_   = nullptr;
    HBITMAP memBmp_  = nullptr;
    HGDIOBJ memOld_  = nullptr;
    int     memW_    = 0;
    int     memH_    = 0;
    HDC     baseDC_  = nullptr;
    HBITMAP baseBmp_ = nullptr;
    HGDIOBJ baseOld_ = nullptr;
    int     baseW_   = 0;
    int     baseH_   = 0;
    bool    dirty_   = true;
};

} // namespace st
