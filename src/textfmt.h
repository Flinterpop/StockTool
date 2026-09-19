// Number and date formatting shared by the chart and the panels.
#pragma once

#include <cstdint>
#include <string>

namespace st {

enum class DateStyle {
    Time,       // 14:35
    DayTime,    // 17 Sep 14:35
    DayMonth,   // 17 Sep
    MonthYear,  // Sep 2026
    Full,       // 17 Sep 2026
    FullTime,   // 17 Sep 2026 14:35
};

std::wstring FormatPrice(double v);                       // 185.32
std::wstring FormatMoney(double v);                       // 28,445.00 (signed if negative)
std::wstring FormatSignedMoney(double v);                 // +1,234.00
std::wstring FormatChange(double change, double pct);     // +1.23 (+0.67%)
std::wstring FormatPct(double pct);                       // +0.67%
std::wstring FormatVolume(double v);                      // 1.23M
std::wstring FormatCompact(double v);                     // 1.23T / 45.6B / 1.2M (0 -> "-")
std::wstring FormatRatio(double v);                       // 12.34 (0 -> "-")
std::wstring FormatDate(int64_t unixTime, int32_t gmtOffsetSec, DateStyle style);

} // namespace st
