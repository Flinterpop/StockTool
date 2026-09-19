#include "indicators.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace st {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void Fill(Track& t) {
    for (size_t i = 0; i < kMaxPoints; ++i) { t[i] = kNaN; }
}

} // namespace

void Sma(const Series& s, size_t period, Track& out) {
    assert(period > 0);
    assert(s.count <= kMaxPoints);
    Fill(out);
    if (period == 0 || s.count < period) { return; }
    double sum = 0.0;
    for (size_t i = 0; i < s.count; ++i) {
        sum += s.pts[i].close;
        if (i >= period) { sum -= s.pts[i - period].close; }
        if (i + 1 >= period) { out[i] = sum / static_cast<double>(period); }
    }
}

void Bollinger(const Series& s, size_t period, double k, Track& mid, Track& upper, Track& lower) {
    assert(period > 1);
    assert(k > 0.0);
    Sma(s, period, mid);
    Fill(upper);
    Fill(lower);
    if (period < 2 || s.count < period) { return; }
    for (size_t i = period - 1; i < s.count; ++i) {
        const double m = mid[i];
        double var = 0.0;
        for (size_t j = i + 1 - period; j <= i; ++j) {
            const double d = s.pts[j].close - m;
            var += d * d;
        }
        const double sd = std::sqrt(var / static_cast<double>(period));
        upper[i] = m + k * sd;
        lower[i] = m - k * sd;
        assert(upper[i] >= lower[i]);
    }
}

void Rsi(const Series& s, size_t period, Track& out) {
    assert(period > 0);
    Fill(out);
    if (period == 0 || s.count <= period) { return; }
    double avgGain = 0.0;
    double avgLoss = 0.0;
    // Seed with a simple average over the first `period` changes.
    for (size_t i = 1; i <= period; ++i) {
        const double d = s.pts[i].close - s.pts[i - 1].close;
        if (d > 0.0) { avgGain += d; } else { avgLoss -= d; }
    }
    avgGain /= static_cast<double>(period);
    avgLoss /= static_cast<double>(period);
    auto rsiOf = [](double g, double l) {
        if (l <= 0.0) { return 100.0; }
        const double rs = g / l;
        return 100.0 - 100.0 / (1.0 + rs);
    };
    out[period] = rsiOf(avgGain, avgLoss);
    // Wilder smoothing for the remainder.
    const double n = static_cast<double>(period);
    for (size_t i = period + 1; i < s.count; ++i) {
        const double d = s.pts[i].close - s.pts[i - 1].close;
        const double gain = (d > 0.0) ? d : 0.0;
        const double loss = (d < 0.0) ? -d : 0.0;
        avgGain = (avgGain * (n - 1.0) + gain) / n;
        avgLoss = (avgLoss * (n - 1.0) + loss) / n;
        out[i] = rsiOf(avgGain, avgLoss);
        assert(out[i] >= 0.0 && out[i] <= 100.0);
    }
}

} // namespace st
