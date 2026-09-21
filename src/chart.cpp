#include "chart.h"

#include "indicators.h"
#include "textfmt.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace st {
namespace {

using namespace Gdiplus;

constexpr float  kMinPriceRange = 0.01f;
constexpr size_t kSmaShort      = 20;
constexpr size_t kSmaLong       = 50;
constexpr size_t kBollPeriod    = 20;
constexpr double kBollK         = 2.0;
constexpr size_t kRsiPeriod     = 14;

struct Layout {
    RectF price;   // main price plot
    RectF volume;  // volume bars
    RectF rsi;     // RSI pane (zero height when off)
    RectF axisR;   // right-hand price labels
    RectF axisB;   // bottom date labels
};

struct PriceScale {
    double lo = 0.0;
    double hi = 1.0;
};

Layout MakeLayout(const RectF& rc, float s, bool withVolume, bool withRsi) {
    assert(s > 0.0f);
    const float axisW = 64.0f * s;
    const float axisH = 20.0f * s;
    const float gap   = 6.0f * s;
    const float plotW = max(rc.Width - axisW, 10.0f);
    const float plotH = max(rc.Height - axisH, 10.0f);
    const float rsiH  = withRsi ? std::floor(plotH * 0.18f) : 0.0f;
    const float volH  = withVolume ? std::floor(plotH * 0.20f) : 0.0f;
    Layout l;
    float bottom = rc.Y + plotH;
    l.rsi = RectF(rc.X, bottom - rsiH, plotW, rsiH);
    if (withRsi) { bottom -= rsiH + gap; }
    l.volume = RectF(rc.X, bottom - volH, plotW, volH);
    if (withVolume) { bottom -= volH + gap; }
    l.price = RectF(rc.X, rc.Y, plotW, max(bottom - rc.Y, 10.0f));
    l.axisR = RectF(rc.X + plotW, rc.Y, axisW, plotH);
    l.axisB = RectF(rc.X, rc.Y + plotH, plotW, axisH);
    return l;
}

void Expand(PriceScale& sc, double v) {
    if (!std::isfinite(v)) { return; }
    sc.lo = min(sc.lo, v);
    sc.hi = max(sc.hi, v);
}

bool ComputeScale(const Series& s, const ChartOptions& o, PriceScale& out) {
    if (s.count == 0) { return false; }
    PriceScale sc{ s.pts[0].close, s.pts[0].close };
    for (size_t i = 0; i < s.count && i < kMaxPoints; ++i) {
        const Candle& c = s.pts[i];
        Expand(sc, o.candles ? c.low : c.close);
        Expand(sc, o.candles ? c.high : c.close);
    }
    if (o.bollinger && s.count >= kBollPeriod) {
        Track mid;
        Track up;
        Track lo;
        Bollinger(s, kBollPeriod, kBollK, mid, up, lo);
        for (size_t i = 0; i < s.count; ++i) {
            Expand(sc, up[i]);
            Expand(sc, lo[i]);
        }
    }
    if (!std::isfinite(sc.lo) || !std::isfinite(sc.hi)) { return false; }
    double pad = (sc.hi - sc.lo) * 0.06;
    if (pad < kMinPriceRange) { pad = max(std::fabs(sc.hi) * 0.01, static_cast<double>(kMinPriceRange)); }
    out.lo = sc.lo - pad;
    out.hi = sc.hi + pad;
    assert(out.hi > out.lo);
    return true;
}

// Rounds a raw tick step to 1/2/2.5/5 x 10^k.
double NiceStep(double raw) {
    assert(raw > 0.0);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f   = raw / mag;
    double nice = 10.0;
    if (f <= 1.0)      { nice = 1.0; }
    else if (f <= 2.0) { nice = 2.0; }
    else if (f <= 2.5) { nice = 2.5; }
    else if (f <= 5.0) { nice = 5.0; }
    return nice * mag;
}

float YFor(double v, const RectF& r, const PriceScale& sc) {
    assert(sc.hi > sc.lo);
    const double t = (v - sc.lo) / (sc.hi - sc.lo);
    return r.Y + r.Height - static_cast<float>(t) * r.Height;
}

float SlotWidth(const RectF& r, size_t n) {
    assert(n > 0);
    return r.Width / static_cast<float>(n);
}

float XFor(size_t i, const RectF& r, size_t n) {
    return r.X + (static_cast<float>(i) + 0.5f) * SlotWidth(r, n);
}

double SpanDays(const Series& s) {
    if (s.count < 2) { return 0.0; }
    return static_cast<double>(s.pts[s.count - 1].time - s.pts[0].time) / 86400.0;
}

Font MakeFont(float pt, float s, INT style = FontStyleRegular) {
    return Font(L"Segoe UI", FontPx(pt, s), style, UnitPixel);
}

// ---------------------------------------------------------------------------
// Axes

void DrawPriceGrid(Graphics& g, const Layout& L, const PriceScale& sc, const Font& font,
                   const Theme& th, float s, bool percent) {
    const double step  = NiceStep((sc.hi - sc.lo) / 5.0);
    const double first = std::ceil(sc.lo / step) * step;
    Pen        grid(th.grid, 1.0f);
    SolidBrush txt(th.textMuted);
    StringFormat fmt;
    fmt.SetLineAlignment(StringAlignmentCenter);
    const float labelH = 16.0f * s;
    double v = first;
    for (int i = 0; i < 64 && v <= sc.hi; ++i) {
        const float y = YFor(v, L.price, sc);
        if (percent && std::fabs(v) < step / 2) {
            Pen zero(th.textMuted, 1.0f);
            g.DrawLine(&zero, L.price.X, y, L.price.X + L.price.Width, y);
        } else {
            g.DrawLine(&grid, L.price.X, y, L.price.X + L.price.Width, y);
        }
        const std::wstring label = percent ? FormatPct(v) : FormatPrice(v);
        g.DrawString(label.c_str(), -1, &font,
                     RectF(L.axisR.X + 6.0f * s, y - labelH / 2, L.axisR.Width, labelH), &fmt, &txt);
        v += step;
    }
}

DateStyle StyleFor(const RangeSpec& r, double spanDays) {
    if (r.intraday) { return (spanDays > 1.0) ? DateStyle::DayTime : DateStyle::Time; }
    return (spanDays > 540.0) ? DateStyle::MonthYear : DateStyle::DayMonth;
}

void DrawDateAxis(Graphics& g, const Layout& L, const Series& srs, const RangeSpec& r,
                  int32_t gmtOffset, const Font& font, const Theme& th, float s, float gridBottom) {
    if (srs.count == 0) { return; }
    const float  labelW = 76.0f * s;
    const size_t nTicks = static_cast<size_t>(max(1.0f, L.axisB.Width / labelW));
    const size_t step   = max<size_t>(1, srs.count / nTicks);
    const DateStyle style = StyleFor(r, SpanDays(srs));

    Pen          grid(th.grid, 1.0f);
    SolidBrush   txt(th.textMuted);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    for (size_t i = step / 2; i < srs.count && i < kMaxPoints; i += step) {
        const float x = XFor(i, L.price, srs.count);
        g.DrawLine(&grid, x, L.price.Y, x, gridBottom);
        const std::wstring label = FormatDate(srs.pts[i].time, gmtOffset, style);
        g.DrawString(label.c_str(), -1, &font,
                     RectF(x - labelW / 2, L.axisB.Y + 3.0f * s, labelW, L.axisB.Height), &fmt, &txt);
    }
}

// ---------------------------------------------------------------------------
// Series

void DrawLineSeries(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc,
                    const Theme& th, float s) {
    assert(srs.count > 0);
    std::array<PointF, kMaxPoints> pts{};
    const size_t n = min(srs.count, kMaxPoints);
    for (size_t i = 0; i < n; ++i) {
        pts[i] = PointF(XFor(i, L.price, n), YFor(srs.pts[i].close, L.price, sc));
    }
    const float bottom = L.price.Y + L.price.Height;
    if (n >= 2) {
        GraphicsPath area;
        area.AddLines(pts.data(), static_cast<INT>(n));
        area.AddLine(pts[n - 1].X, bottom, pts[0].X, bottom);
        area.CloseFigure();
        LinearGradientBrush fill(RectF(L.price.X, L.price.Y, L.price.Width, L.price.Height + 1.0f),
                                 th.fillTop, th.fillBottom, LinearGradientModeVertical);
        g.FillPath(&fill, &area);
        Pen line(th.line, 1.75f * s);
        line.SetLineJoin(LineJoinRound);
        g.DrawLines(&line, pts.data(), static_cast<INT>(n));
    } else {
        SolidBrush dot(th.line);
        const float r = 3.0f * s;
        g.FillEllipse(&dot, pts[0].X - r, pts[0].Y - r, 2 * r, 2 * r);
    }
}

void DrawCandles(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc,
                 const Theme& th, float s) {
    assert(srs.count > 0);
    const size_t n    = min(srs.count, kMaxPoints);
    const float  slot = SlotWidth(L.price, n);
    const float  body = max(1.0f, std::floor(slot * 0.65f));
    SolidBrush up(th.up);
    SolidBrush down(th.down);
    Pen        upPen(th.up, max(1.0f, s));
    Pen        downPen(th.down, max(1.0f, s));
    for (size_t i = 0; i < n; ++i) {
        const Candle& c    = srs.pts[i];
        const bool    isUp = c.close >= c.open;
        const float   x    = XFor(i, L.price, n);
        g.DrawLine(isUp ? &upPen : &downPen, x, YFor(c.high, L.price, sc), x, YFor(c.low, L.price, sc));
        const float yTop = YFor(max(c.open, c.close), L.price, sc);
        const float yBot = YFor(min(c.open, c.close), L.price, sc);
        g.FillRectangle(isUp ? &up : &down, x - body / 2, yTop, body, max(1.0f, yBot - yTop));
    }
}

// Draws a NaN-gapped track as line segments.
void DrawTrack(Graphics& g, const RectF& area, const Track& t, size_t n, const PriceScale& sc, const Pen& pen) {
    std::array<PointF, kMaxPoints> pts{};
    size_t run = 0;
    for (size_t i = 0; i <= n && i <= kMaxPoints; ++i) {
        const bool valid = (i < n) && std::isfinite(t[i]);
        if (valid) {
            pts[run] = PointF(XFor(i, area, n), YFor(t[i], area, sc));
            ++run;
            continue;
        }
        if (run >= 2) { g.DrawLines(&pen, pts.data(), static_cast<INT>(run)); }
        run = 0;
    }
}

void DrawIndicators(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc,
                    const ChartOptions& o, const Theme& th, float s) {
    const size_t n = min(srs.count, kMaxPoints);
    if (n < 2) { return; }
    if (o.bollinger && n >= kBollPeriod) {
        Track mid;
        Track up;
        Track lo;
        Bollinger(srs, kBollPeriod, kBollK, mid, up, lo);
        // Band fill: upper edge forward, lower edge back.
        GraphicsPath band;
        std::array<PointF, kMaxPoints> edge{};
        size_t cnt = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!std::isfinite(up[i])) { continue; }
            edge[cnt++] = PointF(XFor(i, L.price, n), YFor(up[i], L.price, sc));
        }
        if (cnt >= 2) {
            band.AddLines(edge.data(), static_cast<INT>(cnt));
            cnt = 0;
            for (size_t k = n; k > 0; --k) {
                const size_t i = k - 1;
                if (!std::isfinite(lo[i])) { continue; }
                edge[cnt++] = PointF(XFor(i, L.price, n), YFor(lo[i], L.price, sc));
            }
            band.AddLines(edge.data(), static_cast<INT>(cnt));
            band.CloseFigure();
            SolidBrush fill(th.band);
            g.FillPath(&fill, &band);
        }
        Pen edgePen(th.bandEdge, 1.0f);
        DrawTrack(g, L.price, up, n, sc, edgePen);
        DrawTrack(g, L.price, lo, n, sc, edgePen);
        Pen midPen(th.bandEdge, 1.0f);
        midPen.SetDashStyle(DashStyleDot);
        DrawTrack(g, L.price, mid, n, sc, midPen);
    }
    if (o.sma20) {
        Track t;
        Sma(srs, kSmaShort, t);
        Pen pen(th.sma20, 1.5f * s);
        DrawTrack(g, L.price, t, n, sc, pen);
    }
    if (o.sma50) {
        Track t;
        Sma(srs, kSmaLong, t);
        Pen pen(th.sma50, 1.5f * s);
        DrawTrack(g, L.price, t, n, sc, pen);
    }
}

