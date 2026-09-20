// Parser tests against hand-written samples shaped like the provider's JSON.
#include "quote_parser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace st;
using Catch::Approx;

namespace {

// A trimmed-down chart response: three bars, the middle one with a null
// close (no trade) and a null volume.
constexpr char kChartJson[] = R"({
  "chart": {
    "result": [{
      "meta": {
        "currency": "CAD", "symbol": "RY.TO", "exchangeName": "TOR",
        "fullExchangeName": "Toronto", "longName": "Royal Bank of Canada",
        "regularMarketPrice": 284.45, "chartPreviousClose": 280.1,
        "regularMarketDayHigh": 285.05, "regularMarketDayLow": 282.05,
        "regularMarketVolume": 7900000, "fiftyTwoWeekHigh": 306.38,
        "fiftyTwoWeekLow": 200.82, "regularMarketTime": 1789848000, "gmtoffset": -14400
      },
      "timestamp": [1789675200, 1789761600, 1789848000],
      "indicators": { "quote": [{
        "open":   [283.0, 284.0, 284.41],
        "high":   [284.5, 285.0, 285.05],
        "low":    [282.0, 283.5, 282.05],
        "close":  [283.9, null,  284.45],
        "volume": [6500000, null, 7900000]
      }]}
    }],
    "error": null
  }
})";

constexpr char kErrorJson[] = R"({"chart":{"result":null,"error":{"code":"Not Found","description":"No data found, symbol may be delisted"}}})";

constexpr char kQuoteJson[] = R"({
  "quoteResponse": {
    "result": [
      { "symbol": "RY.TO", "marketCap": 401000000000, "trailingPE": 15.2, "forwardPE": 13.9,
        "epsTrailingTwelveMonths": 18.7, "dividendYield": 2.1, "trailingAnnualDividendRate": 5.92,
        "averageDailyVolume3Month": 3200000, "regularMarketOpen": 284.41, "regularMarketPreviousClose": 284.93 },
      { "symbol": "DOL.TO", "marketCap": 48000000000, "trailingAnnualDividendYield": 0.0025 }
    ],
    "error": null
  }
})";

constexpr char kAuthErrorJson[] = R"({"finance":{"result":null,"error":{"code":"Unauthorized","description":"Invalid Crumb"}}})";

} // namespace

TEST_CASE("chart JSON: meta and bars are parsed, null bars are skipped") {
    QuoteData q;
    std::wstring err;
    REQUIRE(ParseChartJson(kChartJson, std::strlen(kChartJson), q, err));
    REQUIRE(q.valid);
    CHECK(q.meta.currency == L"CAD");
    CHECK(q.meta.exchange == L"Toronto");
    CHECK(q.meta.longName == L"Royal Bank of Canada");
    CHECK(q.meta.price == Approx(284.45));
    CHECK(q.meta.chartPrevClose == Approx(280.1));
    CHECK(q.meta.wk52High == Approx(306.38));
    CHECK(q.meta.marketTime == 1789848000);
    CHECK(q.meta.gmtOffsetSec == -14400);

    REQUIRE(q.series.count == 2);  // the null-close bar is dropped
    CHECK(q.series.pts[0].time == 1789675200);
    CHECK(q.series.pts[0].close == Approx(283.9));
    CHECK(q.series.pts[0].volume == Approx(6500000));
    CHECK(q.series.pts[1].time == 1789848000);
    CHECK(q.series.pts[1].open == Approx(284.41));
    CHECK(q.series.pts[1].close == Approx(284.45));
}

TEST_CASE("chart JSON: provider error block is surfaced") {
    QuoteData q;
    std::wstring err;
    CHECK_FALSE(ParseChartJson(kErrorJson, std::strlen(kErrorJson), q, err));
    CHECK_FALSE(q.valid);
    CHECK(err.find(L"delisted") != std::wstring::npos);
}

TEST_CASE("chart JSON: garbage and empty input are rejected") {
    QuoteData q;
    std::wstring err;
    CHECK_FALSE(ParseChartJson("<html>", 6, q, err));
    CHECK_FALSE(ParseChartJson("{}", 2, q, err));
    CHECK_FALSE(ParseChartJson("", 0, q, err));
}

