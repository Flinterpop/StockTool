#include "textfmt.h"

#include <array>
#include <cassert>
#include <cmath>
#include <ctime>
#include <cwchar>

namespace st {
namespace {

constexpr std::array<const wchar_t*, 12> kMonths = {
    L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec",
};

} // namespace

std::wstring FormatPrice(double v) {
    if (!std::isfinite(v)) { return L"-"; }
    std::array<wchar_t, 64> b{};
    swprintf_s(b.data(), b.size(), L"%.2f", v);
    return b.data();
}

std::wstring FormatChange(double change, double pct) {
    if (!std::isfinite(change) || !std::isfinite(pct)) { return L"-"; }
    std::array<wchar_t, 64> b{};
    swprintf_s(b.data(), b.size(), L"%+.2f (%+.2f%%)", change, pct);
    return b.data();
}

std::wstring FormatVolume(double v) {
    if (!std::isfinite(v) || v < 0.0) { return L"-"; }
    std::array<wchar_t, 64> b{};
    if (v >= 1e9)      { swprintf_s(b.data(), b.size(), L"%.2fB", v / 1e9); }
    else if (v >= 1e6) { swprintf_s(b.data(), b.size(), L"%.2fM", v / 1e6); }
    else if (v >= 1e3) { swprintf_s(b.data(), b.size(), L"%.1fK", v / 1e3); }
    else               { swprintf_s(b.data(), b.size(), L"%.0f", v); }
    return b.data();
}

std::wstring FormatDate(int64_t unixTime, int32_t gmtOffsetSec, DateStyle style) {
    const __time64_t local = static_cast<__time64_t>(unixTime + gmtOffsetSec);
    tm p{};
    if (_gmtime64_s(&p, &local) != 0) { return L"?"; }
    assert(p.tm_mon >= 0 && p.tm_mon < 12);
    const wchar_t* mon  = kMonths[static_cast<size_t>(p.tm_mon)];
    const int      year = p.tm_year + 1900;
    std::array<wchar_t, 64> b{};
    switch (style) {
    case DateStyle::Time:
        swprintf_s(b.data(), b.size(), L"%02d:%02d", p.tm_hour, p.tm_min);
        break;
    case DateStyle::DayTime:
        swprintf_s(b.data(), b.size(), L"%d %s %02d:%02d", p.tm_mday, mon, p.tm_hour, p.tm_min);
        break;
    case DateStyle::DayMonth:
        swprintf_s(b.data(), b.size(), L"%d %s", p.tm_mday, mon);
        break;
    case DateStyle::MonthYear:
        swprintf_s(b.data(), b.size(), L"%s %d", mon, year);
        break;
    case DateStyle::Full:
        swprintf_s(b.data(), b.size(), L"%d %s %d", p.tm_mday, mon, year);
        break;
    case DateStyle::FullTime:
        swprintf_s(b.data(), b.size(), L"%d %s %d %02d:%02d", p.tm_mday, mon, year,
                   p.tm_hour, p.tm_min);
        break;
    }
    return b.data();
}

} // namespace st
