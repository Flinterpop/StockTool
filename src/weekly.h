// Per-week view (View > Weekly 3D): one ticker's bars sliced into calendar
// weeks on a common time-of-week grid, drawn as a waterfall of curves (one
// per week, back to front) or as a shaded 3D surface. UI-free, so the
// slicing and the projection are unit-tested; the window is weeklywnd.cpp.
#pragma once

#include "common.h"
#include "gdiplus_inc.h"
#include "theme.h"

#include <array>
#include <cstdint>
#include <string>

namespace st {

// What the weekly window can ask for. Intraday intervals give each week a
// real shape (Yahoo serves 30m for 60 days and 60m for 730 days); the daily
// rows give five points a week over a longer span. Every range stays under
// kMaxPoints bars for an exchange-hours ticker.
constexpr std::array<RangeSpec, 6> kWeeklyRanges = {{
    { L"1M",       L"1mo", L"30m", true  },
    { L"3M",       L"3mo", L"60m", true  },
    { L"6M",       L"6mo", L"60m", true  },
    { L"1Y",       L"1y",  L"60m", true  },
    { L"1Y daily", L"1y",  L"1d",  false },
    { L"2Y daily", L"2y",  L"1d",  false },
}};
constexpr size_t kWeeklyDefaultRange = 3;   // 1Y hourly

constexpr size_t kMaxWeeks     = 112;       // 2Y daily needs 105; older weeks are dropped
constexpr size_t kMaxWeekSlots = 7 * 48;    // a 7-day market at 30m
constexpr size_t kMaxWeekCells = kMaxWeeks * kMaxWeekSlots;

// PctChange: vs the previous week's last close. Volume: shares traded in each bar.
enum class WeekValue : uint8_t { Price, PctChange, Volume };
// Week2D: an ordinary line chart of one week, stepped through with the wheel.
enum class WeekStyle : uint8_t { Waterfall, Surface, Week2D };

// One weekday across every week in the grid: how far it usually moves
// (close vs the previous trading close) and how much trades on it.
struct DayStat {
    size_t n          = 0;     // days that traded
    double absMovePct = 0.0;   // mean |move|
    double movePct    = 0.0;   // mean signed move (a drift towards up or down)
    double volume     = 0.0;   // mean shares traded that day
    double volRatio   = 0.0;   // volume / the mean over all weekdays (1 = a typical day)
};

// Week w, slot s lives at cell w * Slots() + s. Slot s is day s / slotsPerDay
// (Mon = 0) and bar s % slotsPerDay of that day, counted from the day's first
// bar, so a DST change (the provider gives only today's UTC offset) does not
// shift half the year by one slot.
struct WeekGrid {
    size_t  weeks       = 0;
    size_t  days        = 5;        // 7 when the series trades at weekends
    size_t  slotsPerDay = 1;
    int64_t slotSec     = 86400;    // bar interval
    int32_t gmtOffsetSec = 0;
    double  lo = 0.0;               // range of `value` over the grid
    double  hi = 0.0;
    std::array<int64_t, kMaxWeeks> weekStart{};   // Unix seconds of the local Monday 00:00
    std::array<double, kMaxWeeks>  base{};        // previous week's last close (PctChange reference)
    std::array<double, kMaxWeekCells>  price{};   // close; NaN = no data; gaps held at the last trade
    std::array<double, kMaxWeekCells>  value{};   // what is plotted: price or % vs base
    std::array<int64_t, kMaxWeekCells> time{};    // bar time; 0 = a held (filled) cell
    std::array<double, kMaxWeekCells>  volume{};  // shares in that bar; 0 for held cells
    std::array<DayStat, 7> dayStats{};            // Mon..Sun over all weeks (first `days` used)

    size_t Slots() const { return days * slotsPerDay; }
};

// "30m" -> 1800, "60m"/"1h" -> 3600, "1d" -> 86400, "1wk" -> 604800; 0 on junk.
int64_t IntervalSeconds(const wchar_t* interval);

// Slices `q` into weeks. Interior gaps (holidays, missing bars) hold the
// last trade; cells before the first bar and after the last stay NaN. False
// with `err` when there is nothing to plot.
bool BuildWeekGrid(const QuoteData& q, const RangeSpec& spec, WeekValue mode, WeekGrid& out, std::wstring& err);

// Camera: azimuth turns the box about the vertical axis, elevation tilts
// it towards the viewer; zoom scales the fitted box.
struct WeekView {
    float azimuthDeg   = -32.0f;
    float elevationDeg = 28.0f;
    float zoom         = 1.0f;
};
constexpr float kMinElevation = 5.0f;
constexpr float kMaxElevation = 85.0f;
constexpr float kMinZoom      = 0.4f;
constexpr float kMaxZoom      = 3.0f;

// Applies a mouse drag (px) / wheel notches to the camera, clamped.
void RotateWeekView(WeekView& v, float dxPx, float dyPx);
void ZoomWeekView(WeekView& v, float notches);

// Painter's-algorithm scratch for the surface (one depth per quad), owned by
// the caller so drawing never allocates.
struct WeeklyScratch {
    std::array<uint32_t, kMaxWeekCells> order{};
    std::array<float, kMaxWeekCells>    depth{};
};

struct WeeklyInput {
    const WeekGrid* grid    = nullptr;   // nullptr = nothing yet: `message` is shown
    WeekStyle       style   = WeekStyle::Waterfall;
    WeekValue       mode    = WeekValue::Price;
    WeekView        view;
    const wchar_t*  title   = L"";
    const wchar_t*  message = L"";
    size_t          weeksAgo = 0;         // Week2D: 0 = the newest week in the grid
    int             hoverX  = -1;         // client px, -1 = none
    int             hoverY  = -1;
    const Theme*    theme   = nullptr;
};

// Grid row shown for `weeksAgo` (clamped to the oldest week).
size_t WeekRowFor(const WeekGrid& g, size_t weeksAgo);
std::wstring WeeksAgoLabel(size_t weeksAgo);   // "This week", "Last week", "3 weeks ago"

// Base = everything but the hover (cacheable); overlay = hovered week and tooltip.
void DrawWeeklyBase(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const WeeklyInput& in, float scale,
                    WeeklyScratch& scratch);
void DrawWeeklyOverlay(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const WeeklyInput& in, float scale);

// Nearest plotted cell to (x, y) within `radiusPx`; false when none. In
// Week2D the cell is the shown week's slot under x, anywhere over the plot.
bool WeeklyHit(const Gdiplus::RectF& rc, const WeeklyInput& in, float scale, float x, float y, float radiusPx,
               size_t& week, size_t& slot);

// Week2D: screen y of `price` on the right-hand price axis; false when that
// axis has no scale (no previous close in % mode, no prices in Volume mode).
bool WeeklyPriceY(const Gdiplus::RectF& rc, const WeeklyInput& in, float scale, double price, float& y);

// Screen position of cell (week, slot) at the cell's value (for tests and hover).
bool WeeklyProject(const Gdiplus::RectF& rc, const WeeklyInput& in, float scale, size_t week, size_t slot,
                   Gdiplus::PointF& out);

} // namespace st
