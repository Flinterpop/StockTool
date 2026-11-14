// Weekly 3D: slicing bars into weeks, and the projection the hover relies on.
#include "weekly.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace st;
using Catch::Approx;

namespace {

constexpr int64_t kMonday = 1767571200;   // 2026-01-05 00:00 UTC, a Monday
constexpr int32_t kEst    = -5 * 3600;

// Local exchange time -> Unix seconds.
int64_t At(int64_t day, int hour, int minute) { return kMonday + day * 86400 + hour * 3600 + minute * 60 - kEst; }

void Add(QuoteData& q, int64_t t, double close) {
    REQUIRE(q.series.count < kMaxPoints);
    Candle& c = q.series.pts[q.series.count++];
    c.time  = t;
    c.open  = close;
    c.high  = close;
    c.low   = close;
    c.close = close;
}

// `weeks` weeks of 60m bars 09:30..15:30 (7 a day), close = 100 + week + day/10 + bar/100.
std::unique_ptr<QuoteData> Hourly(int weeks) {
    auto q = std::make_unique<QuoteData>();
    q->valid = true;
    q->meta.gmtOffsetSec = kEst;
    for (int w = 0; w < weeks; ++w) {
        for (int d = 0; d < 5; ++d) {
            for (int b = 0; b < 7; ++b) {
                Add(*q, At(w * 7 + d, 9 + b, 30), 100.0 + w + d / 10.0 + b / 100.0);
            }
        }
    }
    return q;
}

const RangeSpec& Hourly1Y() { return kWeeklyRanges[3]; }
const RangeSpec& Daily1Y()  { return kWeeklyRanges[4]; }

} // namespace

TEST_CASE("interval strings parse to seconds") {
    CHECK(IntervalSeconds(L"30m") == 1800);
    CHECK(IntervalSeconds(L"60m") == 3600);
    CHECK(IntervalSeconds(L"1h") == 3600);
    CHECK(IntervalSeconds(L"1d") == 86400);
    CHECK(IntervalSeconds(L"1wk") == 7 * 86400);
    CHECK(IntervalSeconds(L"") == 0);
    CHECK(IntervalSeconds(L"m") == 0);
    CHECK(IntervalSeconds(L"5x") == 0);
    CHECK(IntervalSeconds(nullptr) == 0);
    for (const RangeSpec& r : kWeeklyRanges) { CHECK(IntervalSeconds(r.interval) > 0); }
}

TEST_CASE("hourly bars land on a week x (day, bar) grid") {
    auto q = Hourly(3);
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    CHECK(g->weeks == 3);
    CHECK(g->days == 5);
    CHECK(g->slotsPerDay == 7);
    CHECK(g->Slots() == 35);
    CHECK(g->weekStart[0] == kMonday - kEst);           // local Monday midnight
    CHECK(g->weekStart[2] - g->weekStart[0] == 14 * 86400);
    // Week 1, Wednesday (day 2), bar 4 (13:30).
    const size_t cell = 1 * 35 + 2 * 7 + 4;
    CHECK(g->price[cell] == Approx(101.24));
    CHECK(g->time[cell] == At(7 + 2, 13, 30));
    CHECK(g->lo == Approx(100.0));
    CHECK(g->hi == Approx(102.46));
}

TEST_CASE("a holiday holds the last trade; the unfinished week stays empty") {
    auto q = std::make_unique<QuoteData>();
    q->valid = true;
    q->meta.gmtOffsetSec = kEst;
    auto full = Hourly(2);
    // Keep everything except week 1's Monday; stop after week 1's Wednesday.
    for (size_t i = 0; i < full->series.count; ++i) {
        const Candle& c = full->series.pts[i];
        const bool holiday = c.time >= At(7, 0, 0) && c.time < At(8, 0, 0);
        const bool future  = c.time >= At(10, 0, 0);
        if (!holiday && !future) { Add(*q, c.time, c.close); }
    }
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    REQUIRE(g->weeks == 2);
    const double lastFriday = full->series.pts[5 * 7 - 1].close;
    for (size_t b = 0; b < 7; ++b) {
        CHECK(g->price[35 + b] == Approx(lastFriday));   // Monday held at Friday's close
        CHECK(g->time[35 + b] == 0);                      // and marked as held
    }
    CHECK(g->price[35 + 2 * 7 + 6] == Approx(101.26));   // Wednesday 15:30 is real
    CHECK(std::isnan(g->price[35 + 3 * 7]));             // Thursday has not happened
    CHECK(std::isnan(g->value[35 + 4 * 7 + 6]));
}

