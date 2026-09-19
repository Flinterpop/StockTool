#include "chart.h"

#include "textfmt.h"

#include <cassert>
#include <cmath>

namespace st {
namespace {

using namespace Gdiplus;

const Color kGrid(255, 232, 234, 237);
const Color kAxisText(255, 95, 99, 104);
const Color kLine(255, 26, 115, 232);
const Color kFillTop(70, 26, 115, 232);
const Color kFillBottom(0, 26, 115, 232);
const Color kUp(255, 30, 142, 62);
const Color kDown(255, 217, 48, 37);
const Color kVolume(255, 196, 206, 220);
const Color kHover(255, 60, 64, 67);
const Color kTipBg(235, 32, 33, 36);
const Color kTipText(255, 255, 255, 255);
const Color kMessage(255, 120, 124, 130);

constexpr float kMinPriceRange = 0.01f;

struct Layout {
    RectF price;   // main price plot
    RectF volume;  // volume bars
    RectF axisR;   // right-hand price labels
    RectF axisB;   // bottom date labels
};

struct PriceScale {
    double lo = 0.0;
    double hi = 1.0;
};

Layout MakeLayout(const RectF& rc, float s) {
    assert(s > 0.0f);
    const float axisW = 64.0f * s;
    const float axisH = 20.0f * s;
    const float gap   = 6.0f * s;
    const float plotW = max(rc.Width - axisW, 10.0f);
    const float plotH = max(rc.Height - axisH, 10.0f);
    const float volH  = std::floor(plotH * 0.22f);
    Layout l;
    l.price  = RectF(rc.X, rc.Y, plotW, plotH - volH - gap);
    l.volume = RectF(rc.X, rc.Y + plotH - volH, plotW, volH);
    l.axisR  = RectF(rc.X + plotW, rc.Y, axisW, plotH);
    l.axisB  = RectF(rc.X, rc.Y + plotH, plotW, axisH);
    return l;
}

bool ComputeScale(const Series& s, bool candles, PriceScale& out) {
    if (s.count == 0) { return false; }
    double lo = s.pts[0].close;
    double hi = s.pts[0].close;
    for (size_t i = 0; i < s.count && i < kMaxPoints; ++i) {
        const Candle& c = s.pts[i];
        lo = min(lo, candles ? c.low : c.close);
        hi = max(hi, candles ? c.high : c.close);
    }
    if (!std::isfinite(lo) || !std::isfinite(hi)) { return false; }
    double pad = (hi - lo) * 0.06;
    if (pad < kMinPriceRange) { pad = max(std::fabs(hi) * 0.01, static_cast<double>(kMinPriceRange)); }
    out.lo = lo - pad;
    out.hi = hi + pad;
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

void DrawPriceGrid(Graphics& g, const Layout& L, const PriceScale& sc, const Font& font, float s) {
    const double step  = NiceStep((sc.hi - sc.lo) / 5.0);
    const double first = std::ceil(sc.lo / step) * step;
    Pen        grid(kGrid, 1.0f);
    SolidBrush txt(kAxisText);
    StringFormat fmt;
    fmt.SetLineAlignment(StringAlignmentCenter);
    const float labelH = 16.0f * s;
    double v = first;
    for (int i = 0; i < 64 && v <= sc.hi; ++i) {
        const float y = YFor(v, L.price, sc);
        g.DrawLine(&grid, L.price.X, y, L.price.X + L.price.Width, y);
        const std::wstring label = FormatPrice(v);
        g.DrawString(label.c_str(), -1, &font,
                     RectF(L.axisR.X + 6.0f * s, y - labelH / 2, L.axisR.Width, labelH), &fmt, &txt);
        v += step;
    }
}

void DrawDateAxis(Graphics& g, const Layout& L, const Series& srs, const RangeSpec& r,
                  const QuoteMeta& m, const Font& font, float s) {
    if (srs.count == 0) { return; }
    const float  labelW = 76.0f * s;
    const size_t nTicks = static_cast<size_t>(max(1.0f, L.axisB.Width / labelW));
    const size_t step   = max<size_t>(1, srs.count / nTicks);
    DateStyle style = DateStyle::DayMonth;
    if (r.intraday)             { style = (SpanDays(srs) > 1.0) ? DateStyle::DayTime : DateStyle::Time; }
    else if (SpanDays(srs) > 540.0) { style = DateStyle::MonthYear; }

    Pen          grid(kGrid, 1.0f);
    SolidBrush   txt(kAxisText);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    const float bottom = L.volume.Y + L.volume.Height;
    for (size_t i = step / 2; i < srs.count && i < kMaxPoints; i += step) {
        const float x = XFor(i, L.price, srs.count);
        g.DrawLine(&grid, x, L.price.Y, x, bottom);
        const std::wstring label = FormatDate(srs.pts[i].time, m.gmtOffsetSec, style);
        g.DrawString(label.c_str(), -1, &font,
                     RectF(x - labelW / 2, L.axisB.Y + 3.0f * s, labelW, L.axisB.Height), &fmt, &txt);
    }
}

void DrawLineSeries(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc, float s) {
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
                                 kFillTop, kFillBottom, LinearGradientModeVertical);
        g.FillPath(&fill, &area);
        Pen line(kLine, 1.75f * s);
        line.SetLineJoin(LineJoinRound);
        g.DrawLines(&line, pts.data(), static_cast<INT>(n));
    } else {
        SolidBrush dot(kLine);
        const float r = 3.0f * s;
        g.FillEllipse(&dot, pts[0].X - r, pts[0].Y - r, 2 * r, 2 * r);
    }
}

void DrawCandles(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc, float s) {
    assert(srs.count > 0);
    const size_t n    = min(srs.count, kMaxPoints);
    const float  slot = SlotWidth(L.price, n);
    const float  body = max(1.0f, std::floor(slot * 0.65f));
    SolidBrush up(kUp);
    SolidBrush down(kDown);
    Pen        upPen(kUp, max(1.0f, s));
    Pen        downPen(kDown, max(1.0f, s));
    for (size_t i = 0; i < n; ++i) {
        const Candle& c   = srs.pts[i];
        const bool    isUp = c.close >= c.open;
        const float   x   = XFor(i, L.price, n);
        g.DrawLine(isUp ? &upPen : &downPen, x, YFor(c.high, L.price, sc), x, YFor(c.low, L.price, sc));
        const float yTop = YFor(max(c.open, c.close), L.price, sc);
        const float yBot = YFor(min(c.open, c.close), L.price, sc);
        g.FillRectangle(isUp ? &up : &down, x - body / 2, yTop, body, max(1.0f, yBot - yTop));
    }
}

void DrawVolume(Graphics& g, const Layout& L, const Series& srs) {
    assert(srs.count > 0);
    const size_t n = min(srs.count, kMaxPoints);
    double maxVol = 0.0;
    for (size_t i = 0; i < n; ++i) { maxVol = max(maxVol, srs.pts[i].volume); }
    if (maxVol <= 0.0) { return; }
    const float slot = SlotWidth(L.volume, n);
    const float bw   = max(1.0f, std::floor(slot * 0.7f));
    SolidBrush up(Color(255, 160, 210, 175));
    SolidBrush down(Color(255, 235, 170, 165));
    SolidBrush flat(kVolume);
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

void DrawLastPriceTag(Graphics& g, const Layout& L, const Series& srs, const PriceScale& sc,
                      const Font& font, float s) {
    assert(srs.count > 0);
    const Candle& last = srs.pts[srs.count - 1];
    const bool   isUp  = (srs.count < 2) || last.close >= srs.pts[srs.count - 2].close;
    const Color& col   = isUp ? kUp : kDown;
    const float  y     = YFor(last.close, L.price, sc);
    Pen dash(col, 1.0f);
    dash.SetDashStyle(DashStyleDash);
    g.DrawLine(&dash, L.price.X, y, L.price.X + L.price.Width, y);

    const std::wstring label = FormatPrice(last.close);
    const float tagH = 18.0f * s;
    const RectF tag(L.axisR.X + 2.0f * s, y - tagH / 2, L.axisR.Width - 4.0f * s, tagH);
    SolidBrush   bg(col);
    SolidBrush   txt(kTipText);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.FillRectangle(&bg, tag);
    g.DrawString(label.c_str(), -1, &font, tag, &fmt, &txt);
}

void DrawHover(Graphics& g, const Layout& L, const ChartInput& in, const PriceScale& sc,
               const Font& font, float s) {
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

    Pen cross(kHover, 1.0f);
    cross.SetDashStyle(DashStyleDot);
    g.DrawLine(&cross, x, L.price.Y, x, L.volume.Y + L.volume.Height);
    g.DrawLine(&cross, L.price.X, y, L.price.X + L.price.Width, y);
    SolidBrush dot(kLine);
    const float r = 4.0f * s;
    g.FillEllipse(&dot, x - r, y - r, 2 * r, 2 * r);

    const DateStyle ds = in.range->intraday ? DateStyle::FullTime : DateStyle::Full;
    std::wstring tip = FormatDate(c.time, in.data->meta.gmtOffsetSec, ds);
    tip += L"\nO " + FormatPrice(c.open) + L"   H " + FormatPrice(c.high);
    tip += L"\nL " + FormatPrice(c.low)  + L"   C " + FormatPrice(c.close);
    tip += L"\nVol " + FormatVolume(c.volume);

    RectF bound;
    g.MeasureString(tip.c_str(), -1, &font, PointF(0, 0), &bound);
    const float pad = 8.0f * s;
    float bx = x + 14.0f * s;
    float by = L.price.Y + 8.0f * s;
    const float bw = bound.Width + 2 * pad;
    const float bh = bound.Height + 2 * pad;
    if (bx + bw > L.price.X + L.price.Width) { bx = x - 14.0f * s - bw; }
    bx = max(bx, L.price.X);
    SolidBrush bg(kTipBg);
    SolidBrush txt(kTipText);
    g.FillRectangle(&bg, bx, by, bw, bh);
    g.DrawString(tip.c_str(), -1, &font, PointF(bx + pad, by + pad), &txt);
}

void DrawMessage(Graphics& g, const RectF& rc, const std::wstring& text, const Font& font) {
    SolidBrush   txt(kMessage);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &font, rc, &fmt, &txt);
}

} // namespace

