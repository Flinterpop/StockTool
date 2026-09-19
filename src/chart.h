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
    bool             compare    = false;    // draw `entries` as % change instead of `data`
    const CompareEntry* entries = nullptr;
    size_t           entryCount = 0;
    int              hoverX     = -1;       // px relative to chart rect, -1 = none
    int              hoverY     = -1;
    const Theme*     theme      = nullptr;
};

// Draws the chart into `rc`. `scale` is the DPI factor (1.0 = 96 dpi).
void DrawChart(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const ChartInput& in, float scale);

// Font helper shared with the panels: `pt` is a point size at 96 dpi.
Gdiplus::REAL FontPx(float pt, float scale);

} // namespace st
