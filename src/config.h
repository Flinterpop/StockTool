// Configuration file (INI format) loading.
#pragma once

#include "common.h"

namespace st {

struct StockEntry {
    std::wstring symbol;
    std::wstring name;
};

struct Config {
    std::array<StockEntry, kMaxStocks> stocks{};
    size_t       stockCount     = 0;
    unsigned     refreshSeconds = 60;
    size_t       defaultRange   = 5;  // index into kRanges
    std::wstring urlTemplate;
    std::wstring path;
};

// Path of stocktool.cfg next to the running executable.
std::wstring DefaultConfigPath();

// Writes the built-in default configuration to `path`.
bool WriteDefaultConfig(const std::wstring& path, std::wstring& err);

// Loads `path` into `out`. On failure `err` describes the problem.
bool LoadConfig(const std::wstring& path, Config& out, std::wstring& err);

// Trims, upper-cases and checks a user-typed symbol (Yahoo notation, e.g.
// TD.TO, BRK-B, ^GSPTSE). On failure `err` says what is wrong.
bool NormalizeSymbol(std::wstring& symbol, std::wstring& err);

// Trims a display name and strips characters the INI format cannot hold.
std::wstring NormalizeName(const std::wstring& name, const std::wstring& fallback);

// Writes/updates or deletes one "symbol=name" line under [stocks] in `path`.
bool WriteStockEntry(const std::wstring& path, const StockEntry& entry, std::wstring& err);
bool DeleteStockEntry(const std::wstring& path, const std::wstring& symbol, std::wstring& err);

} // namespace st