void DrawIndicatorLegend(Graphics& g, const Layout& L, const ChartInput& in, const Font& font,
                         const Theme& th, float s) {
    const ChartOptions& o = in.opts;
    struct Item { bool on; const wchar_t* label; const Color* col; };
    const Item items[] = {
        { o.benchmark && in.bench != nullptr, in.benchLabel != nullptr ? in.benchLabel : L"benchmark", &th.textMuted },
        { o.sma20,     L"SMA 20",     &th.sma20 },
        { o.sma50,     L"SMA 50",     &th.sma50 },
        { o.bollinger, L"Bollinger 20/2", &th.bandEdge },
    };
    float x = L.price.X + L.price.Width - 8.0f * s;
    const float y = L.price.Y + 6.0f * s;
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentFar);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    for (const Item& it : items) {
        if (!it.on) { continue; }
        SolidBrush b(*it.col);
        RectF bound;
        g.MeasureString(it.label, -1, &font, PointF(0, 0), &bound);
        g.DrawString(it.label, -1, &font, RectF(x - bound.Width - 2.0f, y, bound.Width + 2.0f, 16.0f * s), &fmt, &b);
        x -= bound.Width + 14.0f * s;
    }
}

void DrawVolume(Graphics& g, const Layout& L, const Series& srs, const Theme& th) {
    assert(srs.count > 0);
    const size_t n = min(srs.count, kMaxPoints);
    double maxVol = 0.0;
    for (size_t i = 0; i < n; ++i) { maxVol = max(maxVol, srs.pts[i].volume); }
    if (maxVol <= 0.0) { return; }
    const float slot = SlotWidth(L.volume, n);
    const float bw   = max(1.0f, std::floor(slot * 0.7f));
    SolidBrush up(th.volUp);
    SolidBrush down(th.volDown);
    SolidBrush flat(th.volFlat);
    for (size_t i = 0; i < n; ++i) {
        const Candle& c = srs.pts[i];
        const float h = static_cast<float>(c.volume / maxVol) * L.volume.Height;
        if (h <= 0.0f) { continue; }
        const float x = XFor(i, L.volume, n);
        const Brush* b = &flat;
        if (c.close > c.open)      { b = &up; }
        else if (c.close < c.open) { b = &down; }
        g.FillRectangle(b, x - bw / 2, L.volume.Y + L.volume.Height - h, bw, h);
    }
}

