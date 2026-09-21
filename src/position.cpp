#include "position.h"

#include <algorithm>
#include <cassert>

namespace st {
namespace {

// Transactions are kept in file order; the walk needs date order. The list
// is small (kMaxTxPerSymbol), so a sorted copy of indices is cheap.
struct Ordered {
    std::array<const Transaction*, kMaxTxPerSymbol> tx{};
    size_t count = 0;
};

Ordered Sorted(const Transaction* tx, size_t count) {
    Ordered o;
    o.count = std::min(count, kMaxTxPerSymbol);
    for (size_t i = 0; i < o.count; ++i) { o.tx[i] = &tx[i]; }
    std::stable_sort(o.tx.begin(), o.tx.begin() + static_cast<std::ptrdiff_t>(o.count),
                     [](const Transaction* a, const Transaction* b) { return a->date < b->date; });
    return o;
}

// Applies one transaction to a running position.
void Apply(Position& p, const Transaction& t) {
    if (t.qty > 0.0) {
        const double newCost = p.cost + t.qty * t.price;
        p.qty     += t.qty;
        p.cost     = newCost;
        p.acb      = (p.qty > 0.0) ? p.cost / p.qty : 0.0;
        p.invested += t.qty * t.price;
    } else if (t.qty < 0.0) {
        const double sold = std::min(-t.qty, p.qty);   // cannot sell more than held
        p.realised += sold * (t.price - p.acb);
        p.qty      -= sold;
        p.cost      = p.qty * p.acb;
        if (p.qty <= 1e-9) {
            p.qty  = 0.0;
            p.cost = 0.0;
            p.acb  = 0.0;
        }
    }
    assert(p.qty >= 0.0 && p.cost >= -1e-6);
}

} // namespace

Position ComputePosition(const Transaction* tx, size_t count) {
    Position p;
    if (tx == nullptr || count == 0) { return p; }
    const Ordered o = Sorted(tx, count);
    for (size_t i = 0; i < o.count; ++i) { Apply(p, *o.tx[i]); }
    return p;
}

double SharesHeldAt(const Transaction* tx, size_t count, int64_t time) {
    if (tx == nullptr || count == 0) { return 0.0; }
    const Ordered o = Sorted(tx, count);
    Position p;
    for (size_t i = 0; i < o.count; ++i) {
        if (o.tx[i]->date >= time) { break; }
        Apply(p, *o.tx[i]);
    }
    return p.qty;
}

double DividendsReceived(const Transaction* tx, size_t count, const Dividend* divs, size_t divCount) {
    if (tx == nullptr || count == 0 || divs == nullptr) { return 0.0; }
    double total = 0.0;
    for (size_t k = 0; k < divCount && k < kMaxDividends; ++k) {
        // Ex-date at the provider's timestamp; shares bought that same day do
        // not qualify, so "held before the ex-date" is the right test.
        const double held = SharesHeldAt(tx, count, divs[k].time);
        if (held > 0.0) { total += held * divs[k].amount; }
    }
    assert(total >= 0.0);
    return total;
}

} // namespace st
