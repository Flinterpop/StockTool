// Price/volume chart rendering with GDI+.
#pragma once

#include "common.h"
#include "gdiplus_inc.h"
#include "theme.h"

namespace st {

struct ChartOptions {
    bool candles   = false;
    bool sma20     = false;
    bool sma50     = false;
    bool bollinger = false;
    bool rsi       = false;
    bool inset     = true;
};

// One line in the compare overlay.
struct CompareEntry {
    const QuoteData* data   = nullptr;
    const wchar_t*   symbol = nullptr;
};

struct ChartInput {
    const QuoteData* data       = nullptr;  // nullptr while loading
    const RangeSpec* range      = nullptr;
    ChartOptions     opts;
    const QuoteData* inset      = nullptr;  // trend inset data (nullptr = loading/off)
    const wchar_t*   insetLabel = nullptr;  // e.g. L"1Y"
    float            insetX     = 0.0f;     // inset position as a fraction (0..1) of the
    float            insetY     = 0.0f;     // free space in the plot; (0,0) = top-left
    bool             compare    = false;    // draw `entries` as % change instead of `data`
    const CompareEntry* entries = nullptr;
    size_t           entryCount = 0;
    int              hoverX     = -1;       // same coordinate space as `rc` (client px), -1 = none
    int              hoverY     = -1;
    const Theme*     theme      = nullptr;
};

// Draws the chart into `rc`. `scale` is the DPI factor (1.0 = 96 dpi).
// DrawChart = DrawChartBase + DrawChartOverlay. The base (everything that
// does not depend on the mouse) is expensive and can be cached by the
// caller; the overlay (hover crosshair/tooltip) is cheap and redrawn per
// mouse move on top of the cached base.
void DrawChart(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const ChartInput& in, float scale);
void DrawChartBase(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const ChartInput& in, float scale);
void DrawChartOverlay(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const ChartInput& in, float scale);

// Where the trend inset is drawn for this input, in the coordinate space of
// `rc` (for hit-testing/dragging). False when the inset is not shown (off,
// compare mode, chart too small).
bool ChartInsetRect(const Gdiplus::RectF& rc, const ChartInput& in, float scale, Gdiplus::RectF& out);

// Converts a desired inset top-left (same space as `rc`) into the clamped
// fractional position that ChartInsetRect maps back to the same place.
void ChartInsetFractionFor(const Gdiplus::RectF& rc, const ChartInput& in, float scale,
                           float px, float py, float& fx, float& fy);

// Font helper shared with the panels: `pt` is a point size at 96 dpi.
Gdiplus::REAL FontPx(float pt, float scale);

} // namespace st
