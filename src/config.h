// Configuration file (INI format) loading and writing.
#pragma once

#include "common.h"

#include <windows.h>

namespace st {

struct StockEntry {
    std::wstring symbol;
    std::wstring name;
    std::wstring currency;   // override for the provider's currency ("" = provider)
    Holding      holding;    // manual position (ignored when transactions exist)
    Alert        alert;
    std::array<Transaction, kMaxTxPerSymbol> tx{};
    size_t       txCount = 0;
};

enum class ThemeMode { System, Light, Dark };

// Where headlines come from.
enum class NewsSource { None, Yahoo, Google };

// Second chart provider tried when the primary fails (daily bars only).
enum class FallbackProvider { None, Tmx };

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
    std::wstring benchmark;                  // index symbol for the benchmark overlay ("" = off)
    std::wstring urlTemplate;                // chart endpoint
    std::wstring quoteUrlTemplate;           // batch fundamentals endpoint ("" = off)
    std::wstring searchUrlTemplate;          // symbol search endpoint ("" = off)
    NewsSource   newsSource = NewsSource::Google;
    std::wstring newsUrlTemplate;            // Yahoo headlines endpoint ({symbol})
    std::wstring newsRssTemplate;            // Google News RSS endpoint ({query})
    FallbackProvider fallback = FallbackProvider::Tmx;
    std::wstring tmxUrl;                     // TMX Money GraphQL endpoint
    unsigned     closedRefreshSeconds = 900; // refresh cadence while every market is closed
    double       cash = 0.0;                 // uninvested cash in the active list, in portfolioCurrency
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
    bool   benchmark = false;
    bool   portfolio = false;           // portfolio-history view instead of the price chart
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

// [cash] <list>=<amount>: uninvested cash counted in that list's portfolio
// total. "" is the default list; 0 removes the line.
bool WriteCash(const std::wstring& path, const std::wstring& list, double amount, std::wstring& err);
double ReadCash(const std::wstring& path, const std::wstring& list);
bool WriteAlert(const std::wstring& path, const std::wstring& symbol, const Alert& a, std::wstring& err);
bool WriteCurrency(const std::wstring& path, const std::wstring& symbol, const std::wstring& code, std::wstring& err);

// Every symbol of every watch list in the file (for the broker import's
// symbol mapping). Returns how many were written to `out`.
size_t ReadAllSymbols(const std::wstring& path, std::wstring* out, size_t max);

// [symbol_map] entries: broker symbol = watch-list symbol.
size_t ReadSymbolMap(const std::wstring& path, SymbolMapEntry* out, size_t max);

// Fills e.tx/e.txCount from the [transactions] lines of e.symbol.
void ReadTransactions(const std::wstring& path, StockEntry& e);

// Replaces the [transactions] lines for `symbol` (SYMBOL.1=date,qty,price ...).
bool WriteTransactions(const std::wstring& path, const std::wstring& symbol,
                       const Transaction* tx, size_t count, std::wstring& err);

} // namespace st
