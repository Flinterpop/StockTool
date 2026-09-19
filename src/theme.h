// Light/dark colour palettes.
#pragma once

#include "gdiplus_inc.h"

namespace st {

struct Theme {
    bool dark = false;
    Gdiplus::Color bg;           // window background
    Gdiplus::Color text;
    Gdiplus::Color textMuted;
    Gdiplus::Color grid;
    Gdiplus::Color line;         // price line
    Gdiplus::Color fillTop;      // area under the price line
    Gdiplus::Color fillBottom;
    Gdiplus::Color up;
    Gdiplus::Color down;
    Gdiplus::Color volUp;
    Gdiplus::Color volDown;
    Gdiplus::Color volFlat;
    Gdiplus::Color hover;
    Gdiplus::Color tipBg;
    Gdiplus::Color tipText;
    Gdiplus::Color listBg;
    Gdiplus::Color listSel;
    Gdiplus::Color listDivider;
    Gdiplus::Color alertBg;      // list row with an active alert
    Gdiplus::Color insetBg;
    Gdiplus::Color insetBorder;
    Gdiplus::Color sma20;
    Gdiplus::Color sma50;
    Gdiplus::Color band;         // Bollinger fill
    Gdiplus::Color bandEdge;
    Gdiplus::Color rsi;
    COLORREF bgRef     = 0;      // same colours for GDI controls
    COLORREF listBgRef = 0;
    COLORREF textRef   = 0;
};

const Theme& ThemeFor(bool dark);

// Reads the Windows "apps use light theme" preference.
bool SystemPrefersDark();

// Distinct series colours for the compare view (cycles after 10).
const Gdiplus::Color& SeriesColor(size_t index, bool dark);

} // namespace st
