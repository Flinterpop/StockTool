// Configuration file (INI format) loading and writing.
#pragma once

#include "common.h"

#include <windows.h>

namespace st {

struct StockEntry {
    std::wstring symbol;
    std::wstring name;
    std::wstring currency;   // override for the provider's currency ("" = provider)
    Holding      holding;
    Alert        alert;
};

enum class ThemeMode { System, Light, Dark };

// User-editable settings: [settings], the active watch list's [stocks...]
// section, plus [holdings], [alerts], [currency] (all keyed by symbol and
// shared across lists).
struct Config {
    std::array<StockEntry, kMaxStocks> stocks{};
    size_t       stockCount     = 0;
    std::wstring listName;                   // active list ("" = the default [stocks])
    std::array<std::wstring, kMaxLists> lists{};   // "" first = default list
    size_t       listCount      = 1;
    unsigned     refreshSeconds = 60;
    size_t       defaultRange   = kRange1Y;  // index into kRanges
    size_t       insetRange     = kRange5Y;  // 1Y or 5Y trend inset
    ThemeMode    theme          = ThemeMode::System;
    bool         startMinimized = false;
    bool         minimizeToTray = false;
    std::wstring portfolioCurrency;          // totals converted into this ("" = CAD)
    std::wstring urlTemplate;                // chart endpoint
    std::wstring quoteUrlTemplate;           // batch fundamentals endpoint ("" = off)
    std::wstring searchUrlTemplate;          // symbol search endpoint ("" = off)
    std::wstring newsUrlTemplate;            // headlines endpoint ("" = off)
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
    bool   news      = false;
    float  insetX    = 0.0f;            // inset position, fractions of the free plot space
    float  insetY    = 0.0f;
    std::wstring selected;              // symbol
    std::wstring list;                  // active watch list ("" = default)
};

// Where stocktool.cfg lives: next to the exe when one is already there or
// the folder is writable (portable / dev tree), otherwise
// %APPDATA%\StockTool\stocktool.cfg (installed under Program Files).
std::wstring DefaultConfigPath();

// Writes the built-in default configuration to `path`.
bool WriteDefaultConfig(const std::wstring& path, std::wstring& err);

// Loads `path` into `out`, with the stocks of watch list `listName` ("" or
// unknown = the default list). On failure `err` describes the problem.
bool LoadConfig(const std::wstring& path, const std::wstring& listName, Config& out, std::wstring& err);

void LoadViewState(const std::wstring& path, ViewState& out);
bool SaveViewState(const std::wstring& path, const ViewState& state, std::wstring& err);

// Trims, upper-cases and checks a user-typed symbol (Yahoo notation, e.g.
// TD.TO, BRK-B, ^GSPTSE). On failure `err` says what is wrong.
bool NormalizeSymbol(std::wstring& symbol, std::wstring& err);

// Trims a display name and strips characters the INI format cannot hold.
std::wstring NormalizeName(const std::wstring& name, const std::wstring& fallback);

// Trims/upper-cases a 3-letter currency code; "" is allowed (no override).
bool NormalizeCurrency(std::wstring& code, std::wstring& err);

// Checks a watch-list name (letters, digits, space, - _; max 24 chars).
bool NormalizeListName(std::wstring& name, std::wstring& err);

// Writes/updates or deletes one ticker in the active list of `cfg` (its
// [stocks...] line) plus its [holdings], [alerts] and [currency] lines.
bool WriteStockEntry(const Config& cfg, const StockEntry& entry, std::wstring& err);
bool DeleteStockEntry(const Config& cfg, const std::wstring& symbol, std::wstring& err);

// Rewrites the active list's section in the order of `cfg.stocks`.
bool WriteStockOrder(const Config& cfg, std::wstring& err);

// Watch lists. Names are as shown; "" is the default list.
bool CreateList(const std::wstring& path, const std::wstring& name, std::wstring& err);
bool RenameList(const std::wstring& path, const std::wstring& from, const std::wstring& to, std::wstring& err);
bool DeleteList(const std::wstring& path, const std::wstring& name, std::wstring& err);

// Writes one key under [settings] (used for menu-driven settings).
bool WriteSetting(const std::wstring& path, const wchar_t* key, const std::wstring& value, std::wstring& err);

bool WriteHolding(const std::wstring& path, const std::wstring& symbol, const Holding& h, std::wstring& err);
bool WriteAlert(const std::wstring& path, const std::wstring& symbol, const Alert& a, std::wstring& err);
bool WriteCurrency(const std::wstring& path, const std::wstring& symbol, const std::wstring& code, std::wstring& err);

} // namespace st
