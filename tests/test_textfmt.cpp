#include "textfmt.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace st;

TEST_CASE("FormatMoney groups thousands and keeps the sign") {
    CHECK(FormatMoney(0.0) == L"0.00");
    CHECK(FormatMoney(999.5) == L"999.50");
    CHECK(FormatMoney(1000.0) == L"1,000.00");
    CHECK(FormatMoney(28445.0) == L"28,445.00");
    CHECK(FormatMoney(1234567.891) == L"1,234,567.89");
    CHECK(FormatMoney(-1234.5) == L"-1,234.50");
    CHECK(FormatSignedMoney(1234.5) == L"+1,234.50");
    CHECK(FormatSignedMoney(-2.0) == L"-2.00");
    CHECK(FormatMoney(std::numeric_limits<double>::quiet_NaN()) == L"-");
}

TEST_CASE("FormatCompact and FormatVolume scale by magnitude") {
    CHECK(FormatCompact(0.0) == L"-");
    CHECK(FormatCompact(950.0) == L"950.00");
    CHECK(FormatCompact(48e9) == L"48.00B");
    CHECK(FormatCompact(1.5e12) == L"1.50T");
    CHECK(FormatVolume(7900000.0) == L"7.90M");
    CHECK(FormatVolume(12500.0) == L"12.5K");
    CHECK(FormatVolume(42.0) == L"42");
}

TEST_CASE("FormatChange / FormatPct / FormatRatio") {
    CHECK(FormatChange(1.23, 0.67) == L"+1.23 (+0.67%)");
    CHECK(FormatChange(-0.48, -0.17) == L"-0.48 (-0.17%)");
    CHECK(FormatPct(4.5) == L"+4.50%");
    CHECK(FormatRatio(0.0) == L"-");
    CHECK(FormatRatio(15.2) == L"15.20");
}

TEST_CASE("FormatDate applies the exchange offset") {
    // 2026-09-18 20:00:00 UTC; Toronto is UTC-4 in September -> 16:00 local.
    const int64_t t = 1789761600;
    CHECK(FormatDate(t, -14400, DateStyle::FullTime) == L"18 Sep 2026 16:00");
    CHECK(FormatDate(t, -14400, DateStyle::Time) == L"16:00");
    CHECK(FormatDate(t, -14400, DateStyle::DayMonth) == L"18 Sep");
    CHECK(FormatDate(t, -14400, DateStyle::MonthYear) == L"Sep 2026");
    CHECK(FormatDate(t, 0, DateStyle::Full) == L"18 Sep 2026");
    CHECK(FormatDate(t, 0, DateStyle::DayTime) == L"18 Sep 20:00");
}
