// Per-week slicing and the waterfall / surface renderer. See weekly.h.
#include "weekly.h"

#include "chart.h"
#include "textfmt.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cwchar>
#include <limits>

using namespace Gdiplus;

namespace st {

using std::max;
using std::min;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr float  kPi  = 3.14159265f;

// Box proportions in world units: x = time of week, z = weeks (newest at
// +z, nearest the default camera), y = value.
constexpr float kBoxW = 1.6f;
constexpr float kBoxD = 1.0f;
constexpr float kBoxH = 0.55f;

int64_t FloorDiv(int64_t a, int64_t b) {
    assert(b > 0);
    const int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// Local calendar day number (days since 1970-01-01 in exchange time).
int64_t LocalDay(int64_t t, int32_t gmt) { return FloorDiv(t + gmt, 86400); }

// 1970-01-01 was a Thursday; +3 makes Monday the first day of week 0.
int64_t WeekOf(int64_t day)    { return FloorDiv(day + 3, 7); }
size_t  WeekdayOf(int64_t day) { return static_cast<size_t>((day + 3) - WeekOf(day) * 7); }

// Pass 1: which weeks, how many slots a day, weekend trading?
struct GridShape {
    int64_t firstWeek = 0;
    int64_t lastWeek  = 0;
    size_t  slotsPerDay = 1;
    bool    weekend   = false;
    bool    any       = false;
};

GridShape MeasureShape(const Series& s, int32_t gmt, int64_t iv, bool intraday) {
    assert(s.count <= kMaxPoints && iv > 0);
    GridShape sh;
    int64_t dayStart = 0;
    int64_t curDay   = INT64_MIN;
    for (size_t i = 0; i < s.count; ++i) {
        const Candle& c = s.pts[i];
        if (!(c.close > 0.0)) { continue; }
        const int64_t day = LocalDay(c.time, gmt);
        if (day != curDay) { curDay = day; dayStart = c.time; }
        const int64_t wk = WeekOf(day);
        if (!sh.any) { sh.firstWeek = wk; sh.lastWeek = wk; sh.any = true; }
        sh.firstWeek = min(sh.firstWeek, wk);
        sh.lastWeek  = max(sh.lastWeek, wk);
        if (WeekdayOf(day) >= 5) { sh.weekend = true; }
        if (intraday) {
            const size_t slot = static_cast<size_t>((c.time - dayStart) / iv) + 1;
            sh.slotsPerDay = max(sh.slotsPerDay, slot);
        }
    }
    const size_t days = sh.weekend ? 7 : 5;
    sh.slotsPerDay = min(sh.slotsPerDay, kMaxWeekSlots / days);
    if (sh.any && sh.lastWeek - sh.firstWeek + 1 > static_cast<int64_t>(kMaxWeeks)) {
        sh.firstWeek = sh.lastWeek - static_cast<int64_t>(kMaxWeeks) + 1;   // keep the newest
    }
    assert(sh.slotsPerDay >= 1 && sh.slotsPerDay * days <= kMaxWeekSlots);
    return sh;
}

// Pass 2: drop each bar into its cell (a later bar in the same cell wins).
void PlaceBars(const Series& s, int32_t gmt, int64_t iv, bool intraday, const GridShape& sh, WeekGrid& g) {
    assert(g.weeks > 0 && g.Slots() <= kMaxWeekSlots);
    int64_t dayStart = 0;
    int64_t curDay   = INT64_MIN;
    for (size_t i = 0; i < s.count; ++i) {
        const Candle& c = s.pts[i];
        if (!(c.close > 0.0)) { continue; }
        const int64_t day = LocalDay(c.time, gmt);
        if (day != curDay) { curDay = day; dayStart = c.time; }
        const int64_t wk = WeekOf(day);
        const size_t  wd = WeekdayOf(day);
        if (wk < sh.firstWeek || wd >= g.days) { continue; }
        const size_t bar = intraday ? static_cast<size_t>((c.time - dayStart) / iv) : 0;
        if (bar >= g.slotsPerDay) { continue; }
        const size_t cell = static_cast<size_t>(wk - sh.firstWeek) * g.Slots() + wd * g.slotsPerDay + bar;
        assert(cell < g.weeks * g.Slots());
        g.price[cell]  = c.close;
        g.time[cell]   = c.time;
        g.volume[cell] = c.volume > 0.0 ? c.volume : 0.0;
    }
}

// Holds the last trade across interior gaps; leading/trailing cells stay NaN.
void FillGaps(WeekGrid& g) {
    const size_t n = g.weeks * g.Slots();
    assert(n <= kMaxWeekCells);
    size_t last = n;
    for (size_t i = 0; i < n; ++i) {
        if (!std::isnan(g.price[i])) { last = i; }
    }
    double held = kNaN;
    for (size_t i = 0; i < n && last < n && i <= last; ++i) {
        if (std::isnan(g.price[i])) {
            g.price[i] = held;
        } else {
            held = g.price[i];
        }
    }
}

// Reference close per week and the plotted values, with their range.
void ComputeValues(const QuoteData& q, bool truncated, WeekValue mode, WeekGrid& g) {
    const size_t S = g.Slots();
    assert(g.weeks > 0 && S > 0);
    double prevLast = (!truncated && q.meta.chartPrevClose > 0.0) ? q.meta.chartPrevClose : kNaN;
    g.lo = std::numeric_limits<double>::infinity();
    g.hi = -std::numeric_limits<double>::infinity();
    for (size_t w = 0; w < g.weeks; ++w) {
        double first = kNaN;
        double lastV = kNaN;
        for (size_t s = 0; s < S; ++s) {
            const double p = g.price[w * S + s];
            if (std::isnan(p)) { continue; }
            if (std::isnan(first)) { first = p; }
            lastV = p;
        }
        g.base[w] = std::isnan(prevLast) ? first : prevLast;
        if (!std::isnan(lastV)) { prevLast = lastV; }
        for (size_t s = 0; s < S; ++s) {
            const double p = g.price[w * S + s];
            double v = p;
            if (mode == WeekValue::PctChange) {
                v = (!std::isnan(p) && g.base[w] > 0.0) ? (p / g.base[w] - 1.0) * 100.0 : kNaN;
            } else if (mode == WeekValue::Volume) {
                v = std::isnan(p) ? kNaN : g.volume[w * S + s];   // a held cell traded nothing
            }
            g.value[w * S + s] = v;
            if (!std::isnan(v)) { g.lo = min(g.lo, v); g.hi = max(g.hi, v); }
        }
    }
}

// Per-weekday move and volume over every week. A day counts when it has a
// real bar; its move is its last close against the close before it (the
// previous trading day, which may sit in the week before).
void ComputeDayStats(WeekGrid& g) {
    const size_t S = g.Slots(), spd = g.slotsPerDay;
    assert(g.days <= 7 && S > 0);
    g.dayStats.fill(DayStat{});
    double prevClose = kNaN;
    for (size_t w = 0; w < g.weeks; ++w) {
        for (size_t d = 0; d < g.days; ++d) {
            const size_t first = w * S + d * spd;
            double close = kNaN, vol = 0.0;
            bool   traded = false;
            for (size_t b = 0; b < spd; ++b) {
                if (g.time[first + b] != 0) { traded = true; vol += g.volume[first + b]; close = g.price[first + b]; }
            }
            if (traded && !std::isnan(prevClose) && prevClose > 0.0) {
                DayStat& st = g.dayStats[d];
                const double move = (close / prevClose - 1.0) * 100.0;
                st.absMovePct += std::fabs(move);
                st.movePct    += move;
                st.volume     += vol;
                ++st.n;
            }
            if (traded) { prevClose = close; }
        }
    }
    double volSum = 0.0;
    size_t volN   = 0;
    for (size_t d = 0; d < g.days; ++d) {
        DayStat& st = g.dayStats[d];
        if (st.n == 0) { continue; }
        st.absMovePct /= static_cast<double>(st.n);
        st.movePct    /= static_cast<double>(st.n);
        st.volume     /= static_cast<double>(st.n);
        volSum += st.volume;
        ++volN;
    }
    const double volMean = volN > 0 ? volSum / static_cast<double>(volN) : 0.0;
    for (size_t d = 0; d < g.days; ++d) {
        g.dayStats[d].volRatio = volMean > 0.0 ? g.dayStats[d].volume / volMean : 0.0;
    }
}

} // namespace

int64_t IntervalSeconds(const wchar_t* interval) {
    if (interval == nullptr) { return 0; }
    wchar_t* end = nullptr;
    const long n = std::wcstol(interval, &end, 10);
    if (end == interval || n <= 0 || n > 1000) { return 0; }
    const std::wstring unit = end;
    if (unit == L"m")  { return n * 60; }
    if (unit == L"h")  { return n * 3600; }
    if (unit == L"d")  { return n * 86400; }
    if (unit == L"wk") { return n * 7 * 86400; }
    return 0;
}

bool BuildWeekGrid(const QuoteData& q, const RangeSpec& spec, WeekValue mode, WeekGrid& g, std::wstring& err) {
    assert(q.series.count <= kMaxPoints);
    const int64_t iv = IntervalSeconds(spec.interval);
    g.weeks = 0;
    if (iv <= 0 || iv > 86400) { err = L"unsupported bar interval"; return false; }
    if (!q.valid || q.series.count < 2) { err = q.error.empty() ? L"no bars" : q.error; return false; }
    const int32_t   gmt = q.meta.gmtOffsetSec;
    const GridShape sh  = MeasureShape(q.series, gmt, iv, spec.intraday);
    if (!sh.any) { err = L"no bars with a price"; return false; }

    g.days         = sh.weekend ? 7 : 5;
    g.slotsPerDay  = sh.slotsPerDay;
    g.slotSec      = iv;
    g.gmtOffsetSec = gmt;
    g.weeks        = static_cast<size_t>(sh.lastWeek - sh.firstWeek + 1);
    assert(g.weeks >= 1 && g.weeks <= kMaxWeeks);
    const size_t n = g.weeks * g.Slots();
    for (size_t i = 0; i < n; ++i) { g.price[i] = kNaN; g.value[i] = kNaN; g.time[i] = 0; g.volume[i] = 0.0; }
    for (size_t w = 0; w < g.weeks; ++w) {
        g.weekStart[w] = ((sh.firstWeek + static_cast<int64_t>(w)) * 7 - 3) * 86400 - gmt;
    }
    PlaceBars(q.series, gmt, iv, spec.intraday, sh, g);
    FillGaps(g);
    int64_t firstBarWeek = INT64_MAX;
    for (size_t i = 0; i < q.series.count; ++i) {
        if (q.series.pts[i].close > 0.0) { firstBarWeek = WeekOf(LocalDay(q.series.pts[i].time, gmt)); break; }
    }
    ComputeValues(q, firstBarWeek < sh.firstWeek, mode, g);
    ComputeDayStats(g);
    if (mode == WeekValue::Volume) { g.lo = 0.0; }   // volume reads from zero
    if (!(g.hi >= g.lo)) { g.weeks = 0; err = L"no plottable values"; return false; }
    assert(g.lo <= g.hi);
    return true;
}

void RotateWeekView(WeekView& v, float dxPx, float dyPx) {
    v.azimuthDeg += dxPx * 0.4f;
    while (v.azimuthDeg > 180.0f)  { v.azimuthDeg -= 360.0f; }   // at most a few turns per drag event
    while (v.azimuthDeg < -180.0f) { v.azimuthDeg += 360.0f; }
    v.elevationDeg = std::clamp(v.elevationDeg + dyPx * 0.3f, kMinElevation, kMaxElevation);
    assert(v.azimuthDeg >= -180.0f && v.azimuthDeg <= 180.0f);
}

void ZoomWeekView(WeekView& v, float notches) {
    v.zoom = std::clamp(v.zoom * std::pow(1.12f, notches), kMinZoom, kMaxZoom);
    assert(v.zoom >= kMinZoom && v.zoom <= kMaxZoom);
}

// ---------------------------------------------------------------------------
// Projection

namespace {

struct Proj {
    float ca = 1.0f, sa = 0.0f, ce = 1.0f, se = 0.0f;
    float k  = 1.0f, cx = 0.0f, cy = 0.0f;
};

struct P3 {
    float sx, sy, depth;   // depth grows towards the viewer
};

P3 Rotate(const Proj& p, float X, float Y, float Z) {
    const float xr = X * p.ca + Z * p.sa;
    const float zr = -X * p.sa + Z * p.ca;
    return { xr, Y * p.ce - zr * p.se, zr * p.ce + Y * p.se };
}

PointF Screen(const Proj& p, const P3& r) { return PointF(p.cx + p.k * r.sx, p.cy - p.k * r.sy); }
PointF Project(const Proj& p, float X, float Y, float Z) { return Screen(p, Rotate(p, X, Y, Z)); }

RectF PlotRect(const RectF& rc, float s) {
    return RectF(rc.X + 76.0f * s, rc.Y + 40.0f * s, max(10.0f, rc.Width - 170.0f * s),
                 max(10.0f, rc.Height - 80.0f * s));
}

// Fits the rotated box into the plot rectangle.
Proj MakeProj(const RectF& rc, const WeekView& v, float s) {
    Proj p;
    const float a = v.azimuthDeg * kPi / 180.0f;
    const float e = v.elevationDeg * kPi / 180.0f;
    p.ca = std::cos(a); p.sa = std::sin(a); p.ce = std::cos(e); p.se = std::sin(e);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < 8; ++i) {
        const P3 r = Rotate(p, (i & 1) ? kBoxW / 2 : -kBoxW / 2, (i & 2) ? kBoxH : 0.0f,
                            (i & 4) ? kBoxD / 2 : -kBoxD / 2);
        x0 = min(x0, r.sx); x1 = max(x1, r.sx); y0 = min(y0, r.sy); y1 = max(y1, r.sy);
    }
    const RectF pr = PlotRect(rc, s);
    p.k  = v.zoom * min(pr.Width / max(x1 - x0, 1e-3f), pr.Height / max(y1 - y0, 1e-3f));
    p.cx = pr.X + pr.Width / 2.0f - p.k * (x0 + x1) / 2.0f;
    p.cy = pr.Y + pr.Height / 2.0f + p.k * (y0 + y1) / 2.0f;
    assert(p.k > 0.0f);
    return p;
}

float WX(const WeekGrid& g, double slot) {
    const size_t S = g.Slots();
    return S > 1 ? static_cast<float>(slot / static_cast<double>(S - 1) - 0.5) * kBoxW : 0.0f;
}
float WZ(const WeekGrid& g, double week) {
    return g.weeks > 1 ? static_cast<float>(week / static_cast<double>(g.weeks - 1) - 0.5) * kBoxD : 0.0f;
}
float WY(const WeekGrid& g, double v) {
    const double span = g.hi - g.lo;
    return span > 0.0 ? static_cast<float>((v - g.lo) / span) * kBoxH : kBoxH / 2.0f;
}

// Grid indices (no implicit size_t -> double under /W4).
float WX(const WeekGrid& g, size_t slot) { return WX(g, static_cast<double>(slot)); }
float WZ(const WeekGrid& g, size_t week) { return WZ(g, static_cast<double>(week)); }

double Cell(const WeekGrid& g, size_t w, size_t s) { return g.value[w * g.Slots() + s]; }

// ---------------------------------------------------------------------------
// Colours

Color Mix(const Color& a, const Color& b, float t, BYTE alpha = 255) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [t](BYTE x, BYTE y) { return static_cast<BYTE>(std::lround(x + (y - x) * t)); };
    return Color(alpha, ch(a.GetR(), b.GetR()), ch(a.GetG(), b.GetG()), ch(a.GetB(), b.GetB()));
}