TEST_CASE("a DST change does not shift the bars by a slot") {
    auto q = Hourly(2);
    // Week 0 was in daylight time: the same local session is an hour earlier
    // in UTC, which today's offset would read as 08:30.
    for (size_t i = 0; i < 35; ++i) { q->series.pts[i].time -= 3600; }
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    CHECK(g->slotsPerDay == 7);
    CHECK(g->price[0] == Approx(100.0));
    CHECK(g->price[6] == Approx(100.06));
}

TEST_CASE("percent mode measures each week against the previous week's last close") {
    auto q = Hourly(2);
    q->meta.chartPrevClose = 99.0;
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::PctChange, *g, err));
    CHECK(g->base[0] == Approx(99.0));
    CHECK(g->base[1] == Approx(100.46));                 // week 0, Friday 15:30
    CHECK(g->value[0] == Approx((100.0 / 99.0 - 1.0) * 100.0));
    CHECK(g->value[35] == Approx((101.0 / 100.46 - 1.0) * 100.0));
    CHECK(g->lo < g->hi);
}

TEST_CASE("daily bars give five points a week and the newest weeks are kept") {
    auto q = std::make_unique<QuoteData>();
    q->valid = true;
    q->meta.gmtOffsetSec = kEst;
    q->meta.chartPrevClose = 50.0;
    const int weeks = static_cast<int>(kMaxWeeks) + 10;
    for (int w = 0; w < weeks; ++w) {
        for (int d = 0; d < 5; ++d) { Add(*q, At(w * 7 + d, 9, 30), 100.0 + w); }
    }
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Daily1Y(), WeekValue::PctChange, *g, err));
    CHECK(g->weeks == kMaxWeeks);
    CHECK(g->slotsPerDay == 1);
    CHECK(g->price[0] == Approx(110.0));                 // the 10 oldest weeks were dropped
    CHECK(g->base[0] == Approx(110.0));                  // so chartPrevClose no longer applies
    CHECK(g->price[(kMaxWeeks - 1) * 5 + 4] == Approx(100.0 + weeks - 1));
}

TEST_CASE("weekend trading widens the week to seven days") {
    auto q = std::make_unique<QuoteData>();
    q->valid = true;
    for (int d = 0; d < 14; ++d) { Add(*q, kMonday + d * 86400, 10.0 + d); }
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Daily1Y(), WeekValue::Price, *g, err));
    CHECK(g->days == 7);
    CHECK(g->weeks == 2);
    CHECK(g->price[6] == Approx(16.0));                  // week 0 Sunday
    CHECK(g->price[7] == Approx(17.0));                  // week 1 Monday
}

TEST_CASE("nothing to plot is an error, not an empty grid") {
    auto q = std::make_unique<QuoteData>();
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    q->error = L"HTTP 404";
    CHECK_FALSE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    CHECK(err == L"HTTP 404");
    q->valid = true;
    Add(*q, kMonday, 0.0);
    Add(*q, kMonday + 3600, 0.0);
    CHECK_FALSE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    CHECK(g->weeks == 0);
}

TEST_CASE("the camera clamps elevation and zoom and wraps azimuth") {
    WeekView v;
    RotateWeekView(v, 0.0f, 10000.0f);
    CHECK(v.elevationDeg == Approx(kMaxElevation));
    RotateWeekView(v, 0.0f, -10000.0f);
    CHECK(v.elevationDeg == Approx(kMinElevation));
    RotateWeekView(v, 1000.0f, 0.0f);                    // +400 degrees
    CHECK(v.azimuthDeg >= -180.0f);
    CHECK(v.azimuthDeg <= 180.0f);
    ZoomWeekView(v, 100.0f);
    CHECK(v.zoom == Approx(kMaxZoom));
    ZoomWeekView(v, -100.0f);
    CHECK(v.zoom == Approx(kMinZoom));
}