REAL FontPx(float pt, float scale) {
    assert(pt > 0.0f && scale > 0.0f);
    return pt * scale * (96.0f / 72.0f);
}

void DrawChart(Graphics& g, const RectF& rc, const ChartInput& in, float scale) {
    assert(scale > 0.0f);
    assert(in.range != nullptr);
    if (rc.Width < 40.0f || rc.Height < 40.0f) { return; }

    const Font axisFont(L"Segoe UI", FontPx(8.5f, scale), FontStyleRegular, UnitPixel);
    const Font body(L"Segoe UI", FontPx(11.0f, scale), FontStyleRegular, UnitPixel);

    if (in.data == nullptr) {
        DrawMessage(g, rc, L"Loading…", body);
        return;
    }
    if (!in.data->valid) {
        DrawMessage(g, rc, in.data->error.empty() ? L"No data" : in.data->error, body);
        return;
    }
    const Series& srs = in.data->series;
    PriceScale sc;
    if (!ComputeScale(srs, in.candles, sc)) {
        DrawMessage(g, rc, L"No trades in this range", body);
        return;
    }

    const Layout L = MakeLayout(rc, scale);
    DrawPriceGrid(g, L, sc, axisFont, scale);
    DrawDateAxis(g, L, srs, *in.range, in.data->meta, axisFont, scale);

    g.SetClip(RectF(L.price.X, L.price.Y - 2.0f, L.price.Width, L.price.Height + 4.0f));
    if (in.candles) { DrawCandles(g, L, srs, sc, scale); }
    else            { DrawLineSeries(g, L, srs, sc, scale); }
    g.ResetClip();

    DrawVolume(g, L, srs);
    DrawLastPriceTag(g, L, srs, sc, axisFont, scale);
    DrawHover(g, L, in, sc, axisFont, scale);
}

} // namespace st