Color Shade(const Color& c, float f) {
    f = std::clamp(f, 0.0f, 1.2f);
    auto ch = [f](BYTE x) { return static_cast<BYTE>(min(255L, std::lround(x * f))); };
    return Color(c.GetA(), ch(c.GetR()), ch(c.GetG()), ch(c.GetB()));
}

// Price: a perceptually ordered ramp. Percent: down / neutral / up, centred on 0.
Color ValueColor(const WeekGrid& g, WeekValue mode, double v, const Theme& th) {
    if (mode == WeekValue::PctChange) {
        const double m = max(std::fabs(g.lo), std::fabs(g.hi));
        const float  t = m > 0.0 ? static_cast<float>(v / m) : 0.0f;
        const Color  mid = th.dark ? Color(255, 90, 92, 98) : Color(255, 214, 216, 220);
        return t < 0.0f ? Mix(mid, th.down, -t) : Mix(mid, th.up, t);
    }
    static const Color ramp[5] = { Color(255, 68, 1, 84), Color(255, 59, 82, 139), Color(255, 33, 145, 140),
                                   Color(255, 94, 201, 98), Color(255, 253, 231, 37) };
    const double span = g.hi - g.lo;
    const float  t = span > 0.0 ? static_cast<float>((v - g.lo) / span) * 4.0f : 2.0f;
    const int    i = std::clamp(static_cast<int>(t), 0, 3);
    return Mix(ramp[i], ramp[i + 1], t - static_cast<float>(i));
}

