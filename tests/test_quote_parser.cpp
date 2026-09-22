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

TEST_CASE("chart JSON: dividend events are parsed and sorted by time") {
    constexpr char kJson[] = R"({"chart":{"result":[{"meta":{"regularMarketPrice":10},
      "timestamp":[1000,2000,3000],"indicators":{"quote":[{"close":[1,2,3]}]},
      "events":{"dividends":{"2500":{"amount":0.5,"date":2500},"1500":{"amount":0.4,"date":1500},
                             "bad":{"amount":0,"date":1700}}}}],"error":null}})";
    QuoteData q;
    std::wstring err;
    REQUIRE(ParseChartJson(kJson, std::strlen(kJson), q, err));
    REQUIRE(q.dividendCount == 2);          // the zero-amount entry is dropped
    CHECK(q.dividends[0].time == 1500);
    CHECK(q.dividends[0].amount == Approx(0.4));
    CHECK(q.dividends[1].time == 2500);
    CHECK(q.dividends[1].amount == Approx(0.5));
}

TEST_CASE("news JSON: headlines with publisher, link and time") {
    constexpr char kJson[] = R"({"quotes":[],"news":[
      {"title":"Bank posts record quarter","publisher":"Reuters","link":"https://example.test/a","providerPublishTime":1789761600},
      {"title":"","publisher":"x","link":"https://example.test/b"},
      {"title":"Second headline","link":"https://example.test/c","providerPublishTime":1789700000}]})";
    std::array<NewsItem, kMaxNews> out{};
    size_t count = 0;
    std::wstring err;
    REQUIRE(ParseNewsJson(kJson, std::strlen(kJson), out, count, err));
    REQUIRE(count == 2);                    // the untitled item is skipped
    CHECK(out[0].title == L"Bank posts record quarter");
    CHECK(out[0].publisher == L"Reuters");
    CHECK(out[0].link == L"https://example.test/a");
    CHECK(out[0].time == 1789761600);
    CHECK(out[1].publisher.empty());
    CHECK_FALSE(ParseNewsJson("{}", 2, out, count, err));
}

