// Configuration file (INI format) loading and writing.
#pragma once

#include "common.h"

#include <windows.h>

namespace st {

struct StockEntry {
    std::wstring symbol;
    std::wstring name;
    Holding      holding;
    Alert        alert;
};

enum class ThemeMode { System, Light, Dark };

// User-editable settings: [settings] and [stocks] (+ [holdings], [alerts]).
struct Config {
    std::array<StockEntry, kMaxStocks> stocks{};
    size_t       stockCount     = 0;
    unsigned     refreshSeconds = 60;
    size_t       defaultRange   = kRange1Y;  // index into kRanges
    size_t       insetRange     = kRange1Y;  // 1Y or 5Y trend inset
    ThemeMode    theme          = ThemeMode::System;
    bool         startMinimized = false;
    bool         minimizeToTray = false;
    std::wstring urlTemplate;                // chart endpoint
    std::wstring quoteUrlTemplate;           // batch fundamentals endpoint ("" = off)
    std::wstring path;
};

// Window/view state the app writes back to [state] on exit.
struct ViewState {
    bool   hasWindow = false;
    RECT   window{};
    bool   maximized = false;
    size_t range     = kRanges.size();  // kRanges.size() = not set
    bool   candles   = false;
    bool   compare   = false;
    bool   sma20     = false;
    bool   sma50     = false;
    bool   bollinger = false;
    bool   rsi       = false;
    bool   inset     = true;
    std::wstring selected;              // symbol
};

// Path of stocktool.cfg next to the running executable.
std::wstring DefaultConfigPath();

// Writes the built-in default configuration to `path`.
bool WriteDefaultConfig(const std::wstring& path, std::wstring& err);

// Loads `path` into `out`. On failure `err` describes the problem.
bool LoadConfig(const std::wstring& path, Config& out, std::wstring& err);

void LoadViewState(const std::wstring& path, ViewState& out);
bool SaveViewState(const std::wstring& path, const ViewState& state, std::wstring& err);

// Trims, upper-cases and checks a user-typed symbol (Yahoo notation, e.g.
// TD.TO, BRK-B, ^GSPTSE). On failure `err` says what is wrong.
bool NormalizeSymbol(std::wstring& symbol, std::wstring& err);

// Trims a display name and strips characters the INI format cannot hold.
std::wstring NormalizeName(const std::wstring& name, const std::wstring& fallback);

// Writes/updates or deletes one ticker (its [stocks], [holdings] and
// [alerts] lines) in `path`.
bool WriteStockEntry(const std::wstring& path, const StockEntry& entry, std::wstring& err);
bool DeleteStockEntry(const std::wstring& path, const std::wstring& symbol, std::wstring& err);

// Rewrites the whole [stocks] section in the order of `cfg.stocks`.
bool WriteStockOrder(const std::wstring& path, const Config& cfg, std::wstring& err);

// Writes one key under [settings] (used for menu-driven settings).
bool WriteSetting(const std::wstring& path, const wchar_t* key, const std::wstring& value, std::wstring& err);

bool WriteHolding(const std::wstring& path, const std::wstring& symbol, const Holding& h, std::wstring& err);
bool WriteAlert(const std::wstring& path, const std::wstring& symbol, const Alert& a, std::wstring& err);

} // namespace st