// A week's curve colour: how the week ended, faded with age.
Color WeekColor(const WeekGrid& g, size_t w, const Theme& th) {
    const size_t S = g.Slots();
    double last = kNaN;
    for (size_t s = 0; s < S; ++s) {
        const double p = g.price[w * S + s];
        if (!std::isnan(p)) { last = p; }
    }
    const Color& c = (std::isnan(last) || last >= g.base[w]) ? th.up : th.down;
    const float age = g.weeks > 1 ? 1.0f - static_cast<float>(w) / static_cast<float>(g.weeks - 1) : 0.0f;
    return Mix(c, th.textMuted, 0.55f * age);
}

Font MakeFont(float pt, float s, INT style = FontStyleRegular) {
    return Font(L"Segoe UI", FontPx(pt, s), style, UnitPixel);
}

double NiceStep(double span, int target) {
    assert(target > 0);
    if (!(span > 0.0)) { return 1.0; }
    const double raw = span / target;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f   = raw / mag;
    const double nice = (f < 1.5) ? 1.0 : (f < 3.0) ? 2.0 : (f < 7.0) ? 5.0 : 10.0;
    return nice * mag;
}

std::wstring ValueLabel(WeekValue mode, double v) {
    if (mode == WeekValue::Volume) { return FormatVolume(v); }
    return mode == WeekValue::PctChange ? FormatPct(v) : FormatPrice(v);
}

// Text anchored at `at`: Near = starts there, Far = ends there, Center = centred on it.
void PutText(Graphics& g, const std::wstring& text, const Font& f, const Color& c, PointF at,
             StringAlignment h, StringAlignment v) {
    constexpr float kW = 800.0f, kH = 400.0f;
    auto origin = [](float p, StringAlignment a, float len) {
        return a == StringAlignmentNear ? p : (a == StringAlignmentFar ? p - len : p - len / 2.0f);
    };
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(text.c_str(), static_cast<INT>(text.size()), &f, RectF(origin(at.X, h, kW), origin(at.Y, v, kH), kW, kH),
                 &fmt, &b);
}

// ---------------------------------------------------------------------------
// Axes: value grid on the two back walls, day and week labels on the floor.

struct Corner { float x, z; };

Corner BackCorner(const Proj& p) {
    Corner best{ -kBoxW / 2, -kBoxD / 2 };
    float  bestDepth = 1e9f;
    for (int i = 0; i < 4; ++i) {
        const Corner c{ (i & 1) ? kBoxW / 2 : -kBoxW / 2, (i & 2) ? kBoxD / 2 : -kBoxD / 2 };
        const float d = Rotate(p, c.x, 0.0f, c.z).depth;
        if (d < bestDepth) { bestDepth = d; best = c; }
    }
    return best;
}

void DrawWalls(Graphics& g, const Proj& p, const WeekGrid& grid, WeekValue mode, const Theme& th, float s) {
    const Corner bc = BackCorner(p);
    const float  fx = -bc.x;   // the far end of each back wall
    const float  fz = -bc.z;
    Pen axis(th.grid, 1.0f * s);
    Pen grid1(Color(th.dark ? 70 : 90, th.grid.GetR(), th.grid.GetG(), th.grid.GetB()), 1.0f * s);
    // Floor outline and the back vertical edges.
    const PointF floor[4] = { Project(p, -kBoxW / 2, 0, -kBoxD / 2), Project(p, kBoxW / 2, 0, -kBoxD / 2),
                              Project(p, kBoxW / 2, 0, kBoxD / 2), Project(p, -kBoxW / 2, 0, kBoxD / 2) };
    g.DrawPolygon(&axis, floor, 4);
    g.DrawLine(&axis, Project(p, bc.x, 0, bc.z), Project(p, bc.x, kBoxH, bc.z));
    g.DrawLine(&axis, Project(p, fx, 0, bc.z), Project(p, fx, kBoxH, bc.z));
    g.DrawLine(&axis, Project(p, bc.x, 0, fz), Project(p, bc.x, kBoxH, fz));

    const Font font = MakeFont(8.0f, s);
    const double step = NiceStep(grid.hi - grid.lo, 5);
    const double first = std::ceil(grid.lo / step) * step;
    for (int i = 0; i < 12; ++i) {
        const double v = first + i * step;
        if (v > grid.hi + step * 1e-6) { break; }
        const float  y = WY(grid, v);
        const PointF a = Project(p, fx, y, bc.z), b = Project(p, bc.x, y, bc.z), c = Project(p, bc.x, y, fz);
        g.DrawLine(&grid1, a, b);
        g.DrawLine(&grid1, b, c);
        const PointF left = (a.X < c.X) ? (a.X < b.X ? a : b) : (c.X < b.X ? c : b);
        PutText(g, ValueLabel(mode, v), font, th.textMuted, PointF(left.X - 6.0f * s, left.Y),
                 StringAlignmentFar, StringAlignmentCenter);
    }
}

