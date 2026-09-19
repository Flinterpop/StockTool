#include "indicators.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace st;
using Catch::Approx;

namespace {

Series MakeSeries(std::initializer_list<double> closes) {
    Series s;
    for (double c : closes) {
        s.pts[s.count].close = c;
        s.pts[s.count].time  = static_cast<int64_t>(s.count) * 86400;
        ++s.count;
    }
    return s;
}

} // namespace

TEST_CASE("SMA is NaN until the window fills, then a plain average") {
    const Series s = MakeSeries({ 1, 2, 3, 4, 5, 6 });
    Track t;
    Sma(s, 3, t);
    CHECK(std::isnan(t[0]));
    CHECK(std::isnan(t[1]));
    CHECK(t[2] == Approx(2.0));
    CHECK(t[3] == Approx(3.0));
    CHECK(t[5] == Approx(5.0));
    CHECK(std::isnan(t[6]));  // beyond count stays undefined
}

TEST_CASE("SMA with a window longer than the series is all NaN") {
    const Series s = MakeSeries({ 1, 2 });
    Track t;
    Sma(s, 5, t);
    CHECK(std::isnan(t[0]));
    CHECK(std::isnan(t[1]));
}

TEST_CASE("Bollinger bands are symmetric about the SMA") {
    const Series s = MakeSeries({ 10, 12, 11, 13, 12, 14, 13, 15 });
    Track mid;
    Track up;
    Track lo;
    Bollinger(s, 4, 2.0, mid, up, lo);
    CHECK(std::isnan(up[2]));
    for (size_t i = 3; i < s.count; ++i) {
        CHECK(up[i] - mid[i] == Approx(mid[i] - lo[i]));
        CHECK(up[i] >= mid[i]);
    }
    // Window {10,12,11,13}: mean 11.5, population sd sqrt(1.25)
    CHECK(mid[3] == Approx(11.5));
    CHECK(up[3] == Approx(11.5 + 2.0 * std::sqrt(1.25)));
}

TEST_CASE("RSI is 100 for a series that only rises and stays in range") {
    const Series rising = MakeSeries({ 1, 2, 3, 4, 5, 6, 7, 8 });
    Track t;
    Rsi(rising, 3, t);
    CHECK(std::isnan(t[2]));
    CHECK(t[3] == Approx(100.0));
    CHECK(t[7] == Approx(100.0));

    const Series mixed = MakeSeries({ 10, 11, 10, 12, 11, 13, 12, 11, 12, 14 });
    Rsi(mixed, 4, t);
    for (size_t i = 4; i < mixed.count; ++i) {
        CHECK(t[i] >= 0.0);
        CHECK(t[i] <= 100.0);
    }
    // First value: gains 1+2+... over the seed window {11-10, 10-11, 12-10, 11-12} = gains 3, losses 2
    CHECK(t[4] == Approx(100.0 - 100.0 / (1.0 + 3.0 / 2.0)));
}

TEST_CASE("RSI with too few bars is undefined") {
    const Series s = MakeSeries({ 1, 2, 3 });
    Track t;
    Rsi(s, 14, t);
    CHECK(std::isnan(t[0]));
    CHECK(std::isnan(t[2]));
}
