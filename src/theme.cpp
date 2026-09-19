#include "theme.h"

#include <array>
#include <cassert>

namespace st {
namespace {

using Gdiplus::Color;

Theme MakeLight() {
    Theme t;
    t.dark        = false;
    t.bg          = Color(255, 250, 250, 250);
    t.text        = Color(255, 32, 33, 36);
    t.textMuted   = Color(255, 95, 99, 104);
    t.grid        = Color(255, 232, 234, 237);
    t.line        = Color(255, 26, 115, 232);
    t.fillTop     = Color(70, 26, 115, 232);
    t.fillBottom  = Color(0, 26, 115, 232);
    t.up          = Color(255, 30, 142, 62);
    t.down        = Color(255, 217, 48, 37);
    t.volUp       = Color(255, 160, 210, 175);
    t.volDown     = Color(255, 235, 170, 165);
    t.volFlat     = Color(255, 196, 206, 220);
    t.hover       = Color(255, 60, 64, 67);
    t.tipBg       = Color(235, 32, 33, 36);
    t.tipText     = Color(255, 255, 255, 255);
    t.listBg      = Color(255, 255, 255, 255);
    t.listSel     = Color(255, 226, 236, 252);
    t.listDivider = Color(255, 238, 238, 238);
    t.alertBg     = Color(255, 255, 243, 205);
    t.insetBg     = Color(230, 255, 255, 255);
    t.insetBorder = Color(255, 210, 214, 220);
    t.sma20       = Color(255, 245, 124, 0);
    t.sma50       = Color(255, 156, 39, 176);
    t.band        = Color(40, 26, 115, 232);
    t.bandEdge    = Color(120, 26, 115, 232);
    t.rsi         = Color(255, 0, 137, 123);
    t.bgRef       = RGB(250, 250, 250);
    t.listBgRef   = RGB(255, 255, 255);
    t.textRef     = RGB(32, 33, 36);
    return t;
}

Theme MakeDark() {
    Theme t;
    t.dark        = true;
    t.bg          = Color(255, 32, 33, 36);
    t.text        = Color(255, 232, 234, 237);
    t.textMuted   = Color(255, 154, 160, 166);
    t.grid        = Color(255, 52, 54, 58);
    t.line        = Color(255, 138, 180, 248);
    t.fillTop     = Color(80, 138, 180, 248);
    t.fillBottom  = Color(0, 138, 180, 248);
    t.up          = Color(255, 129, 201, 149);
    t.down        = Color(255, 242, 139, 130);
    t.volUp       = Color(255, 60, 110, 80);
    t.volDown     = Color(255, 120, 65, 62);
    t.volFlat     = Color(255, 70, 76, 86);
    t.hover       = Color(255, 200, 204, 210);
    t.tipBg       = Color(240, 60, 64, 67);
    t.tipText     = Color(255, 255, 255, 255);
    t.listBg      = Color(255, 41, 42, 45);
    t.listSel     = Color(255, 54, 68, 92);
    t.listDivider = Color(255, 56, 58, 62);
    t.alertBg     = Color(255, 84, 70, 30);
    t.insetBg     = Color(220, 41, 42, 45);
    t.insetBorder = Color(255, 80, 84, 90);
    t.sma20       = Color(255, 255, 171, 64);
    t.sma50       = Color(255, 206, 147, 216);
    t.band        = Color(45, 138, 180, 248);
    t.bandEdge    = Color(130, 138, 180, 248);
    t.rsi         = Color(255, 77, 208, 225);
    t.bgRef       = RGB(32, 33, 36);
    t.listBgRef   = RGB(41, 42, 45);
    t.textRef     = RGB(232, 234, 237);
    return t;
}

const std::array<Color, 10> kSeriesLight = {{
    Color(255, 26, 115, 232), Color(255, 217, 48, 37),  Color(255, 30, 142, 62),
    Color(255, 245, 124, 0),  Color(255, 156, 39, 176), Color(255, 0, 137, 123),
    Color(255, 121, 85, 72),  Color(255, 233, 30, 99),  Color(255, 63, 81, 181),
    Color(255, 96, 125, 139),
}};

const std::array<Color, 10> kSeriesDark = {{
    Color(255, 138, 180, 248), Color(255, 242, 139, 130), Color(255, 129, 201, 149),
    Color(255, 255, 171, 64),  Color(255, 206, 147, 216), Color(255, 77, 208, 225),
    Color(255, 188, 170, 164), Color(255, 244, 143, 177), Color(255, 159, 168, 218),
    Color(255, 176, 190, 197),
}};

} // namespace

const Theme& ThemeFor(bool dark) {
    static const Theme light = MakeLight();
    static const Theme darkTheme = MakeDark();
    return dark ? darkTheme : light;
}

bool SystemPrefersDark() {
    DWORD value = 1;
    DWORD size  = sizeof(value);
    const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER,
                                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                    L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return rc == ERROR_SUCCESS && value == 0;
}

const Gdiplus::Color& SeriesColor(size_t index, bool dark) {
    const auto& pal = dark ? kSeriesDark : kSeriesLight;
    assert(!pal.empty());
    return pal[index % pal.size()];
}

} // namespace st
