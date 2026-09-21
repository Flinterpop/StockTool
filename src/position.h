// Position accounting from a transaction list (average-cost method, as used
// for Canadian adjusted cost base). Pure functions, unit-tested.
#pragma once

#include "common.h"

namespace st {

struct Position {
    double qty      = 0.0;   // shares held now
    double acb      = 0.0;   // average cost per share of what is held (0 if none)
    double cost     = 0.0;   // qty * acb
    double realised = 0.0;   // realised gain/loss from sells, in the ticker's currency
    double invested = 0.0;   // total spent on buys (for reference)
};

// Processes transactions in date order. A sell larger than the position is
// clipped to what is held (the excess is ignored).
Position ComputePosition(const Transaction* tx, size_t count);

// Shares held immediately before `time` (i.e. after every transaction dated
// before it), from the same average-cost walk.
double SharesHeldAt(const Transaction* tx, size_t count, int64_t time);

// Cash dividends actually received: for each ex-date, shares held on that
// date times the amount per share.
double DividendsReceived(const Transaction* tx, size_t count, const Dividend* divs, size_t divCount);

} // namespace st