void DrawFloorLabels(Graphics& g, const Proj& p, const WeekGrid& grid, const Theme& th, float s) {
    static const wchar_t* kDays[7] = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
    const Font font = MakeFont(8.0f, s);
    Pen sep(Color(th.dark ? 60 : 80, th.grid.GetR(), th.grid.GetG(), th.grid.GetB()), 1.0f * s);
    // Days along the front edge, separators across the floor between them.
    const float front = Rotate(p, 0, 0, kBoxD / 2).depth > Rotate(p, 0, 0, -kBoxD / 2).depth ? kBoxD / 2 : -kBoxD / 2;
    const float out   = front > 0 ? 0.07f : -0.07f;
    const double spd  = static_cast<double>(grid.slotsPerDay);
    for (size_t d = 0; d < grid.days; ++d) {
        const float x = WX(grid, d * spd + (spd - 1.0) / 2.0);
        PutText(g, kDays[d], font, th.textMuted, Project(p, x, 0, front + out), StringAlignmentCenter,
                 StringAlignmentCenter);
        if (d > 0 && grid.slotsPerDay > 1) {
            const float xs = WX(grid, d * spd - 0.5);
            g.DrawLine(&sep, Project(p, xs, 0, -kBoxD / 2), Project(p, xs, 0, kBoxD / 2));
        }
    }
    // Weeks along whichever side edge is on the right of the screen.
    const float side  = Project(p, kBoxW / 2, 0, 0).X > Project(p, -kBoxW / 2, 0, 0).X ? kBoxW / 2 : -kBoxW / 2;
    const float sOut  = side > 0 ? 0.05f : -0.05f;
    const float span  = grid.weeks > 1 ? std::fabs(Project(p, side, 0, WZ(grid, size_t{0})).Y -
                                                   Project(p, side, 0, WZ(grid, static_cast<double>(grid.weeks - 1))).Y) /
                                             static_cast<float>(grid.weeks - 1)
                                       : 100.0f;
    const size_t every = max<size_t>(1, static_cast<size_t>(std::ceil(font.GetHeight(&g) * 1.3f / max(span, 0.5f))));
    for (size_t k = 0; k < grid.weeks; k += every) {
        const size_t w = grid.weeks - 1 - k;   // newest always labelled
        PutText(g, FormatDate(grid.weekStart[w], grid.gmtOffsetSec, DateStyle::DayMonth), font, th.textMuted,
                 Project(p, side + sOut, 0, WZ(grid, static_cast<double>(w))),
                 side > 0 ? StringAlignmentNear : StringAlignmentFar, StringAlignmentCenter);
    }
}

// ---------------------------------------------------------------------------
// Waterfall: weeks back to front, each an opaque ribbon so nearer weeks
// hide the ones behind (hidden-line removal by painting order).

void DrawWaterfall(Graphics& g, const Proj& p, const WeekGrid& grid, const Theme& th, float s,
                   WeeklyScratch& scr) {
    const size_t S = grid.Slots();
    for (size_t w = 0; w < grid.weeks; ++w) {
        scr.order[w] = static_cast<uint32_t>(w);
        scr.depth[w] = Rotate(p, 0.0f, 0.0f, WZ(grid, static_cast<double>(w))).depth;
    }
    std::sort(scr.order.begin(), scr.order.begin() + static_cast<ptrdiff_t>(grid.weeks),
              [&scr](uint32_t a, uint32_t b) { return scr.depth[a] < scr.depth[b]; });
    std::array<PointF, 2 * kMaxWeekSlots + 2> pts;
    for (size_t k = 0; k < grid.weeks; ++k) {
        const size_t w = scr.order[k];
        const float  z = WZ(grid, static_cast<double>(w));
        const Color  c = WeekColor(grid, w, th);
        SolidBrush fill(Mix(th.bg, c, 0.10f, 205));   // mostly opaque: the weeks behind still show faintly
        Pen pen(c, (w + 1 == grid.weeks ? 2.2f : 1.4f) * s);
        pen.SetLineJoin(LineJoinRound);
        size_t s0 = 0;
        for (size_t guard = 0; guard < S && s0 < S; ++guard) {   // one ribbon per run of real cells
            while (s0 < S && std::isnan(Cell(grid, w, s0))) { ++s0; }
            size_t s1 = s0;
            while (s1 < S && !std::isnan(Cell(grid, w, s1))) { ++s1; }
            if (s1 > s0) {
                size_t n = 0;
                for (size_t i = s0; i < s1; ++i) {
                    pts[n++] = Project(p, WX(grid, static_cast<double>(i)), WY(grid, Cell(grid, w, i)), z);
                }
                const size_t line = n;
                pts[n++] = Project(p, WX(grid, static_cast<double>(s1 - 1)), 0.0f, z);
                pts[n++] = Project(p, WX(grid, static_cast<double>(s0)), 0.0f, z);
                g.FillPolygon(&fill, pts.data(), static_cast<INT>(n));
                if (line > 1) {
                    g.DrawLines(&pen, pts.data(), static_cast<INT>(line));
                } else {
                    g.DrawLine(&pen, pts[0], pts[1]);
                }
            }
            s0 = s1;
        }
    }
}

// ---------------------------------------------------------------------------
// Surface: quads sorted far to near, coloured by value and lit from above.

void DrawSurface(Graphics& g, const Proj& p, const WeekGrid& grid, WeekValue mode, const Theme& th, float s,
                 WeeklyScratch& scr) {
    const size_t S = grid.Slots();
    if (grid.weeks < 2 || S < 2) { return; }
    size_t n = 0;
    for (size_t w = 0; w + 1 < grid.weeks; ++w) {
        for (size_t c = 0; c + 1 < S; ++c) {
            const double a = Cell(grid, w, c), b = Cell(grid, w, c + 1);
            const double d = Cell(grid, w + 1, c), e = Cell(grid, w + 1, c + 1);
            if (std::isnan(a) || std::isnan(b) || std::isnan(d) || std::isnan(e)) { continue; }
            const size_t cell = w * S + c;
            const float  y    = WY(grid, (a + b + d + e) / 4.0);
            scr.order[n]    = static_cast<uint32_t>(cell);
            scr.depth[cell] = Rotate(p, WX(grid, c + 0.5), y, WZ(grid, w + 0.5)).depth;   // keyed by cell
            ++n;
        }
    }
    assert(n <= kMaxWeekCells);
    std::array<uint32_t, kMaxWeekCells>& ord = scr.order;
    std::sort(ord.begin(), ord.begin() + static_cast<ptrdiff_t>(n),
              [&scr](uint32_t x, uint32_t y) { return scr.depth[x] < scr.depth[y]; });
    const float lightLen = std::sqrt(0.3f * 0.3f + 1.0f + 0.4f * 0.4f);
    const bool  edges = p.k * kBoxW / static_cast<float>(S) > 5.0f * s;
    Pen edge(Color(40, 0, 0, 0), 0.6f * s);
    for (size_t i = 0; i < n; ++i) {
        const size_t w = ord[i] / S, c = ord[i] % S;
        const double v[4] = { Cell(grid, w, c), Cell(grid, w, c + 1), Cell(grid, w + 1, c + 1), Cell(grid, w + 1, c) };
        const float  xs[4] = { WX(grid, c), WX(grid, c + 1), WX(grid, c + 1), WX(grid, c) };
        const float  zs[4] = { WZ(grid, w), WZ(grid, w), WZ(grid, w + 1), WZ(grid, w + 1) };
        PointF q[4];
        float  ys[4];
        for (int k = 0; k < 4; ++k) { ys[k] = WY(grid, v[k]); q[k] = Project(p, xs[k], ys[k], zs[k]); }
        // Normal from the two diagonals; light from above and slightly in front.
        const float ux = xs[2] - xs[0], uy = ys[2] - ys[0], uz = zs[2] - zs[0];
        const float vx = xs[3] - xs[1], vy = ys[3] - ys[1], vz = zs[3] - zs[1];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }
        if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
        const float lambert = std::fabs(nx * -0.3f + ny * 1.0f + nz * 0.4f) / lightLen;
        SolidBrush b(Shade(ValueColor(grid, mode, (v[0] + v[1] + v[2] + v[3]) / 4.0, th), 0.55f + 0.5f * lambert));
        g.FillPolygon(&b, q, 4);
        if (edges) { g.DrawPolygon(&edge, q, 4); }
    }
}