void DrawRsi(Graphics& g, const Layout& L, const Series& srs, const Font& font, const Theme& th, float s) {
    const size_t n = min(srs.count, kMaxPoints);
    Pen grid(th.grid, 1.0f);
    g.DrawRectangle(&grid, L.rsi);
    const PriceScale sc{ 0.0, 100.0 };
    Pen band(th.textMuted, 1.0f);
    band.SetDashStyle(DashStyleDash);
    SolidBrush txt(th.textMuted);
    StringFormat fmt;
    fmt.SetLineAlignment(StringAlignmentCenter);
    for (double level : { 30.0, 70.0 }) {
        const float y = YFor(level, L.rsi, sc);
        g.DrawLine(&band, L.rsi.X, y, L.rsi.X + L.rsi.Width, y);
        g.DrawString(FormatPrice(level).c_str(), -1, &font,
                     RectF(L.axisR.X + 6.0f * s, y - 8.0f * s, L.axisR.Width, 16.0f * s), &fmt, &txt);
    }
    g.DrawString(L"RSI 14", -1, &font, PointF(L.rsi.X + 6.0f * s, L.rsi.Y + 2.0f * s), &txt);
    if (n <= kRsiPeriod) { return; }
    Track t;
    Rsi(srs, kRsiPeriod, t);
    Pen pen(th.rsi, 1.5f * s);
    DrawTrack(g, L.rsi, t, n, sc, pen);
}

