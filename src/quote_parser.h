// Parses the provider's JSON into QuoteData / QuoteStats.
#pragma once

#include "common.h"

namespace st {

// Chart endpoint: parses `len` bytes of JSON at `data`. Returns false and
// sets `err` (including any error message embedded in the response).
bool ParseChartJson(const char* data, size_t len, QuoteData& out, std::wstring& err);

// TMX Money GraphQL getTimeSeriesData reply (daily/weekly/monthly bars,
// newest first). Bars come out ascending; meta is derived from the newest
// bar and `source` is set to "TMX". Currency is left for the caller.
bool ParseTmxJson(const char* data, size_t len, QuoteData& out, std::wstring& err);

// The TMX Money symbol for a Yahoo one: RY.TO -> RY, RCI-B.TO -> RCI.B,
// AAPL -> AAPL:US, ^GSPTSE -> ^TSX. Empty when there is no equivalent
// (funds, FX pairs, other exchanges).
std::wstring TmxSymbol(const std::wstring& yahooSymbol);

// Batch quote endpoint: one QuoteStats per result, `count` set on return.
bool ParseQuoteBatchJson(const char* data, size_t len,
                         std::array<QuoteStats, kMaxStocks>& out, size_t& count, std::wstring& err);

// Symbol search endpoint: one SearchHit per quote result, `count` set on return.
bool ParseSearchJson(const char* data, size_t len,
                     std::array<SearchHit, kMaxSearchHits>& out, size_t& count, std::wstring& err);

// Same endpoint's "news" array: newest first as delivered.
bool ParseNewsJson(const char* data, size_t len,
                   std::array<NewsItem, kMaxNews>& out, size_t& count, std::wstring& err);

// RSS 2.0 feed (Google News and friends): <item> title/link/pubDate/source.
bool ParseNewsRss(const char* data, size_t len,
                  std::array<NewsItem, kMaxNews>& out, size_t& count, std::wstring& err);

} // namespace st