void DrawMessage(Graphics& g, const RectF& rc, const wchar_t* text, const Theme& th, float s) {
    const Font f = MakeFont(11.0f, s);
    PutText(g, text, f, th.textMuted, PointF(rc.X + rc.Width / 2, rc.Y + rc.Height / 2), StringAlignmentCenter,
             StringAlignmentCenter);
}

const wchar_t* const kDayNames[7] = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };

// ---------------------------------------------------------------------------
// Week2D: one week as an ordinary line chart, the week before dashed behind it.

struct Flat {
    RectF  plot;
    double lo   = 0.0;
    double hi   = 1.0;
    size_t row  = 0;       // grid row shown
    bool   prev = false;   // row - 1 has data (the dashed reference)
};

bool RowHasData(const WeekGrid& g, size_t w) {
    assert(w < g.weeks);
    for (size_t c = 0; c < g.Slots(); ++c) {
        if (!std::isnan(Cell(g, w, c))) { return true; }
    }
    return false;
}

// Reference level: the previous close in Price mode, 0 % in percent mode,
// none (NaN) for volume.
double BaseLevel(const WeekGrid& g, WeekValue mode, size_t row) {
    if (mode == WeekValue::Volume) { return kNaN; }
    return mode == WeekValue::PctChange ? 0.0 : g.base[row];
}

Flat MakeFlat(const RectF& rc, const WeeklyInput& in, float s) {
    assert(in.grid != nullptr && in.grid->weeks > 0);
    const WeekGrid& g = *in.grid;
    Flat f;
    f.plot = RectF(rc.X + 72.0f * s, rc.Y + 84.0f * s, max(10.0f, rc.Width - 148.0f * s),   // price axis on the right
                   max(10.0f, rc.Height - 172.0f * s));   // room below for the day labels and stats
    f.row  = WeekRowFor(g, in.weeksAgo);
    f.prev = f.row > 0 && RowHasData(g, f.row - 1);
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    const double base = BaseLevel(g, in.mode, f.row);
    if (in.mode == WeekValue::Volume) { lo = hi = 0.0; }
    else if (base > 0.0 || in.mode == WeekValue::PctChange) { lo = hi = base; }
    for (size_t k = 0; k < (f.prev ? 2u : 1u); ++k) {
        const size_t w = f.row - k;
        for (size_t c = 0; c < g.Slots(); ++c) {
            const double v = Cell(g, w, c);
            if (!std::isnan(v)) { lo = min(lo, v); hi = max(hi, v); }
        }
    }
    if (!(hi >= lo)) { lo = 0.0; hi = 1.0; }
    double pad = (hi - lo) * 0.08;
    if (!(pad > 0.0)) { pad = max(std::fabs(hi) * 0.01, 0.5); }
    f.lo = (in.mode == WeekValue::Volume) ? 0.0 : lo - pad;
    f.hi = hi + pad;
    assert(f.hi > f.lo);
    return f;
}

float FX(const Flat& f, size_t slots, double slot) {
    assert(slots > 0);
    return f.plot.X + static_cast<float>((slot + 0.5) / static_cast<double>(slots)) * f.plot.Width;
}
float FY(const Flat& f, double v) {
    return f.plot.Y + f.plot.Height - static_cast<float>((v - f.lo) / (f.hi - f.lo)) * f.plot.Height;
}

void DrawRowLine(Graphics& g, const Flat& f, const WeekGrid& grid, size_t row, Pen& pen) {
    const size_t S = grid.Slots();
    std::array<PointF, kMaxWeekSlots> pts;
    size_t n = 0;
    for (size_t c = 0; c <= S; ++c) {   // one polyline per run of real cells
        const bool real = c < S && !std::isnan(Cell(grid, row, c));
        if (real) { pts[n++] = PointF(FX(f, S, static_cast<double>(c)), FY(f, Cell(grid, row, c))); }
        if (!real && n > 1) { g.DrawLines(&pen, pts.data(), static_cast<INT>(n)); }
        if (!real) { n = 0; }
    }
}

void DrawFlatAxes(Graphics& g, const Flat& f, const WeekGrid& grid, WeekValue mode, const Theme& th, float s) {
    const Font font = MakeFont(8.0f, s);
    Pen gridPen(Color(th.dark ? 70 : 90, th.grid.GetR(), th.grid.GetG(), th.grid.GetB()), 1.0f * s);
    const double step  = NiceStep(f.hi - f.lo, 6);
    const double first = std::ceil(f.lo / step) * step;
    for (int i = 0; i < 16; ++i) {
        const double v = first + i * step;
        if (v > f.hi) { break; }
        const float y = FY(f, v);
        g.DrawLine(&gridPen, f.plot.X, y, f.plot.X + f.plot.Width, y);
        PutText(g, ValueLabel(mode, v), font, th.textMuted, PointF(f.plot.X - 6.0f * s, y), StringAlignmentFar,
                StringAlignmentCenter);
    }
    // Day columns: separators between them, weekday and date underneath.
    const size_t S = grid.Slots();
    const double spd = static_cast<double>(grid.slotsPerDay);
    Pen axis(th.grid, 1.0f * s);
    for (size_t d = 0; d < grid.days; ++d) {
        if (d > 0) {
            const float x = FX(f, S, d * spd - 0.5);
            g.DrawLine(&axis, x, f.plot.Y, x, f.plot.Y + f.plot.Height);
        }
        const int64_t noon = grid.weekStart[f.row] + static_cast<int64_t>(d) * 86400 + 43200;
        const std::wstring label = std::wstring(kDayNames[d]) + L" " +
                                   FormatDate(noon, grid.gmtOffsetSec, DateStyle::DayMonth);
        PutText(g, label, font, th.textMuted, PointF(FX(f, S, d * spd + (spd - 1.0) / 2.0), f.plot.Y + f.plot.Height + 12.0f * s),
                StringAlignmentCenter, StringAlignmentCenter);
    }
    g.DrawRectangle(&axis, f.plot);
}