TEST_CASE("chart JSON: only the newest kMaxPoints bars are kept") {
    std::string json = R"({"chart":{"result":[{"meta":{},"timestamp":[)";
    const size_t n = kMaxPoints + 10;
    for (size_t i = 0; i < n; ++i) { json += (i ? "," : "") + std::to_string(1000 + i); }
    json += R"(],"indicators":{"quote":[{"close":[)";
    for (size_t i = 0; i < n; ++i) { json += (i ? "," : "") + std::to_string(i); }
    json += "]}]}}]}}";
    QuoteData q;
    std::wstring err;
    REQUIRE(ParseChartJson(json.data(), json.size(), q, err));
    CHECK(q.series.count == kMaxPoints);
    CHECK(q.series.pts[0].time == 1010);                    // first 10 dropped
    CHECK(q.series.pts[kMaxPoints - 1].time == static_cast<int64_t>(1000 + n - 1));
}

TEST_CASE("quote JSON: fundamentals per symbol") {
    std::array<QuoteStats, kMaxStocks> out{};
    size_t count = 0;
    std::wstring err;
    REQUIRE(ParseQuoteBatchJson(kQuoteJson, std::strlen(kQuoteJson), out, count, err));
    REQUIRE(count == 2);
    CHECK(out[0].symbol == L"RY.TO");
    CHECK(out[0].marketCap == Approx(401e9));
    CHECK(out[0].trailingPE == Approx(15.2));
    CHECK(out[0].dividendYieldPct == Approx(2.1));
    CHECK(out[0].dividendRate == Approx(5.92));
    CHECK(out[0].prevClose == Approx(284.93));
    CHECK(out[1].symbol == L"DOL.TO");
    CHECK(out[1].dividendYieldPct == Approx(0.25));  // fraction form scaled to percent
    CHECK(out[1].trailingPE == 0.0);                 // absent = unknown
}

TEST_CASE("search JSON: hits with names, exchange and type") {
    constexpr char kSearchJson[] = R"({
      "count": 3,
      "quotes": [
        { "symbol": "RY.TO", "shortname": "ROYAL BANK OF CANADA", "longname": "Royal Bank of Canada",
          "exchDisp": "Toronto", "typeDisp": "Equity", "quoteType": "EQUITY", "exchange": "TOR" },
        { "symbol": "RY", "shortname": "Royal Bank of Canada", "exchange": "NYQ", "quoteType": "EQUITY" },
        { "index": "quotes", "score": 1 }
      ],
      "news": []
    })";
    std::array<SearchHit, kMaxSearchHits> out{};
    size_t count = 0;
    std::wstring err;
    REQUIRE(ParseSearchJson(kSearchJson, std::strlen(kSearchJson), out, count, err));
    REQUIRE(count == 2);                       // the symbol-less entry is skipped
    CHECK(out[0].symbol == L"RY.TO");
    CHECK(out[0].name == L"Royal Bank of Canada");   // longname preferred
    CHECK(out[0].exchange == L"Toronto");
    CHECK(out[0].type == L"Equity");
    CHECK(out[1].symbol == L"RY");
    CHECK(out[1].name == L"Royal Bank of Canada");   // shortname fallback
    CHECK(out[1].exchange == L"NYQ");                // exchange code fallback
    CHECK(out[1].type == L"EQUITY");

    CHECK_FALSE(ParseSearchJson("{\"finance\":{\"error\":{\"description\":\"Too Many Requests\"}}}", 66, out, count, err));
    CHECK(err.find(L"Too Many") != std::wstring::npos);
}

TEST_CASE("quote JSON: auth failure is reported") {
    std::array<QuoteStats, kMaxStocks> out{};
    size_t count = 0;
    std::wstring err;
    CHECK_FALSE(ParseQuoteBatchJson(kAuthErrorJson, std::strlen(kAuthErrorJson), out, count, err));
    CHECK(count == 0);
    CHECK(err.find(L"Crumb") != std::wstring::npos);
}
