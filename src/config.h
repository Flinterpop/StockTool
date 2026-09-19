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

} // namespace st