// "3 weeks ago · week of 20 Oct 2026 · +1.23% · close 123.45" above the plot.
void DrawFlatReadout(Graphics& g, const RectF& rc, const Flat& f, const WeekGrid& grid, WeekValue mode, const Theme& th,
                     float s) {
    const size_t weeksAgo = grid.weeks - 1 - f.row;
    const Font big   = MakeFont(14.0f, s, FontStyleBold);
    const Font detailFont = MakeFont(9.0f, s);
    const std::wstring head = WeeksAgoLabel(weeksAgo);
    const PointF at(rc.X + 12.0f * s, rc.Y + 52.0f * s);
    PutText(g, head, big, th.text, at, StringAlignmentNear, StringAlignmentCenter);
    RectF box;
    g.MeasureString(head.c_str(), static_cast<INT>(head.size()), &big, PointF(0, 0), &box);
    std::wstring detail = L"week of " + FormatDate(grid.weekStart[f.row], grid.gmtOffsetSec, DateStyle::Full);
    double last = kNaN;
    for (size_t c = 0; c < grid.Slots(); ++c) {
        const double p = grid.price[f.row * grid.Slots() + c];
        if (!std::isnan(p)) { last = p; }
    }
    if (!std::isnan(last) && grid.base[f.row] > 0.0) {
        detail += L"  ·  " + FormatPct((last / grid.base[f.row] - 1.0) * 100.0) + L"  ·  last " + FormatPrice(last) +
                  L"  ·  previous close " + FormatPrice(grid.base[f.row]);
    }
    PutText(g, detail, detailFont, th.textMuted, PointF(at.X + box.Width + 10.0f * s, at.Y + 2.0f * s), StringAlignmentNear,
            StringAlignmentCenter);
    if (f.prev && mode != WeekValue::Volume) {   // Volume mode draws no dashed week
        PutText(g, L"dashed: the week before", detailFont, th.textMuted, PointF(rc.X + rc.Width - 12.0f * s, at.Y + 2.0f * s),
                StringAlignmentFar, StringAlignmentCenter);
    }
}

// Under each day column: how that weekday behaves over every week loaded,
// with the biggest mover and the busiest day picked out.
void DrawDayStats(Graphics& g, const RectF& rc, const Flat& f, const WeekGrid& grid, const Theme& th, float s) {
    const Font font = MakeFont(8.0f, s);
    const Font bold = MakeFont(8.0f, s, FontStyleBold);
    size_t bigMove = 0, bigVol = 0;
    for (size_t d = 1; d < grid.days; ++d) {
        if (grid.dayStats[d].absMovePct > grid.dayStats[bigMove].absMovePct) { bigMove = d; }
        if (grid.dayStats[d].volRatio > grid.dayStats[bigVol].volRatio) { bigVol = d; }
    }
    const size_t S   = grid.Slots();
    const double spd = static_cast<double>(grid.slotsPerDay);
    const float  y   = f.plot.Y + f.plot.Height + 32.0f * s;
    for (size_t d = 0; d < grid.days; ++d) {
        const DayStat& st = grid.dayStats[d];
        if (st.n == 0) { continue; }
        const float x = FX(f, S, d * spd + (spd - 1.0) / 2.0);
        wchar_t size[32];
        swprintf_s(size, L"avg move %.2f%%", st.absMovePct);
        const std::wstring move = std::wstring(size) + L" (" + FormatPct(st.movePct) + L")";
        PutText(g, move, d == bigMove ? bold : font, d == bigMove ? th.text : th.textMuted, PointF(x, y),
                StringAlignmentCenter, StringAlignmentCenter);
        wchar_t ratio[32];
        swprintf_s(ratio, L"volume %.2fx avg", st.volRatio);
        PutText(g, ratio, d == bigVol ? bold : font, d == bigVol ? th.text : th.textMuted, PointF(x, y + 15.0f * s),
                StringAlignmentCenter, StringAlignmentCenter);
    }
    const std::wstring note = L"Per weekday over all " + std::to_wstring(grid.weeks) +
        L" weeks loaded: mean size of the close-to-close move (mean signed move in brackets) and volume against an "
        L"average day. Bold = largest.";
    PutText(g, note, font, th.textMuted, PointF(rc.X + 12.0f * s, rc.Y + rc.Height - 12.0f * s), StringAlignmentNear,
            StringAlignmentCenter);
}

// Volume bars along the bottom fifth of the plot, scaled to the week's busiest bar.
void DrawVolumeBars(Graphics& g, const Flat& f, const WeekGrid& grid, const Theme& th, float heightFrac) {
    assert(heightFrac > 0.0f && heightFrac <= 1.0f);
    const size_t S = grid.Slots();
    double top = 0.0;
    for (size_t c = 0; c < S; ++c) { top = max(top, grid.volume[f.row * S + c]); }
    if (!(top > 0.0)) { return; }
    const float slotW = f.plot.Width / static_cast<float>(S);
    const float maxH  = f.plot.Height * heightFrac;
    SolidBrush bar(Color(90, th.volFlat.GetR(), th.volFlat.GetG(), th.volFlat.GetB()));
    for (size_t c = 0; c < S; ++c) {
        const double v = grid.volume[f.row * S + c];
        if (!(v > 0.0)) { continue; }
        const float h = static_cast<float>(v / top) * maxH;
        g.FillRectangle(&bar, f.plot.X + slotW * static_cast<float>(c) + slotW * 0.15f,
                        f.plot.Y + f.plot.Height - h, slotW * 0.7f, h);
    }
}

// Right-hand price axis. In Price mode it mirrors the left axis; in % mode
// price and percent are tied by the previous close, so the same y reads as
// both; in Volume mode it is a scale of its own for the price line drawn
// over the bars.
struct PriceAxis {
    bool   ok = false;
    double lo = 0.0;   // price at the bottom of the plot
    double hi = 1.0;   // and at the top
};

PriceAxis MakePriceAxis(const Flat& f, const WeekGrid& g, WeekValue mode) {
    PriceAxis a;
    if (mode == WeekValue::Price) {
        a.lo = f.lo; a.hi = f.hi; a.ok = true;
    } else if (mode == WeekValue::PctChange) {
        const double base = g.base[f.row];
        if (base > 0.0) { a.lo = base * (1.0 + f.lo / 100.0); a.hi = base * (1.0 + f.hi / 100.0); a.ok = true; }
    } else {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -lo;
        for (size_t c = 0; c < g.Slots(); ++c) {
            const double p = g.price[f.row * g.Slots() + c];
            if (!std::isnan(p)) { lo = min(lo, p); hi = max(hi, p); }
        }
        if (hi >= lo) {
            double pad = (hi - lo) * 0.08;
            if (!(pad > 0.0)) { pad = max(hi * 0.01, 0.01); }
            a.lo = lo - pad; a.hi = hi + pad; a.ok = true;
        }
    }
    a.ok = a.ok && a.hi > a.lo;
    return a;
}

float PY(const Flat& f, const PriceAxis& a, double price) {
    assert(a.ok && a.hi > a.lo);
    return f.plot.Y + f.plot.Height - static_cast<float>((price - a.lo) / (a.hi - a.lo)) * f.plot.Height;
}

double LastPrice(const WeekGrid& g, size_t row) {
    double last = kNaN;
    for (size_t c = 0; c < g.Slots(); ++c) {
        const double p = g.price[row * g.Slots() + c];
        if (!std::isnan(p)) { last = p; }
    }
    return last;
}

