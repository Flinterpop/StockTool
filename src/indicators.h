// Technical indicators over a Series. Outputs are NaN where undefined.
#pragma once

#include "common.h"

namespace st {

using Track = std::array<double, kMaxPoints>;

// Simple moving average of closes over `period` bars.
void Sma(const Series& s, size_t period, Track& out);

// Bollinger bands: SMA(period) +/- k standard deviations.
void Bollinger(const Series& s, size_t period, double k, Track& mid, Track& upper, Track& lower);

// Relative strength index (Wilder smoothing), 0..100.
void Rsi(const Series& s, size_t period, Track& out);

} // namespace st