// Index of the bar a dividend ex-date falls on (the last bar at or before
// it), or kMaxPoints when it is outside the series.
size_t BarForTime(const Series& s, int64_t t) {
    const size_t n = min(s.count, kMaxPoints);
    if (n == 0 || t < s.pts[0].time) { return kMaxPoints; }
    const auto end = s.pts.begin() + static_cast<std::ptrdiff_t>(n);
    const auto it  = std::upper_bound(s.pts.begin(), end, t,
                                      [](int64_t v, const Candle& c) { return v < c.time; });
    return static_cast<size_t>((it - 1) - s.pts.begin());
}

// Sum of the dividends that land on bar `idx` (0 when none).
double DividendOnBar(const QuoteData& d, size_t idx) {
    double total = 0.0;
    for (size_t k = 0; k < d.dividendCount && k < kMaxDividends; ++k) {
        if (BarForTime(d.series, d.dividends[k].time) == idx) { total += d.dividends[k].amount; }
    }
    return total;
}

// Benchmark close on or before the stock's bar `i`, rebased so the two
// start at the same price. NaN when the benchmark has no bar that early.
double BenchRebased(const ChartInput& in, size_t i) {
    const QuoteData* b = in.bench;
    if (b == nullptr || !b->valid || in.data == nullptr) { return std::numeric_limits<double>::quiet_NaN(); }
    const Series& s  = in.data->series;
    const Series& bs = b->series;
    if (s.count < 2 || bs.count < 2) { return std::numeric_limits<double>::quiet_NaN(); }
    const size_t j0 = BarForTime(bs, s.pts[0].time);
    const size_t j  = BarForTime(bs, s.pts[i].time);
    if (j0 >= kMaxPoints || j >= kMaxPoints) { return std::numeric_limits<double>::quiet_NaN(); }
    const double base = bs.pts[j0].close;
    if (base <= 0.0) { return std::numeric_limits<double>::quiet_NaN(); }
    return s.pts[0].close * bs.pts[j].close / base;
}

void DrawBenchmark(Graphics& g, const Layout& L, const ChartInput& in, const PriceScale& sc,
                   const Theme& th, float s) {
    const size_t n = min(in.data->series.count, kMaxPoints);
    Track t;
    for (size_t i = 0; i < kMaxPoints; ++i) { t[i] = (i < n) ? BenchRebased(in, i) : std::numeric_limits<double>::quiet_NaN(); }
    Pen pen(th.textMuted, 1.5f * s);
    pen.SetDashStyle(DashStyleDash);
    DrawTrack(g, L.price, t, n, sc, pen);
}

// Small markers along the bottom of the price plot on ex-dividend bars.
void DrawDividendMarkers(Graphics& g, const Layout& L, const QuoteData& d, const Font& font,
                         const Theme& th, float s) {
    const size_t n = min(d.series.count, kMaxPoints);
    if (n == 0 || d.dividendCount == 0) { return; }
    SolidBrush   fill(th.up);
    SolidBrush   txt(th.tipText);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    const float r = 7.0f * s;
    const float y = L.price.Y + L.price.Height - r - 2.0f * s;
    for (size_t k = 0; k < d.dividendCount && k < kMaxDividends; ++k) {
        const size_t idx = BarForTime(d.series, d.dividends[k].time);
        if (idx >= n) { continue; }
        const float x = XFor(idx, L.price, n);
        g.FillEllipse(&fill, x - r, y - r, 2 * r, 2 * r);
        g.DrawString(L"D", 1, &font, RectF(x - r, y - r, 2 * r, 2 * r), &fmt, &txt);
    }
}

void DrawLastPriceTag(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc,
                      const Font& font, const Theme& th, float s) {
    assert(srs.count > 0);
    const Candle& last = srs.pts[srs.count - 1];
    const bool   isUp  = (srs.count < 2) || last.close >= srs.pts[srs.count - 2].close;
    const Color& col   = isUp ? th.up : th.down;
    const float  y     = YFor(last.close, L.price, sc);
    Pen dash(col, 1.0f);
    dash.SetDashStyle(DashStyleDash);
    g.DrawLine(&dash, L.price.X, y, L.price.X + L.price.Width, y);

    const std::wstring label = FormatPrice(last.close);
    const float tagH = 18.0f * s;
    const RectF tag(L.axisR.X + 2.0f * s, y - tagH / 2, L.axisR.Width - 4.0f * s, tagH);
    SolidBrush   bg(col);
    SolidBrush   txt(th.tipText);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.FillRectangle(&bg, tag);
    g.DrawString(label.c_str(), -1, &font, tag, &fmt, &txt);
}