TEST_CASE("news RSS: Google News items with source, entities and RFC 822 dates") {
    constexpr char kRss[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><rss version=\"2.0\"><channel><title>x</title>"
        "<item><title>Royal Bank &amp; peers rise as BoC holds - The Globe and Mail</title>"
        "<link>https://news.google.com/rss/articles/abc</link>"
        "<pubDate>Sat, 20 Sep 2026 12:00:00 GMT</pubDate>"
        "<source url=\"https://www.theglobeandmail.com\">The Globe and Mail</source></item>"
        "<item><title><![CDATA[RBC&#39;s CEO on rates]]></title><link>https://example.test/b</link>"
        "<pubDate>19 Sep 2026 08:30:00 GMT</pubDate></item>"
        "<item><title></title><link>https://example.test/empty</link></item>"
        "</channel></rss>";
    std::array<NewsItem, kMaxNews> out{};
    size_t count = 0;
    std::wstring err;
    REQUIRE(ParseNewsRss(kRss, std::strlen(kRss), out, count, err));
    REQUIRE(count == 2);                                           // untitled item skipped
    CHECK(out[0].title == L"Royal Bank & peers rise as BoC holds"); // publisher suffix stripped, entity decoded
    CHECK(out[0].publisher == L"The Globe and Mail");
    CHECK(out[0].link == L"https://news.google.com/rss/articles/abc");
    CHECK(out[0].time == 1789905600);                              // 2026-09-20 12:00 UTC
    CHECK(out[1].title == L"RBC's CEO on rates");                  // CDATA + numeric entity
    CHECK(out[1].publisher.empty());
    CHECK(out[1].time == 1789806600);                              // 2026-09-19 08:30 UTC, no weekday
    CHECK_FALSE(ParseNewsRss("<html>nope</html>", 16, out, count, err));
}

TEST_CASE("TMX JSON: newest-first bars come out ascending with meta from the last bar") {
    const char* json =
        "{\"data\":{\"getTimeSeriesData\":["
        "{\"dateTime\":\"2026-09-21T16:00:00-04:00\",\"open\":286.03,\"high\":289.72,\"low\":286.03,\"close\":289.64,\"volume\":1579437},"
        "{\"dateTime\":\"2026-09-18T16:00:00-04:00\",\"open\":284.41,\"high\":285.05,\"low\":282.05,\"close\":284.45,\"volume\":7898342},"
        "{\"dateTime\":\"bogus\",\"open\":1,\"high\":1,\"low\":1,\"close\":1,\"volume\":1},"
        "{\"dateTime\":\"2026-09-17T16:00:00-04:00\",\"open\":285.14,\"high\":286.77,\"low\":283.11,\"close\":284.93,\"volume\":2740252}"
        "]}}";
    QuoteData q;
    std::wstring err;
    REQUIRE(ParseTmxJson(json, strlen(json), q, err));
    CHECK(q.valid);
    CHECK(q.source == L"TMX");
    REQUIRE(q.series.count == 3);
    CHECK(q.series.pts[0].close == Approx(284.93));
    CHECK(q.series.pts[2].close == Approx(289.64));
    CHECK(q.series.pts[0].time < q.series.pts[1].time);
    CHECK(q.series.pts[2].time == 1790020800);            // 2026-09-21 16:00 EDT
    CHECK(q.meta.gmtOffsetSec == -4 * 3600);
    CHECK(q.meta.price == Approx(289.64));
    CHECK(q.meta.marketTime == 1790020800);
    CHECK(q.meta.dayVolume == Approx(1579437.0));

    const char* empty = "{\"data\":{\"getTimeSeriesData\":[]}}";
    CHECK_FALSE(ParseTmxJson(empty, strlen(empty), q, err));
    const char* gqlErr = "{\"errors\":[{\"message\":\"Cannot query field\"}],\"data\":null}";
    CHECK_FALSE(ParseTmxJson(gqlErr, strlen(gqlErr), q, err));
    CHECK(err.find(L"Cannot query field") != std::wstring::npos);
    CHECK_FALSE(ParseTmxJson("nope", 4, q, err));
}

TEST_CASE("TMX symbols map from Yahoo notation") {
    CHECK(TmxSymbol(L"RY.TO") == L"RY");
    CHECK(TmxSymbol(L"RCI-B.TO") == L"RCI.B");
    CHECK(TmxSymbol(L"XYZ.V") == L"XYZ");
    CHECK(TmxSymbol(L"AAPL") == L"AAPL:US");
    CHECK(TmxSymbol(L"BRK-B") == L"BRK.B:US");
    CHECK(TmxSymbol(L"^GSPTSE") == L"^TSX");
    CHECK(TmxSymbol(L"^GSPC") == L"^SPX");
    CHECK(TmxSymbol(L"^FTSE").empty());
    CHECK(TmxSymbol(L"USDCAD=X").empty());
    CHECK(TmxSymbol(L"0P000071WA.TO").empty());
    CHECK(TmxSymbol(L"BMW.DE").empty());
    CHECK(TmxSymbol(L"").empty());
}

TEST_CASE("chart JSON: the regular trading session is read from the meta block") {
    const char* json =
        "{\"chart\":{\"result\":[{\"meta\":{\"currency\":\"CAD\",\"regularMarketPrice\":1.0,"
        "\"currentTradingPeriod\":{\"regular\":{\"start\":1789997400,\"end\":1790020800}}},"
        "\"timestamp\":[1790000000],\"indicators\":{\"quote\":[{\"open\":[1.0],\"high\":[1.0],\"low\":[1.0],\"close\":[1.0],\"volume\":[1]}]}}]}}";
    QuoteData q;
    std::wstring err;
    REQUIRE(ParseChartJson(json, strlen(json), q, err));
    CHECK(q.meta.regularStart == 1789997400);
    CHECK(q.meta.regularEnd == 1790020800);
    CHECK(MarketOpen(q.meta, 1790000000));
    CHECK_FALSE(MarketOpen(q.meta, 1790020800));   // the close itself is after hours
    CHECK_FALSE(MarketOpen(q.meta, 1789997399));
    QuoteMeta none;
    CHECK_FALSE(MarketOpen(none, 1790000000));
}