// Ticks and labels down the right edge, axis captions on both sides, and a
// tag with the week's last price in the week's colour.
void DrawPriceAxis(Graphics& g, const Flat& f, const PriceAxis& a, const WeekGrid& grid, WeekValue mode,
                   const Theme& th, float s) {
    const Font  font  = MakeFont(8.0f, s);
    const float right = f.plot.X + f.plot.Width;
    const wchar_t* leftCaption = mode == WeekValue::Price ? L"Price" : mode == WeekValue::PctChange ? L"% vs previous close" : L"Volume";
    PutText(g, leftCaption, font, th.textMuted, PointF(f.plot.X, f.plot.Y - 10.0f * s), StringAlignmentNear,
            StringAlignmentCenter);
    if (!a.ok) { return; }
    PutText(g, L"Price", font, th.textMuted, PointF(right, f.plot.Y - 10.0f * s), StringAlignmentFar,
            StringAlignmentCenter);
    Pen tick(th.grid, 1.0f * s);
    const double step  = NiceStep(a.hi - a.lo, 6);
    const double first = std::ceil(a.lo / step) * step;
    for (int i = 0; i < 16; ++i) {
        const double p = first + i * step;
        if (p > a.hi) { break; }
        const float y = PY(f, a, p);
        g.DrawLine(&tick, right, y, right + 4.0f * s, y);
        PutText(g, FormatPrice(p), font, th.textMuted, PointF(right + 7.0f * s, y), StringAlignmentNear,
                StringAlignmentCenter);
    }
    const double last = LastPrice(grid, f.row);
    if (std::isnan(last) || last < a.lo || last > a.hi) { return; }
    const Font   bold = MakeFont(8.0f, s, FontStyleBold);
    const std::wstring text = FormatPrice(last);
    RectF box;
    g.MeasureString(text.c_str(), static_cast<INT>(text.size()), &bold, PointF(0, 0), &box);
    const float y = PY(f, a, last);
    SolidBrush tag(last >= grid.base[f.row] ? th.up : th.down);
    g.FillRectangle(&tag, right + 2.0f * s, y - box.Height / 2.0f - 1.0f * s, box.Width + 8.0f * s, box.Height + 2.0f * s);
    PutText(g, text, bold, Color(255, 255, 255, 255), PointF(right + 6.0f * s, y), StringAlignmentNear,
            StringAlignmentCenter);
}

// The shown week's price as a line on the right-hand scale (Volume mode).
void DrawPriceOverlay(Graphics& g, const Flat& f, const PriceAxis& a, const WeekGrid& grid, const Theme& th, float s) {
    if (!a.ok) { return; }
    const size_t S = grid.Slots();
    Pen pen(LastPrice(grid, f.row) >= grid.base[f.row] ? th.up : th.down, 2.0f * s);
    pen.SetLineJoin(LineJoinRound);
    std::array<PointF, kMaxWeekSlots> pts;
    size_t n = 0;
    for (size_t c = 0; c <= S; ++c) {
        const double p = c < S ? grid.price[f.row * S + c] : kNaN;
        if (!std::isnan(p)) { pts[n++] = PointF(FX(f, S, static_cast<double>(c)), PY(f, a, p)); }
        if (std::isnan(p) && n > 1) { g.DrawLines(&pen, pts.data(), static_cast<INT>(n)); }
        if (std::isnan(p)) { n = 0; }
    }
}

void DrawWeek2D(Graphics& g, const RectF& rc, const WeeklyInput& in, float s) {
    const WeekGrid& grid = *in.grid;
    const Theme&    th   = *in.theme;
    const Flat      f    = MakeFlat(rc, in, s);
    DrawFlatAxes(g, f, grid, in.mode, th, s);
    DrawFlatReadout(g, rc, f, grid, in.mode, th, s);
    DrawDayStats(g, rc, f, grid, th, s);
    const PriceAxis axis = MakePriceAxis(f, grid, in.mode);
    DrawPriceAxis(g, f, axis, grid, in.mode, th, s);
    if (in.mode == WeekValue::Volume) {   // the week's volume is the chart: full-height bars
        if (!RowHasData(grid, f.row)) { DrawMessage(g, f.plot, L"No trading that week", th, s); return; }
        // Bars are scaled to the axis, so reuse the axis range rather than the week's own peak.
        const size_t S = grid.Slots();
        const float  slotW = f.plot.Width / static_cast<float>(S);
        SolidBrush bar(th.line);
        for (size_t c = 0; c < S; ++c) {
            const double v = grid.volume[f.row * S + c];
            if (!(v > 0.0)) { continue; }
            const float y = FY(f, v);
            g.FillRectangle(&bar, f.plot.X + slotW * static_cast<float>(c) + slotW * 0.15f, y, slotW * 0.7f,
                            f.plot.Y + f.plot.Height - y);
        }
        DrawPriceOverlay(g, f, axis, grid, th, s);
        return;
    }
    DrawVolumeBars(g, f, grid, th, 0.2f);
    const double base = BaseLevel(grid, in.mode, f.row);
    if (base > 0.0 || in.mode == WeekValue::PctChange) {
        Pen basePen(th.textMuted, 1.0f * s);
        basePen.SetDashStyle(DashStyleDot);
        g.DrawLine(&basePen, f.plot.X, FY(f, base), f.plot.X + f.plot.Width, FY(f, base));
    }
    if (f.prev) {
        Pen prevPen(Color(160, th.textMuted.GetR(), th.textMuted.GetG(), th.textMuted.GetB()), 1.3f * s);
        prevPen.SetDashStyle(DashStyleDash);
        DrawRowLine(g, f, grid, f.row - 1, prevPen);
    }
    if (!RowHasData(grid, f.row)) {
        DrawMessage(g, f.plot, L"No trading that week", th, s);
        return;
    }
    Pen pen(th.line, 2.2f * s);
    double last = kNaN;
    for (size_t c = 0; c < grid.Slots(); ++c) {
        const double p = grid.price[f.row * grid.Slots() + c];
        if (!std::isnan(p)) { last = p; }
    }
    pen.SetColor(std::isnan(last) || last >= grid.base[f.row] ? th.up : th.down);
    pen.SetLineJoin(LineJoinRound);
    DrawRowLine(g, f, grid, f.row, pen);
}

// Tooltip for cell (w, c) near the mouse, kept inside `rc`.
void DrawTip(Graphics& g, const RectF& rc, const WeeklyInput& in, size_t w, size_t c, float s) {
    const WeekGrid& grid  = *in.grid;
    const Theme&    th    = *in.theme;
    const size_t    cell  = w * grid.Slots() + c;
    const double    price = grid.price[cell];
    const double    pct   = grid.base[w] > 0.0 ? (price / grid.base[w] - 1.0) * 100.0 : 0.0;
    const bool      intra = grid.slotSec < 86400;
    std::wstring tip = L"Week of " + FormatDate(grid.weekStart[w], grid.gmtOffsetSec, DateStyle::Full) + L"\n";
    if (grid.time[cell] != 0) {
        tip += FormatDate(grid.time[cell], grid.gmtOffsetSec, intra ? DateStyle::DayTime : DateStyle::DayMonth);
    } else {
        tip += L"(no trade: last price held)";
    }
    tip += L"   " + FormatPrice(price) + L"\n" + FormatPct(pct) + L" on the week";
    const Font f = MakeFont(9.0f, s);
    RectF box;
    g.MeasureString(tip.c_str(), static_cast<INT>(tip.size()), &f, PointF(0, 0), &box);
    float bx = static_cast<float>(in.hoverX) + 14.0f * s;
    float by = static_cast<float>(in.hoverY) + 14.0f * s;
    if (bx + box.Width + 12.0f * s > rc.X + rc.Width)  { bx = static_cast<float>(in.hoverX) - box.Width - 26.0f * s; }
    if (by + box.Height + 10.0f * s > rc.Y + rc.Height) { by = static_cast<float>(in.hoverY) - box.Height - 24.0f * s; }
    SolidBrush tipBg(th.tipBg);
    g.FillRectangle(&tipBg, bx, by, box.Width + 12.0f * s, box.Height + 10.0f * s);
    SolidBrush tipText(th.tipText);
    g.DrawString(tip.c_str(), static_cast<INT>(tip.size()), &f, PointF(bx + 6.0f * s, by + 5.0f * s), &tipText);
}

} // namespace

