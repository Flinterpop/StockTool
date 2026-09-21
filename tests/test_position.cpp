// Average-cost position accounting from transactions.
#include "position.h"
#include "textfmt.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace st;
using Catch::Approx;

namespace {

constexpr int64_t kDay = 86400;

Transaction Tx(int day, double qty, double price) {
    Transaction t;
    t.date  = 1'700'000'000 + static_cast<int64_t>(day) * kDay;
    t.qty   = qty;
    t.price = price;
    return t;
}

} // namespace

TEST_CASE("buys average into the ACB; sells realise against it") {
    const Transaction tx[] = {
        Tx(0, 100, 100.0),    // 100 @ 100 -> ACB 100
        Tx(10, 100, 120.0),   // 200 @ avg 110
        Tx(20, -50, 130.0),   // sell 50: realised 50 * (130 - 110) = 1000; 150 left @ 110
        Tx(30, 50, 90.0),     // 200 held, cost 16500 + 4500 = 21000 -> ACB 105
    };
    const Position p = ComputePosition(tx, 4);
    CHECK(p.qty == Approx(200.0));
    CHECK(p.acb == Approx(105.0));
    CHECK(p.cost == Approx(21000.0));
    CHECK(p.realised == Approx(1000.0));
    CHECK(p.invested == Approx(10000.0 + 12000.0 + 4500.0));
}

TEST_CASE("file order does not matter; date order does") {
    const Transaction a[] = { Tx(20, -50, 130.0), Tx(0, 100, 100.0), Tx(10, 100, 120.0) };
    const Transaction b[] = { Tx(0, 100, 100.0), Tx(10, 100, 120.0), Tx(20, -50, 130.0) };
    const Position pa = ComputePosition(a, 3);
    const Position pb = ComputePosition(b, 3);
    CHECK(pa.qty == Approx(pb.qty));
    CHECK(pa.acb == Approx(pb.acb));
    CHECK(pa.realised == Approx(pb.realised));
    CHECK(pa.realised == Approx(1000.0));
}

TEST_CASE("selling everything clears the position; overselling is clipped") {
    const Transaction tx[] = { Tx(0, 10, 50.0), Tx(1, -25, 60.0) };
    const Position p = ComputePosition(tx, 2);
    CHECK(p.qty == 0.0);
    CHECK(p.acb == 0.0);
    CHECK(p.cost == 0.0);
    CHECK(p.realised == Approx(10 * (60.0 - 50.0)));   // only the 10 held are sold
}

TEST_CASE("empty input is an empty position") {
    const Position p = ComputePosition(nullptr, 0);
    CHECK(p.qty == 0.0);
    CHECK(p.realised == 0.0);
}

TEST_CASE("shares held at a date and dividends received") {
    const Transaction tx[] = { Tx(0, 100, 10.0), Tx(10, 50, 11.0), Tx(20, -150, 12.0) };
    CHECK(SharesHeldAt(tx, 3, Tx(0, 0, 0).date) == 0.0);          // bought that day: not yet held
    CHECK(SharesHeldAt(tx, 3, Tx(5, 0, 0).date) == Approx(100.0));
    CHECK(SharesHeldAt(tx, 3, Tx(15, 0, 0).date) == Approx(150.0));
    CHECK(SharesHeldAt(tx, 3, Tx(25, 0, 0).date) == 0.0);

    const Dividend divs[] = {
        { Tx(5, 0, 0).date, 0.50 },    // 100 held -> 50
        { Tx(15, 0, 0).date, 0.50 },   // 150 held -> 75
        { Tx(25, 0, 0).date, 0.50 },   // nothing held
        { Tx(-5, 0, 0).date, 0.50 },   // before the first buy
    };
    CHECK(DividendsReceived(tx, 3, divs, 4) == Approx(125.0));
    CHECK(DividendsReceived(nullptr, 0, divs, 4) == 0.0);
}

TEST_CASE("ISO dates round-trip and reject impossible days") {
    int64_t t = 0;
    REQUIRE(ParseIsoDate(L"2026-09-20", t));
    CHECK(FormatIsoDate(t) == L"2026-09-20");
    CHECK(t % kDay == 0);                       // midnight UTC
    CHECK_FALSE(ParseIsoDate(L"2026-02-30", t));
    CHECK_FALSE(ParseIsoDate(L"20-09-2026", t));
    CHECK_FALSE(ParseIsoDate(L"nonsense", t));
}
