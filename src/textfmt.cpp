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

// Inserts thousands separators into the integer part of a "%.2f" string.
std::wstring Group(const std::wstring& plain) {
    const size_t dot = plain.find(L'.');
    const size_t intEnd = (dot == std::wstring::npos) ? plain.size() : dot;
    const size_t start = (!plain.empty() && plain[0] == L'-') ? 1 : 0;
    std::wstring out;
    out.reserve(plain.size() + 8);
    if (start == 1) { out += L'-'; }
    const size_t digits = intEnd - start;
    for (size_t i = 0; i < digits && i < 64; ++i) {
        if (i > 0 && (digits - i) % 3 == 0) { out += L','; }
        out += plain[start + i];
    }
    out += plain.substr(intEnd);
    return out;
}

} // namespace

std::wstring FormatPrice(double v) {
    if (!std::isfinite(v)) { return L"-"; }
    std::array<wchar_t, 64> b{};
    swprintf_s(b.data(), b.size(), L"%.2f", v);
    return b.data();
}

std::wstring FormatMoney(double v) {
    if (!std::isfinite(v)) { return L"-"; }
    return Group(FormatPrice(v));
}

std::wstring FormatSignedMoney(double v) {
    if (!std::isfinite(v)) { return L"-"; }
    return (v >= 0.0 ? L"+" : L"") + FormatMoney(v);
}

std::wstring FormatChange(double change, double pct) {
    if (!std::isfinite(change) || !std::isfinite(pct)) { return L"-"; }
    std::array<wchar_t, 64> b{};
    swprintf_s(b.data(), b.size(), L"%+.2f (%+.2f%%)", change, pct);
    return b.data();
}

std::wstring FormatPct(double pct) {
    if (!std::isfinite(pct)) { return L"-"; }
    std::array<wchar_t, 32> b{};
    swprintf_s(b.data(), b.size(), L"%+.2f%%", pct);
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

std::wstring FormatCompact(double v) {
    if (!std::isfinite(v) || v <= 0.0) { return L"-"; }
    std::array<wchar_t, 64> b{};
    if (v >= 1e12)     { swprintf_s(b.data(), b.size(), L"%.2fT", v / 1e12); }
    else if (v >= 1e9) { swprintf_s(b.data(), b.size(), L"%.2fB", v / 1e9); }
    else if (v >= 1e6) { swprintf_s(b.data(), b.size(), L"%.2fM", v / 1e6); }
    else if (v >= 1e3) { swprintf_s(b.data(), b.size(), L"%.1fK", v / 1e3); }
    else               { swprintf_s(b.data(), b.size(), L"%.2f", v); }
    return b.data();
}

std::wstring FormatRatio(double v) {
    if (!std::isfinite(v) || v == 0.0) { return L"-"; }
    return FormatPrice(v);
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