size_t WeekRowFor(const WeekGrid& g, size_t weeksAgo) {
    assert(g.weeks > 0);
    return g.weeks - 1 - min(weeksAgo, g.weeks - 1);
}

std::wstring WeeksAgoLabel(size_t weeksAgo) {
    if (weeksAgo == 0) { return L"This week"; }
    if (weeksAgo == 1) { return L"Last week"; }
    return std::to_wstring(weeksAgo) + L" weeks ago";
}

// ---------------------------------------------------------------------------
// Public drawing entry points

void DrawWeeklyBase(Graphics& g, const RectF& rc, const WeeklyInput& in, float s, WeeklyScratch& scratch) {
    assert(in.theme != nullptr && s > 0.0f);
    const Theme& th = *in.theme;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    SolidBrush bg(th.bg);
    g.FillRectangle(&bg, rc);
    const Font titleFont = MakeFont(11.0f, s, FontStyleBold);
    const Font hintFont  = MakeFont(8.0f, s);
    const bool flat = in.style == WeekStyle::Week2D;
    PutText(g, in.title, titleFont, th.text, PointF(rc.X + 12.0f * s, rc.Y + 16.0f * s), StringAlignmentNear,
             StringAlignmentCenter);
    PutText(g, flat ? L"Wheel or ← → to step through weeks · Home = this week"
                    : L"Drag to rotate · wheel to zoom · double-click to reset",
            hintFont, th.textMuted, PointF(rc.X + rc.Width - 12.0f * s, rc.Y + 16.0f * s), StringAlignmentFar,
            StringAlignmentCenter);
    if (in.grid == nullptr || in.grid->weeks == 0) {
        DrawMessage(g, rc, in.message, th, s);
        return;
    }
    if (flat) {
        DrawWeek2D(g, rc, in, s);
        return;
    }
    const WeekGrid& grid = *in.grid;
    const Proj p = MakeProj(rc, in.view, s);
    DrawWalls(g, p, grid, in.mode, th, s);
    DrawFloorLabels(g, p, grid, th, s);
    if (in.style == WeekStyle::Surface) {
        DrawSurface(g, p, grid, in.mode, th, s, scratch);
    } else {
        DrawWaterfall(g, p, grid, th, s, scratch);
    }
}

bool WeeklyPriceY(const RectF& rc, const WeeklyInput& in, float s, double price, float& y) {
    if (in.grid == nullptr || in.grid->weeks == 0 || in.style != WeekStyle::Week2D) { return false; }
    const Flat      f = MakeFlat(rc, in, s);
    const PriceAxis a = MakePriceAxis(f, *in.grid, in.mode);
    if (!a.ok) { return false; }
    y = PY(f, a, price);
    return true;
}

bool WeeklyProject(const RectF& rc, const WeeklyInput& in, float s, size_t week, size_t slot, PointF& out) {
    if (in.grid == nullptr || week >= in.grid->weeks || slot >= in.grid->Slots()) { return false; }
    const double v = Cell(*in.grid, week, slot);
    if (std::isnan(v)) { return false; }
    if (in.style == WeekStyle::Week2D) {
        const Flat f = MakeFlat(rc, in, s);
        out = PointF(FX(f, in.grid->Slots(), static_cast<double>(slot)), FY(f, v));
        return true;
    }
    const Proj p = MakeProj(rc, in.view, s);
    out = Project(p, WX(*in.grid, static_cast<double>(slot)), WY(*in.grid, v), WZ(*in.grid, static_cast<double>(week)));
    return true;
}

bool WeeklyHit(const RectF& rc, const WeeklyInput& in, float s, float x, float y, float radiusPx, size_t& week,
               size_t& slot) {
    if (in.grid == nullptr || in.grid->weeks == 0) { return false; }
    const WeekGrid& grid = *in.grid;
    const size_t S = grid.Slots();
    if (in.style == WeekStyle::Week2D) {   // the shown week's slot under the mouse
        const Flat f = MakeFlat(rc, in, s);
        if (x < f.plot.X || x >= f.plot.X + f.plot.Width || y < f.plot.Y || y > f.plot.Y + f.plot.Height) { return false; }
        const size_t c = min(S - 1, static_cast<size_t>((x - f.plot.X) / f.plot.Width * static_cast<float>(S)));
        if (std::isnan(Cell(grid, f.row, c))) { return false; }
        week = f.row;
        slot = c;
        return true;
    }
    const Proj p = MakeProj(rc, in.view, s);
    float best = radiusPx * radiusPx;
    float bestDepth = -1e9f;
    bool  found = false;
    for (size_t w = 0; w < grid.weeks; ++w) {
        for (size_t c = 0; c < S; ++c) {
            const double v = Cell(grid, w, c);
            if (std::isnan(v)) { continue; }
            const P3     r = Rotate(p, WX(grid, static_cast<double>(c)), WY(grid, v), WZ(grid, static_cast<double>(w)));
            const PointF q = Screen(p, r);
            const float  d2 = (q.X - x) * (q.X - x) + (q.Y - y) * (q.Y - y);
            // Nearest on screen; among near-ties the one in front (what the user sees).
            if (d2 < best - 4.0f || (d2 <= best + 4.0f && found && r.depth > bestDepth)) {
                best = min(best, d2); bestDepth = r.depth; week = w; slot = c; found = true;
            }
        }
    }
    assert(!found || (week < grid.weeks && slot < S));
    return found;
}

void DrawWeeklyOverlay(Graphics& g, const RectF& rc, const WeeklyInput& in, float s) {
    assert(in.theme != nullptr);
    size_t w = 0, c = 0;
    if (in.hoverX < 0 || !WeeklyHit(rc, in, s, static_cast<float>(in.hoverX), static_cast<float>(in.hoverY),
                                     16.0f * s, w, c)) {
        return;
    }
    const Theme&    th   = *in.theme;
    const WeekGrid& grid = *in.grid;
    const size_t    S    = grid.Slots();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    PointF at;
    if (!WeeklyProject(rc, in, s, w, c, at)) { return; }
    if (in.style == WeekStyle::Week2D) {
        const Flat f = MakeFlat(rc, in, s);
        Pen cross(th.hover, 1.0f * s);
        g.DrawLine(&cross, at.X, f.plot.Y, at.X, f.plot.Y + f.plot.Height);
    } else {
        // The hovered week, drawn over everything.
        const Proj p = MakeProj(rc, in.view, s);
        Pen hi(th.text, 2.4f * s);
        hi.SetLineJoin(LineJoinRound);
        std::array<PointF, kMaxWeekSlots> pts;
        size_t n = 0;
        const float z = WZ(grid, static_cast<double>(w));
        for (size_t i = 0; i <= S; ++i) {
            const bool real = i < S && !std::isnan(Cell(grid, w, i));
            if (real) { pts[n++] = Project(p, WX(grid, static_cast<double>(i)), WY(grid, Cell(grid, w, i)), z); }
            if (!real && n > 1) { g.DrawLines(&hi, pts.data(), static_cast<INT>(n)); }
            if (!real) { n = 0; }
        }
    }
    SolidBrush dot(th.hover);
    g.FillEllipse(&dot, at.X - 4.0f * s, at.Y - 4.0f * s, 8.0f * s, 8.0f * s);
    DrawTip(g, rc, in, w, c, s);
}

} // namespace st