void DrawTooltip(Graphics& g, const Layout& L, float anchorX, const std::wstring& tip,
                 const Font& font, const Theme& th, float s) {
    RectF bound;
    g.MeasureString(tip.c_str(), -1, &font, PointF(0, 0), &bound);
    const float pad = 8.0f * s;
    float bx = anchorX + 14.0f * s;
    const float by = L.price.Y + 8.0f * s;
    const float bw = bound.Width + 2 * pad;
    const float bh = bound.Height + 2 * pad;
    if (bx + bw > L.price.X + L.price.Width) { bx = anchorX - 14.0f * s - bw; }
    bx = max(bx, L.price.X);
    SolidBrush bg(th.tipBg);
    SolidBrush txt(th.tipText);
    g.FillRectangle(&bg, bx, by, bw, bh);
    g.DrawString(tip.c_str(), -1, &font, PointF(bx + pad, by + pad), &txt);
}

void DrawHover(Graphics& g, const Layout& L, const ChartInput& in, const PriceScale& sc,
               const Font& font, const Theme& th, float s, float crossBottom) {
    assert(in.data != nullptr && in.range != nullptr);
    const Series& srs = in.data->series;
    const size_t  n   = min(srs.count, kMaxPoints);
    if (n == 0 || in.hoverX < 0) { return; }
    const float fx = static_cast<float>(in.hoverX);
    if (fx < L.price.X || fx > L.price.X + L.price.Width) { return; }

    const float slot = SlotWidth(L.price, n);
    size_t idx = static_cast<size_t>((fx - L.price.X) / slot);
    idx = min(idx, n - 1);
    const Candle& c = srs.pts[idx];
    const float x = XFor(idx, L.price, n);
    const float y = YFor(c.close, L.price, sc);

    Pen cross(th.hover, 1.0f);
    cross.SetDashStyle(DashStyleDot);
    g.DrawLine(&cross, x, L.price.Y, x, crossBottom);
    g.DrawLine(&cross, L.price.X, y, L.price.X + L.price.Width, y);
    SolidBrush dot(th.line);
    const float r = 4.0f * s;
    g.FillEllipse(&dot, x - r, y - r, 2 * r, 2 * r);

    const DateStyle ds = in.range->intraday ? DateStyle::FullTime : DateStyle::Full;
    std::wstring tip = FormatDate(c.time, in.data->meta.gmtOffsetSec, ds);
    tip += L"\nO " + FormatPrice(c.open) + L"   H " + FormatPrice(c.high);
    tip += L"\nL " + FormatPrice(c.low)  + L"   C " + FormatPrice(c.close);
    tip += L"\nVol " + FormatVolume(c.volume);
    const double div = DividendOnBar(*in.data, idx);
    if (div > 0.0) { tip += L"\nDividend " + FormatPrice(div) + L" (ex-date)"; }
    if (in.opts.benchmark && in.bench != nullptr) {
        const double b = BenchRebased(in, idx);
        const double first = srs.pts[0].close;
        if (std::isfinite(b) && first > 0.0) {
            tip += L"\n" + std::wstring(in.benchLabel != nullptr ? in.benchLabel : L"Benchmark") + L" " +
                   FormatPct((b - first) / first * 100.0) + L" vs " + FormatPct((c.close - first) / first * 100.0);
        }
    }
    if (in.opts.rsi && n > kRsiPeriod) {
        Track t;
        Rsi(srs, kRsiPeriod, t);
        if (std::isfinite(t[idx])) { tip += L"   RSI " + FormatPrice(t[idx]); }
    }
    DrawTooltip(g, L, x, tip, font, th, s);
}

void DrawMessage(Graphics& g, const RectF& rc, const std::wstring& text, const Font& font, const Theme& th) {
    SolidBrush   txt(th.textMuted);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &font, rc, &fmt, &txt);
}

// ---------------------------------------------------------------------------
// Trend inset (draggable box inside the price area)

// Geometry shared by drawing, hit-testing and drag mapping. `margin` is the
// gap kept between the box and the plot edge.
struct InsetGeom {
    RectF box;
    float margin = 0.0f;
    float freeW  = 0.0f;   // how far the box can travel horizontally
    float freeH  = 0.0f;
};

bool InsetGeometry(const Layout& L, const ChartInput& in, float s, InsetGeom& out) {
    const float w = min(190.0f * s, L.price.Width * 0.32f);
    const float h = 70.0f * s;
    if (w < 60.0f * s || L.price.Height < 3.0f * h) { return false; }
    out.margin = 8.0f * s;
    out.freeW  = max(L.price.Width - w - 2.0f * out.margin, 0.0f);
    out.freeH  = max(L.price.Height - h - 2.0f * out.margin, 0.0f);
    const float fx = min(max(in.insetX, 0.0f), 1.0f);
    const float fy = min(max(in.insetY, 0.0f), 1.0f);
    out.box = RectF(L.price.X + out.margin + fx * out.freeW, L.price.Y + out.margin + fy * out.freeH, w, h);
    assert(out.box.X >= L.price.X && out.box.Y >= L.price.Y);
    return true;
}

