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

// Fields lifted from the provider's "meta" block.
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
};

struct QuoteData {
    bool         valid = false;
    std::wstring symbol;   // the symbol this was fetched for
    std::wstring error;
    QuoteMeta    meta;
    Series       series;
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

// Daily bars over the last week: enough for last price, previous close and
// today's open/high/low/volume in the list and stats panel.
constexpr RangeSpec kSummarySpec = { L"5D", L"5d", L"1d", false };

} // namespace st
