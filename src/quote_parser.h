// Parses the provider's JSON into QuoteData / QuoteStats.
#pragma once

#include "common.h"

namespace st {

// Chart endpoint: parses `len` bytes of JSON at `data`. Returns false and
// sets `err` (including any error message embedded in the response).
bool ParseChartJson(const char* data, size_t len, QuoteData& out, std::wstring& err);

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