void DrawInset(Graphics& g, const Layout& L, const ChartInput& in, const Font& font, const Theme& th, float s) {
    InsetGeom geom;
    if (!InsetGeometry(L, in, s, geom)) { return; }
    const RectF& box = geom.box;
    SolidBrush bg(th.insetBg);
    Pen        border(th.insetBorder, 1.0f);
    g.FillRectangle(&bg, box);
    g.DrawRectangle(&border, box);

    SolidBrush muted(th.textMuted);
    std::wstring title = std::wstring(in.insetLabel != nullptr ? in.insetLabel : L"") + L" trend";
    g.DrawString(title.c_str(), -1, &font, PointF(box.X + 6.0f * s, box.Y + 3.0f * s), &muted);

    const QuoteData* d = in.inset;
    if (d == nullptr || !d->valid || d->series.count < 2) {
        SolidBrush txt(th.textMuted);
        g.DrawString(d == nullptr ? L"…" : L"no data", -1, &font,
                     PointF(box.X + 6.0f * s, box.Y + 20.0f * s), &txt);
        return;
    }
    const Series& srs = d->series;
    const size_t n = min(srs.count, kMaxPoints);
    const double first = srs.pts[0].close;
    const double last  = srs.pts[n - 1].close;
    const double pct   = (first > 0.0) ? (last - first) / first * 100.0 : 0.0;
    const Color& col   = (last >= first) ? th.up : th.down;

    SolidBrush pctBrush(col);
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    g.DrawString(FormatPct(pct).c_str(), -1, &font,
                 RectF(box.X, box.Y + 3.0f * s, box.Width - 6.0f * s, 16.0f * s), &right, &pctBrush);

    const RectF plot(box.X + 6.0f * s, box.Y + 22.0f * s, box.Width - 12.0f * s, box.Height - 28.0f * s);
    PriceScale sc{ first, first };
    for (size_t i = 0; i < n; ++i) { Expand(sc, srs.pts[i].close); }
    if (sc.hi <= sc.lo) { sc.hi = sc.lo + kMinPriceRange; }
    std::array<PointF, kMaxPoints> pts{};
    for (size_t i = 0; i < n; ++i) {
        pts[i] = PointF(plot.X + static_cast<float>(i) / static_cast<float>(n - 1) * plot.Width,
                        YFor(srs.pts[i].close, plot, sc));
    }
    GraphicsPath area;
    area.AddLines(pts.data(), static_cast<INT>(n));
    area.AddLine(pts[n - 1].X, plot.Y + plot.Height, pts[0].X, plot.Y + plot.Height);
    area.CloseFigure();
    SolidBrush fill(Color(60, col.GetR(), col.GetG(), col.GetB()));
    g.FillPath(&fill, &area);
    Pen line(col, 1.25f * s);
    g.DrawLines(&line, pts.data(), static_cast<INT>(n));
}

// ---------------------------------------------------------------------------
// Compare overlay: every entry as % change from its first bar, on a time axis.

struct CompareRange {
    int64_t t0 = 0;
    int64_t t1 = 0;
    bool    ok = false;
};

CompareRange TimeRange(const ChartInput& in) {
    CompareRange r;
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        if (d == nullptr || !d->valid || d->series.count < 2) { continue; }
        const int64_t a = d->series.pts[0].time;
        const int64_t b = d->series.pts[d->series.count - 1].time;
        if (!r.ok) { r.t0 = a; r.t1 = b; r.ok = true; }
        r.t0 = min(r.t0, a);
        r.t1 = max(r.t1, b);
    }
    if (r.ok && r.t1 <= r.t0) { r.ok = false; }
    return r;
}

float XForTime(int64_t t, const RectF& r, const CompareRange& cr) {
    assert(cr.t1 > cr.t0);
    const double f = static_cast<double>(t - cr.t0) / static_cast<double>(cr.t1 - cr.t0);
    return r.X + static_cast<float>(f) * r.Width;
}

double PctAt(const Series& s, size_t i) {
    const double base = s.pts[0].close;
    return (base > 0.0) ? (s.pts[i].close - base) / base * 100.0 : 0.0;
}

// Index of the last bar at or before `t` (0 if none).
size_t IndexAtOrBefore(const Series& s, int64_t t) {
    const auto end = s.pts.begin() + static_cast<std::ptrdiff_t>(min(s.count, kMaxPoints));
    const auto it  = std::upper_bound(s.pts.begin(), end, t,
                                      [](int64_t v, const Candle& c) { return v < c.time; });
    if (it == s.pts.begin()) { return 0; }
    return static_cast<size_t>((it - 1) - s.pts.begin());
}

// Everything the compare base and its hover overlay share.
struct CompareCtx {
    Layout           L;
    CompareRange     cr;
    PriceScale       sc;
    const QuoteData* longest = nullptr;   // series used for the date axis
};

bool CompareContext(const RectF& rc, const ChartInput& in, float s, CompareCtx& out) {
    out.L  = MakeLayout(rc, s, false, false);
    out.cr = TimeRange(in);
    if (!out.cr.ok) { return false; }
    PriceScale sc{ 0.0, 0.0 };
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        if (d == nullptr || !d->valid || d->series.count < 2) { continue; }
        for (size_t i = 0; i < d->series.count; ++i) { Expand(sc, PctAt(d->series, i)); }
        if (out.longest == nullptr || d->series.count > out.longest->series.count) { out.longest = d; }
    }
    double pad = (sc.hi - sc.lo) * 0.06;
    if (pad < 0.5) { pad = 0.5; }
    sc.lo -= pad;
    sc.hi += pad;
    out.sc = sc;
    assert(out.sc.hi > out.sc.lo);
    return true;
}