TEST_CASE("hovering a projected cell finds that cell, from any angle") {
    auto q = Hourly(4);
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    const Theme& th = ThemeFor(false);
    const Gdiplus::RectF rc(0.0f, 0.0f, 1000.0f, 700.0f);
    const float azimuths[] = { -32.0f, 40.0f, 150.0f };
    for (const float az : azimuths) {
        WeeklyInput in;
        in.grid  = g.get();
        in.theme = &th;
        in.view.azimuthDeg = az;
        Gdiplus::PointF p;
        REQUIRE(WeeklyProject(rc, in, 1.0f, 2, 17, p));
        CHECK(p.X > rc.X);
        CHECK(p.X < rc.X + rc.Width);
        CHECK(p.Y > rc.Y);
        CHECK(p.Y < rc.Y + rc.Height);
        size_t w = 99, s = 99;
        REQUIRE(WeeklyHit(rc, in, 1.0f, p.X, p.Y, 3.0f, w, s));
        CHECK(w == 2);
        CHECK(s == 17);
    }
    WeeklyInput none;
    size_t w = 0, s = 0;
    CHECK_FALSE(WeeklyHit(rc, none, 1.0f, 10.0f, 10.0f, 16.0f, w, s));
}

TEST_CASE("both styles draw without tripping an assertion") {
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput si;
    REQUIRE(Gdiplus::GdiplusStartup(&token, &si, nullptr) == Gdiplus::Ok);
    {
        auto q = Hourly(6);
        auto g = std::make_unique<WeekGrid>();
        auto scratch = std::make_unique<WeeklyScratch>();
        std::wstring err;
        REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::PctChange, *g, err));
        Gdiplus::Bitmap bmp(800, 500, PixelFormat32bppARGB);
        Gdiplus::Graphics gr(&bmp);
        const Gdiplus::RectF rc(0.0f, 0.0f, 800.0f, 500.0f);
        for (const bool dark : { false, true }) {
            WeeklyInput in;
            in.grid   = g.get();
            in.mode   = WeekValue::PctChange;
            in.theme  = &ThemeFor(dark);
            in.hoverX = 400;
            in.hoverY = 250;
            for (const WeekStyle st : { WeekStyle::Waterfall, WeekStyle::Surface, WeekStyle::Week2D }) {
                in.style = st;
                DrawWeeklyBase(gr, rc, in, 1.0f, *scratch);
                DrawWeeklyOverlay(gr, rc, in, 1.0f);
            }
            in.grid    = nullptr;
            in.message = L"Loading";
            DrawWeeklyBase(gr, rc, in, 1.0f, *scratch);
        }
        // The bitmap is still readable after every pass.
        Gdiplus::Color c;
        REQUIRE(bmp.GetPixel(400, 250, &c) == Gdiplus::Ok);
    }
    Gdiplus::GdiplusShutdown(token);
}

TEST_CASE("weekday stats find the day that moves and the day that trades") {
    // Daily bars: Fridays jump 2 % on triple volume, other days drift 0.5 %.
    auto q = std::make_unique<QuoteData>();
    q->valid = true;
    q->meta.gmtOffsetSec = kEst;
    double px = 100.0;
    for (int w = 0; w < 10; ++w) {
        for (int d = 0; d < 5; ++d) {
            px *= (d == 4) ? 1.02 : 1.005;
            Add(*q, At(w * 7 + d, 9, 30), px);
            q->series.pts[q->series.count - 1].volume = (d == 4) ? 3000.0 : 1000.0;
        }
    }
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Daily1Y(), WeekValue::Price, *g, err));
    const DayStat& mon = g->dayStats[0];
    const DayStat& fri = g->dayStats[4];
    CHECK(mon.n == 9);                                   // the very first Monday has no previous close
    CHECK(fri.n == 10);
    CHECK(fri.absMovePct == Approx(2.0));
    CHECK(fri.movePct == Approx(2.0));
    CHECK(mon.absMovePct == Approx(0.5));
    CHECK(fri.volRatio == Approx(3000.0 / 1400.0));      // mean day = (4 x 1000 + 3000) / 5
    CHECK(mon.volRatio == Approx(1000.0 / 1400.0));
}

