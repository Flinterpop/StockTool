// Parses the provider's chart JSON into QuoteData.
#pragma once

#include "common.h"

namespace st {

// Parses `len` bytes of JSON at `data`. Returns false and sets `err`
// (including any error message embedded in the response) on failure.
bool ParseChartJson(const char* data, size_t len, QuoteData& out, std::wstring& err);

} // namespace st