void DrawCompareBase(Graphics& g, const RectF& rc, const ChartInput& in, const Font& axisFont,
                     const Font& body, const Theme& th, float s) {
    CompareCtx c;
    if (!CompareContext(rc, in, s, c)) {
        DrawMessage(g, rc, L"Loading…", body, th);
        return;
    }
    const Layout& L = c.L;
    const CompareRange& cr = c.cr;
    const PriceScale& sc = c.sc;
    const QuoteData* longest = c.longest;
    DrawPriceGrid(g, L, sc, axisFont, th, s, true);

    // Date ticks from the longest series.
    if (longest != nullptr && in.range != nullptr) {
        const Series& srs = longest->series;
        const DateStyle style = StyleFor(*in.range, SpanDays(srs));
        const float  labelW = 76.0f * s;
        const size_t nTicks = static_cast<size_t>(max(1.0f, L.axisB.Width / labelW));
        const size_t step   = max<size_t>(1, srs.count / nTicks);
        Pen grid(th.grid, 1.0f);
        SolidBrush txt(th.textMuted);
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        for (size_t i = step / 2; i < srs.count && i < kMaxPoints; i += step) {
            const float x = XForTime(srs.pts[i].time, L.price, cr);
            g.DrawLine(&grid, x, L.price.Y, x, L.price.Y + L.price.Height);
            const std::wstring label = FormatDate(srs.pts[i].time, longest->meta.gmtOffsetSec, style);
            g.DrawString(label.c_str(), -1, &axisFont,
                         RectF(x - labelW / 2, L.axisB.Y + 3.0f * s, labelW, L.axisB.Height), &fmt, &txt);
        }
    }

    g.SetClip(L.price);
    std::array<PointF, kMaxPoints> pts{};
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        if (d == nullptr || !d->valid || d->series.count < 2) { continue; }
        const size_t n = min(d->series.count, kMaxPoints);
        for (size_t i = 0; i < n; ++i) {
            pts[i] = PointF(XForTime(d->series.pts[i].time, L.price, cr), YFor(PctAt(d->series, i), L.price, sc));
        }
        Pen pen(SeriesColor(k, th.dark), 1.6f * s);
        pen.SetLineJoin(LineJoinRound);
        g.DrawLines(&pen, pts.data(), static_cast<INT>(n));
    }
    g.ResetClip();

    // Legend.
    float ly = L.price.Y + 6.0f * s;
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        const Color& col = SeriesColor(k, th.dark);
        SolidBrush swatch(col);
        g.FillRectangle(&swatch, L.price.X + 8.0f * s, ly + 4.0f * s, 10.0f * s, 10.0f * s);
        std::wstring label = in.entries[k].symbol != nullptr ? in.entries[k].symbol : L"?";
        if (d != nullptr && d->valid && d->series.count >= 2) {
            label += L"  " + FormatPct(PctAt(d->series, d->series.count - 1));
        } else if (d != nullptr && !d->valid) {
            label += L"  (error)";
        } else {
            label += L"  …";
        }
        SolidBrush txt(th.text);
        g.DrawString(label.c_str(), -1, &axisFont, PointF(L.price.X + 22.0f * s, ly), &txt);
        ly += 16.0f * s;
    }
}

// Hover: nearest bar at or before the hovered time in each series.
void DrawCompareOverlay(Graphics& g, const RectF& rc, const ChartInput& in, const Font& axisFont,
                        const Theme& th, float s) {
    if (in.hoverX < 0) { return; }
    CompareCtx c;
    if (!CompareContext(rc, in, s, c)) { return; }
    const Layout& L = c.L;
    const CompareRange& cr = c.cr;
    const QuoteData* longest = c.longest;
    const float fx = static_cast<float>(in.hoverX);
    if (fx < L.price.X || fx > L.price.X + L.price.Width) { return; }
    const double f = (fx - L.price.X) / L.price.Width;
    const int64_t t = cr.t0 + static_cast<int64_t>(f * static_cast<double>(cr.t1 - cr.t0));
    Pen cross(th.hover, 1.0f);
    cross.SetDashStyle(DashStyleDot);
    g.DrawLine(&cross, fx, L.price.Y, fx, L.price.Y + L.price.Height);
    std::wstring tip;
    if (longest != nullptr && in.range != nullptr) {
        tip = FormatDate(t, longest->meta.gmtOffsetSec, in.range->intraday ? DateStyle::FullTime : DateStyle::Full);
    }
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        if (d == nullptr || !d->valid || d->series.count < 2) { continue; }
        const size_t i = IndexAtOrBefore(d->series, t);
        tip += L"\n" + std::wstring(in.entries[k].symbol) + L"  " + FormatPct(PctAt(d->series, i)) +
               L"  (" + FormatPrice(d->series.pts[i].close) + L")";
    }
    DrawTooltip(g, L, fx, tip, axisFont, th, s);
}

} // namespace

REAL FontPx(float pt, float scale) {
    assert(pt > 0.0f && scale > 0.0f);
    return pt * scale * (96.0f / 72.0f);
}

bool ChartInsetRect(const RectF& rc, const ChartInput& in, float scale, RectF& out) {
    assert(scale > 0.0f);
    if (!in.opts.inset || in.compare || rc.Width < 40.0f || rc.Height < 40.0f) { return false; }
    const Layout L = MakeLayout(rc, scale, true, in.opts.rsi);
    InsetGeom geom;
    if (!InsetGeometry(L, in, scale, geom)) { return false; }
    out = geom.box;
    return true;
}