TEST_CASE("volume mode plots shares from zero and held cells as nothing traded") {
    auto q = Hourly(2);
    for (size_t i = 0; i < q->series.count; ++i) { q->series.pts[i].volume = 100.0 + static_cast<double>(i); }
    // Drop week 1's Monday: those cells are held, so they traded nothing.
    size_t n = 0;
    for (size_t i = 0; i < q->series.count; ++i) {
        const Candle c = q->series.pts[i];
        if (c.time >= At(7, 0, 0) && c.time < At(8, 0, 0)) { continue; }
        q->series.pts[n++] = c;
    }
    q->series.count = n;
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Volume, *g, err));
    CHECK(g->lo == Approx(0.0));
    CHECK(g->value[0] == Approx(100.0));
    CHECK(g->value[35] == Approx(0.0));
    CHECK(g->hi == Approx(100.0 + static_cast<double>(n - 1) + 7.0));
}

TEST_CASE("weeks ago counts back from the newest week and stops at the oldest") {
    auto q = Hourly(4);
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    CHECK(WeekRowFor(*g, 0) == 3);
    CHECK(WeekRowFor(*g, 2) == 1);
    CHECK(WeekRowFor(*g, 99) == 0);
    CHECK(WeeksAgoLabel(0) == L"This week");
    CHECK(WeeksAgoLabel(1) == L"Last week");
    CHECK(WeeksAgoLabel(5) == L"5 weeks ago");
}

TEST_CASE("in Week 2D the hover picks the shown week's slot under the mouse") {
    auto q = Hourly(4);
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Price, *g, err));
    const Theme& th = ThemeFor(true);
    const Gdiplus::RectF rc(0.0f, 0.0f, 1000.0f, 700.0f);
    WeeklyInput in;
    in.grid     = g.get();
    in.theme    = &th;
    in.style    = WeekStyle::Week2D;
    in.weeksAgo = 2;
    Gdiplus::PointF p;
    REQUIRE(WeeklyProject(rc, in, 1.0f, 1, 20, p));
    size_t w = 99, s = 99;
    REQUIRE(WeeklyHit(rc, in, 1.0f, p.X, p.Y + 30.0f, 1.0f, w, s));   // any height over the plot
    CHECK(w == 1);
    CHECK(s == 20);
    CHECK_FALSE(WeeklyHit(rc, in, 1.0f, 2.0f, 2.0f, 16.0f, w, s));    // outside the plot
}

TEST_CASE("Week 2D's right-hand price axis reads the plotted line's price in every mode") {
    auto q = Hourly(3);
    q->meta.chartPrevClose = 99.0;
    const Gdiplus::RectF rc(0.0f, 0.0f, 1000.0f, 700.0f);
    const Theme& th = ThemeFor(false);
    for (const WeekValue mode : { WeekValue::Price, WeekValue::PctChange }) {
        auto g = std::make_unique<WeekGrid>();
        std::wstring err;
        REQUIRE(BuildWeekGrid(*q, Hourly1Y(), mode, *g, err));
        WeeklyInput in;
        in.grid     = g.get();
        in.theme    = &th;
        in.style    = WeekStyle::Week2D;
        in.mode     = mode;
        in.weeksAgo = 1;
        // Cell (week 1, slot 9): its price on the right axis sits where the line is drawn.
        Gdiplus::PointF p;
        REQUIRE(WeeklyProject(rc, in, 1.0f, 1, 9, p));
        float y = 0.0f;
        REQUIRE(WeeklyPriceY(rc, in, 1.0f, g->price[35 + 9], y));
        CHECK(y == Approx(p.Y).margin(0.01));
    }
    // Volume mode: the price scale is its own, spanning the week's prices inside the plot.
    auto g = std::make_unique<WeekGrid>();
    std::wstring err;
    REQUIRE(BuildWeekGrid(*q, Hourly1Y(), WeekValue::Volume, *g, err));
    WeeklyInput in;
    in.grid  = g.get();
    in.theme = &th;
    in.style = WeekStyle::Week2D;
    in.mode  = WeekValue::Volume;
    float top = 0.0f, bottom = 0.0f;
    REQUIRE(WeeklyPriceY(rc, in, 1.0f, 102.46, top));      // the newest week's high
    REQUIRE(WeeklyPriceY(rc, in, 1.0f, 102.0, bottom));    // and its low
    CHECK(top < bottom);
    CHECK(top > 84.0f);
    CHECK(bottom < 700.0f - 172.0f + 84.0f);
    in.style = WeekStyle::Waterfall;
    CHECK_FALSE(WeeklyPriceY(rc, in, 1.0f, 100.0, top));
}
