// Shared types and compile-time limits for StockTool.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace st {

// Fixed capacities (Power of 10: no unbounded growth).
constexpr size_t kMaxStocks   = 32;
constexpr size_t kMaxPoints   = 2048;
constexpr size_t kMaxJobs     = 128;
constexpr size_t kHttpBufSize = 4u * 1024u * 1024u;

// One bar of OHLCV data. Time is Unix seconds (UTC).
struct Candle {
    int64_t time   = 0;
    double  open   = 0.0;
    double  high   = 0.0;
    double  low    = 0.0;
    double  close  = 0.0;
    double  volume = 0.0;
};

struct Series {
    size_t count = 0;
    std::array<Candle, kMaxPoints> pts{};
};

// Cash dividend event (from the chart endpoint's events=div).
constexpr size_t kMaxDividends = 64;

struct Dividend {
    int64_t time   = 0;   // ex-date, Unix seconds
    double  amount = 0.0; // per share
};

// Fields lifted from the provider's chart "meta" block.
struct QuoteMeta {
    std::wstring currency;
    std::wstring exchange;
    std::wstring longName;
    double  price          = 0.0;  // regular-market price
    double  chartPrevClose = 0.0;  // close immediately before the requested range
    double  dayHigh        = 0.0;
    double  dayLow         = 0.0;
    double  dayVolume      = 0.0;
    double  wk52High       = 0.0;
    double  wk52Low        = 0.0;
    int64_t marketTime     = 0;    // Unix seconds of last trade
    int32_t gmtOffsetSec   = 0;    // exchange local offset from UTC
    int64_t regularStart   = 0;    // today's regular session, Unix seconds (0 = unknown)
    int64_t regularEnd     = 0;
};

// Regular session open right now? False when the provider gave no session.
inline bool MarketOpen(const QuoteMeta& m, int64_t now) {
    return m.regularStart > 0 && m.regularEnd > m.regularStart && now >= m.regularStart && now < m.regularEnd;
}

struct QuoteData {
    bool         valid = false;
    std::wstring symbol;   // the symbol this was fetched for
    std::wstring error;    // why it failed; with `source` set, why the primary provider failed
    std::wstring source;   // "" = primary provider, else the fallback that supplied the data ("TMX")
    QuoteMeta    meta;
    Series       series;
    size_t       dividendCount = 0;
    std::array<Dividend, kMaxDividends> dividends{};   // ascending by time
};

// One headline from the news endpoint.
constexpr size_t kMaxNews = 8;

struct NewsItem {
    std::wstring title;
    std::wstring publisher;
    std::wstring link;
    int64_t      time = 0;   // Unix seconds
};

// Cached FX rate: 1 unit of `from` = `rate` units of `to`.
constexpr size_t kMaxFx = 8;

struct FxRate {
    std::wstring from;
    std::wstring to;
    double       rate  = 0.0;
    bool         valid = false;
};

// Watch lists: the default [stocks] section plus named [stocks.<name>] ones.
constexpr size_t kMaxLists = 8;

// Fundamentals from the (optional) batch quote endpoint. Zero = unknown.
struct QuoteStats {
    bool         valid = false;
    std::wstring symbol;
    double marketCap        = 0.0;
    double trailingPE       = 0.0;
    double forwardPE        = 0.0;
    double eps              = 0.0;
    double dividendYieldPct = 0.0;  // percent, e.g. 3.9
    double dividendRate     = 0.0;  // annual, per share
    double avgVolume3M      = 0.0;
    double priceToBook      = 0.0;
    double open             = 0.0;
    double prevClose        = 0.0;
};

// One row from the symbol search endpoint.
constexpr size_t kMaxSearchHits = 20;

struct SearchHit {
    std::wstring symbol;
    std::wstring name;
    std::wstring exchange;   // e.g. "Toronto"
    std::wstring type;       // e.g. "Equity", "ETF"
};

// Per-ticker user data kept in stocktool.cfg.
struct Holding {
    double qty  = 0.0;  // shares held
    double cost = 0.0;  // average cost per share
};

// A buy (qty > 0) or sell (qty < 0) on a date, at a price per share.
constexpr size_t kMaxTxPerSymbol = 64;

struct Transaction {
    int64_t date  = 0;    // Unix seconds at 00:00 UTC of the trade date
    double  qty   = 0.0;  // shares; negative = sell
    double  price = 0.0;  // per share, in the ticker's currency
};

// [symbol_map] broker symbol -> watch-list symbol, for the broker import.
constexpr size_t kMaxSymbolMap    = 32;
constexpr size_t kMaxKnownSymbols = kMaxStocks * kMaxLists;

struct SymbolMapEntry {
    std::wstring from;
    std::wstring to;
};

// Health of one data endpoint, for the diagnostics panel.
struct EndpointHealth {
    const wchar_t* name       = L"";
    uint32_t       lastStatus = 0;     // HTTP status of the last attempt (0 = transport error / none)
    uint32_t       lastMs     = 0;     // latency of the last attempt
    int64_t        lastOk     = 0;     // Unix time of the last success
    int64_t        lastFail   = 0;
    uint32_t       failures   = 0;     // consecutive failures
    std::wstring   lastError;
};
constexpr size_t kEndpointCount = 7;   // chart, fundamentals, search, fx, news, crumb, tmx

struct Alert {
    double above = 0.0;  // 0 = unset
    double below = 0.0;
};

// Chart range presets. The interval is chosen so the point count stays
// well under kMaxPoints.
struct RangeSpec {
    const wchar_t* label;
    const wchar_t* range;
    const wchar_t* interval;
    bool           intraday;
};

constexpr std::array<RangeSpec, 8> kRanges = {{
    { L"1D",  L"1d",  L"5m",  true  },
    { L"5D",  L"5d",  L"15m", true  },
    { L"1M",  L"1mo", L"1d",  false },
    { L"6M",  L"6mo", L"1d",  false },
    { L"YTD", L"ytd", L"1d",  false },
    { L"1Y",  L"1y",  L"1d",  false },
    { L"5Y",  L"5y",  L"1wk", false },
    { L"MAX", L"max", L"1mo", false },
}};

constexpr size_t kRange1Y = 5;
constexpr size_t kRange5Y = 6;

// Daily bars over the last week: enough for last price, previous close and
// today's open/high/low/volume in the list and stats panel.
constexpr RangeSpec kSummarySpec = { L"5D", L"5d", L"1d", false };

} // namespace st