void ChartInsetFractionFor(const RectF& rc, const ChartInput& in, float scale,
                           float px, float py, float& fx, float& fy) {
    assert(scale > 0.0f);
    fx = in.insetX;
    fy = in.insetY;
    if (rc.Width < 40.0f || rc.Height < 40.0f) { return; }
    const Layout L = MakeLayout(rc, scale, true, in.opts.rsi);
    InsetGeom geom;
    if (!InsetGeometry(L, in, scale, geom)) { return; }
    fx = (geom.freeW > 0.0f) ? (px - L.price.X - geom.margin) / geom.freeW : 0.0f;
    fy = (geom.freeH > 0.0f) ? (py - L.price.Y - geom.margin) / geom.freeH : 0.0f;
    fx = min(max(fx, 0.0f), 1.0f);
    fy = min(max(fy, 0.0f), 1.0f);
}

namespace {

// Shared by the normal chart's base and overlay. `ok` is false when there is
// nothing to plot (loading / error / empty), in which case `message` says why.
struct NormalCtx {
    Layout       L;
    PriceScale   sc;
    float        gridBottom = 0.0f;
    bool         ok = false;
    std::wstring message;
};

NormalCtx NormalContext(const RectF& rc, const ChartInput& in, float s) {
    NormalCtx c;
    if (in.data == nullptr) {
        c.message = L"Loading…";
        return c;
    }
    if (!in.data->valid) {
        c.message = in.data->error.empty() ? L"No data" : in.data->error;
        return c;
    }
    if (!ComputeScale(in.data->series, in.opts, c.sc)) {
        c.message = L"No trades in this range";
        return c;
    }
    if (in.opts.benchmark && in.bench != nullptr) {
        // Widen the scale so a rebased benchmark that outruns the stock stays visible.
        PriceScale raw{ c.sc.lo, c.sc.hi };
        const size_t n = min(in.data->series.count, kMaxPoints);
        for (size_t i = 0; i < n; ++i) { Expand(raw, BenchRebased(in, i)); }
        c.sc = raw;
    }
    c.L = MakeLayout(rc, s, true, in.opts.rsi);
    c.gridBottom = in.opts.rsi ? c.L.rsi.Y + c.L.rsi.Height : c.L.volume.Y + c.L.volume.Height;
    c.ok = true;
    return c;
}

} // namespace

void DrawChartBase(Graphics& g, const RectF& rc, const ChartInput& in, float scale) {
    assert(scale > 0.0f);
    assert(in.range != nullptr);
    assert(in.theme != nullptr);
    if (rc.Width < 40.0f || rc.Height < 40.0f) { return; }
    const Theme& th = *in.theme;
    const Font axisFont = MakeFont(8.5f, scale);
    const Font body     = MakeFont(11.0f, scale);

    if (in.compare) {
        DrawCompareBase(g, rc, in, axisFont, body, th, scale);
        return;
    }
    const NormalCtx c = NormalContext(rc, in, scale);
    if (!c.ok) {
        DrawMessage(g, rc, c.message, body, th);
        return;
    }
    const Series& srs = in.data->series;
    const Layout& L = c.L;
    DrawPriceGrid(g, L, c.sc, axisFont, th, scale, false);
    DrawDateAxis(g, L, srs, *in.range, in.data->meta.gmtOffsetSec, axisFont, th, scale, c.gridBottom);

    g.SetClip(RectF(L.price.X, L.price.Y - 2.0f, L.price.Width, L.price.Height + 4.0f));
    if (in.opts.candles) { DrawCandles(g, L, srs, c.sc, th, scale); }
    else                 { DrawLineSeries(g, L, srs, c.sc, th, scale); }
    DrawIndicators(g, L, srs, c.sc, in.opts, th, scale);
    if (in.opts.benchmark && in.bench != nullptr) { DrawBenchmark(g, L, in, c.sc, th, scale); }
    g.ResetClip();

    DrawVolume(g, L, srs, th);
    if (in.opts.rsi) { DrawRsi(g, L, srs, axisFont, th, scale); }
    DrawDividendMarkers(g, L, *in.data, axisFont, th, scale);
    DrawLastPriceTag(g, L, srs, c.sc, axisFont, th, scale);
    DrawIndicatorLegend(g, L, in, axisFont, th, scale);
    if (in.opts.inset) { DrawInset(g, L, in, axisFont, th, scale); }
}

void DrawChartOverlay(Graphics& g, const RectF& rc, const ChartInput& in, float scale) {
    assert(scale > 0.0f);
    assert(in.range != nullptr);
    assert(in.theme != nullptr);
    if (in.hoverX < 0 || rc.Width < 40.0f || rc.Height < 40.0f) { return; }
    const Theme& th = *in.theme;
    const Font axisFont = MakeFont(8.5f, scale);
    if (in.compare) {
        DrawCompareOverlay(g, rc, in, axisFont, th, scale);
        return;
    }
    const NormalCtx c = NormalContext(rc, in, scale);
    if (!c.ok) { return; }
    DrawHover(g, c.L, in, c.sc, axisFont, th, scale, c.gridBottom);
}

void DrawChart(Graphics& g, const RectF& rc, const ChartInput& in, float scale) {
    DrawChartBase(g, rc, in, scale);
    DrawChartOverlay(g, rc, in, scale);
}

} // namespace st
