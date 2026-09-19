// Price/volume chart rendering with GDI+.
#pragma once

#include "common.h"

#include <windows.h>
#include <objidl.h>  // IStream/PROPID for gdiplus.h (excluded by WIN32_LEAN_AND_MEAN)

#include <algorithm>
using std::max;  // gdiplus.h expects min/max in scope (NOMINMAX is defined)
using std::min;
#pragma warning(push, 1)
#include <gdiplus.h>
#pragma warning(pop)

namespace st {

struct ChartInput {
    const QuoteData* data    = nullptr;  // nullptr while loading
    const RangeSpec* range   = nullptr;
    bool             candles = false;
    int              hoverX  = -1;       // px relative to chart rect, -1 = none
    int              hoverY  = -1;
};

// Draws the chart into `rc`. `scale` is the DPI factor (1.0 = 96 dpi).
void DrawChart(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const ChartInput& in, float scale);

// Font helper shared with the panels: `pt` is a point size at 96 dpi.
Gdiplus::REAL FontPx(float pt, float scale);

} // namespace st
