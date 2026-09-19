#include "chart.h"

#include "indicators.h"
#include "textfmt.h"

#include <cassert>
#include <cmath>

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

void DrawIndicatorLegend(Graphics& g, const Layout& L, const ChartOptions& o, const Font& font,
                         const Theme& th, float s) {
    struct Item { bool on; const wchar_t* label; const Color* col; };
    const Item items[] = {
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
// Trend inset (top-left corner of the price area)

void DrawInset(Graphics& g, const Layout& L, const ChartInput& in, const Font& font, const Theme& th, float s) {
    const float w = min(190.0f * s, L.price.Width * 0.32f);
    const float h = 70.0f * s;
    if (w < 60.0f * s || L.price.Height < 3.0f * h) { return; }
    const RectF box(L.price.X + 8.0f * s, L.price.Y + 8.0f * s, w, h);
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

void DrawCompare(Graphics& g, const RectF& rc, const ChartInput& in, const Font& axisFont,
                 const Font& body, const Theme& th, float s) {
    const Layout L = MakeLayout(rc, s, false, false);
    const CompareRange cr = TimeRange(in);
    if (!cr.ok) {
        DrawMessage(g, rc, L"Loading…", body, th);
        return;
    }
    PriceScale sc{ 0.0, 0.0 };
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        if (d == nullptr || !d->valid || d->series.count < 2) { continue; }
        for (size_t i = 0; i < d->series.count; ++i) { Expand(sc, PctAt(d->series, i)); }
    }
    double pad = (sc.hi - sc.lo) * 0.06;
    if (pad < 0.5) { pad = 0.5; }
    sc.lo -= pad;
    sc.hi += pad;
    DrawPriceGrid(g, L, sc, axisFont, th, s, true);

    // Date ticks from the longest series.
    const QuoteData* longest = nullptr;
    for (size_t k = 0; k < in.entryCount && k < kMaxStocks; ++k) {
        const QuoteData* d = in.entries[k].data;
        if (d != nullptr && d->valid && (longest == nullptr || d->series.count > longest->series.count)) { longest = d; }
    }
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

    // Hover: nearest bar at or before the hovered time in each series.
    if (in.hoverX < 0) { return; }
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

void DrawChart(Graphics& g, const RectF& rc, const ChartInput& in, float scale) {
    assert(scale > 0.0f);
    assert(in.range != nullptr);
    assert(in.theme != nullptr);
    if (rc.Width < 40.0f || rc.Height < 40.0f) { return; }
    const Theme& th = *in.theme;
    const Font axisFont = MakeFont(8.5f, scale);
    const Font body     = MakeFont(11.0f, scale);

    if (in.compare) {
        DrawCompare(g, rc, in, axisFont, body, th, scale);
        return;
    }
    if (in.data == nullptr) {
        DrawMessage(g, rc, L"Loading…", body, th);
        return;
    }
    if (!in.data->valid) {
        DrawMessage(g, rc, in.data->error.empty() ? L"No data" : in.data->error, body, th);
        return;
    }
    const Series& srs = in.data->series;
    PriceScale sc;
    if (!ComputeScale(srs, in.opts, sc)) {
        DrawMessage(g, rc, L"No trades in this range", body, th);
        return;
    }

    const Layout L = MakeLayout(rc, scale, true, in.opts.rsi);
    const float gridBottom = in.opts.rsi ? L.rsi.Y + L.rsi.Height : L.volume.Y + L.volume.Height;
    DrawPriceGrid(g, L, sc, axisFont, th, scale, false);
    DrawDateAxis(g, L, srs, *in.range, in.data->meta.gmtOffsetSec, axisFont, th, scale, gridBottom);

    g.SetClip(RectF(L.price.X, L.price.Y - 2.0f, L.price.Width, L.price.Height + 4.0f));
    if (in.opts.candles) { DrawCandles(g, L, srs, sc, th, scale); }
    else                 { DrawLineSeries(g, L, srs, sc, th, scale); }
    DrawIndicators(g, L, srs, sc, in.opts, th, scale);
    g.ResetClip();

    DrawVolume(g, L, srs, th);
    if (in.opts.rsi) { DrawRsi(g, L, srs, axisFont, th, scale); }
    DrawLastPriceTag(g, L, srs, sc, axisFont, th, scale);
    DrawIndicatorLegend(g, L, in.opts, axisFont, th, scale);
    if (in.opts.inset) { DrawInset(g, L, in, axisFont, th, scale); }
    DrawHover(g, L, in, sc, axisFont, th, scale, gridBottom);
}

} // namespace st
