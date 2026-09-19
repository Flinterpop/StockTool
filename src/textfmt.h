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
std::wstring FormatChange(double change, double pct);     // +1.23 (+0.67%)
std::wstring FormatVolume(double v);                      // 1.23M
std::wstring FormatDate(int64_t unixTime, int32_t gmtOffsetSec, DateStyle style);

} // namespace st
