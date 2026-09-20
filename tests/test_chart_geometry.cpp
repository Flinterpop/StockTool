// Inset placement / drag mapping, which the UI otherwise only exercises by hand.
#include "chart.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace st;
using Catch::Approx;
using Gdiplus::RectF;

namespace {

ChartInput Input(const Theme& theme, bool inset = true, bool compare = false) {
    ChartInput in;
    in.range      = &kRanges[kRange1Y];
    in.theme      = &theme;
    in.opts.inset = inset;
    in.compare    = compare;
    return in;
}

} // namespace

TEST_CASE("inset sits in the top-left with the default fractions") {
    const Theme& th = ThemeFor(false);
    const RectF rc(100.0f, 50.0f, 1200.0f, 600.0f);   // chart rect in client coords
    RectF box;
    REQUIRE(ChartInsetRect(rc, Input(th), 1.0f, box));
    CHECK(box.X == Approx(108.0f));   // rc.X + 8 px margin
    CHECK(box.Y == Approx(58.0f));
    CHECK(box.Width > 100.0f);
    CHECK(box.Height == Approx(70.0f));
    CHECK(box.X + box.Width < rc.X + rc.Width);
}

TEST_CASE("inset is hidden when off, in compare mode, or when the chart is tiny") {
    const Theme& th = ThemeFor(false);
    RectF box;
    CHECK_FALSE(ChartInsetRect(RectF(0, 0, 800, 500), Input(th, false), 1.0f, box));
    CHECK_FALSE(ChartInsetRect(RectF(0, 0, 800, 500), Input(th, true, true), 1.0f, box));
    CHECK_FALSE(ChartInsetRect(RectF(0, 0, 30, 30), Input(th), 1.0f, box));
    CHECK_FALSE(ChartInsetRect(RectF(0, 0, 800, 120), Input(th), 1.0f, box));   // not 3 inset heights tall
}

TEST_CASE("fractions map to the plot corners and clamp") {
    const Theme& th = ThemeFor(false);
    const RectF rc(0.0f, 0.0f, 1000.0f, 600.0f);
    ChartInput in = Input(th);
    RectF tl;
    REQUIRE(ChartInsetRect(rc, in, 1.0f, tl));
    in.insetX = 1.0f;
    in.insetY = 1.0f;
    RectF br;
    REQUIRE(ChartInsetRect(rc, in, 1.0f, br));
    CHECK(br.X > tl.X);
    CHECK(br.Y > tl.Y);
    CHECK(br.Width == Approx(tl.Width));
    // Bottom-right corner stays inside the price plot (which is above the volume pane).
    CHECK(br.X + br.Width <= 1000.0f - 64.0f);   // right axis width
    CHECK(br.Y + br.Height < 600.0f);
    in.insetX = 7.0f;   // out of range clamps to 1
    in.insetY = -3.0f;
    RectF clamped;
    REQUIRE(ChartInsetRect(rc, in, 1.0f, clamped));
    CHECK(clamped.X == Approx(br.X));
    CHECK(clamped.Y == Approx(tl.Y));
}

TEST_CASE("a dragged position round-trips through the fraction") {
    const Theme& th = ThemeFor(false);
    const RectF rc(40.0f, 20.0f, 1100.0f, 640.0f);
    ChartInput in = Input(th);
    // Ask for the box's top-left at an arbitrary pixel, then draw at that fraction.
    float fx = 0.0f;
    float fy = 0.0f;
    ChartInsetFractionFor(rc, in, 1.25f, 300.0f, 200.0f, fx, fy);
    CHECK(fx > 0.0f);
    CHECK(fx < 1.0f);
    in.insetX = fx;
    in.insetY = fy;
    RectF box;
    REQUIRE(ChartInsetRect(rc, in, 1.25f, box));
    CHECK(box.X == Approx(300.0f).margin(0.01));
    CHECK(box.Y == Approx(200.0f).margin(0.01));

    // Off the left/top clamps to the top-left corner.
    ChartInsetFractionFor(rc, in, 1.25f, -500.0f, -500.0f, fx, fy);
    CHECK(fx == 0.0f);
    CHECK(fy == 0.0f);
}

TEST_CASE("the RSI pane shrinks the price plot, moving the inset's travel range") {
    const Theme& th = ThemeFor(true);
    const RectF rc(0.0f, 0.0f, 900.0f, 700.0f);
    ChartInput in = Input(th);
    in.insetX = 1.0f;
    in.insetY = 1.0f;
    RectF without;
    REQUIRE(ChartInsetRect(rc, in, 1.0f, without));
    in.opts.rsi = true;
    RectF with;
    REQUIRE(ChartInsetRect(rc, in, 1.0f, with));
    CHECK(with.Y < without.Y);
    CHECK(with.X == Approx(without.X));
}
