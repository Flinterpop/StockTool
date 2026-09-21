// Import of broker CSV exports (TD Direct Investing / WebBroker "Holdings"
// and "Activity" files) into holdings and transactions.
//
// The parser is header-driven: it finds the header row by its column names
// (Symbol, Quantity, Price, Trade Date, Transaction Type, Average Cost ...)
// so preamble lines, column order and extra columns do not matter.
#pragma once

#include "common.h"
#include "config.h"

#include <array>
#include <string>

namespace st {

constexpr size_t kMaxImportRows = 512;

// One buy/sell (activity file) or one position (holdings file).
struct ImportRow {
    std::wstring symbol;      // as the broker writes it, upper-cased (RY, TDB902)
    std::wstring market;      // Market/Exchange column, upper-cased ("" if absent)
    std::wstring name;        // Description column ("" if absent)
    bool         isHolding = false;
    Transaction  tx;          // holdings: date 0, qty = shares, price = average cost
};

struct ImportResult {
    std::array<ImportRow, kMaxImportRows> rows{};
    size_t count    = 0;
    size_t skipped  = 0;      // data rows that were not a buy, sell or position
    bool   holdings = false;  // the file was a holdings export (else activity)
};

// Parses the UTF-8 text of an export. False with `err` set when no usable
// header row is found. Rows beyond kMaxImportRows are dropped and counted
// in `skipped`.
bool ParseBrokerCsv(const std::string& utf8, ImportResult& out, std::wstring& err);

// What the symbol mapper knows: every symbol in every watch list, and the
// user's [symbol_map] overrides (broker symbol -> watch-list symbol).
struct ImportContext {
    std::array<std::wstring, kMaxKnownSymbols> known{};
    size_t knownCount = 0;
    std::array<SymbolMapEntry, kMaxSymbolMap> map{};
    size_t mapCount = 0;
};

// Maps a broker symbol to Yahoo notation: [symbol_map] first, then an exact
// or base-symbol match against the known list (RY + market CA -> RY.TO when
// RY.TO is in a list), then the market column (CA -> .TO, TSXV -> .V), else
// the symbol as given.
std::wstring MapBrokerSymbol(const std::wstring& symbol, const std::wstring& market, const ImportContext& ctx);

// Broker exports come as UTF-8, UTF-16 or the ANSI code page; this returns
// UTF-8 whatever the file used.
std::string DecodeTextFile(const std::string& bytes);

// Exposed for tests: tolerant number/date readers used by the parser.
bool ParseBrokerNumber(const std::wstring& text, double& value);   // "$1,234.50", "(12.5)", "-"
bool ParseBrokerDate(const std::wstring& text, int64_t& unixTime);  // ISO, MM/DD/YYYY, 12 Mar 2026 ...

// ---------------------------------------------------------------------------
// Import plan: parsed rows grouped per watch-list symbol, merged with the
// transactions already in the config so re-importing the same file is a no-op.

constexpr size_t kMaxImportSymbols = 64;

struct ImportItem {
    std::wstring broker;          // symbol as the file wrote it
    std::wstring symbol;          // mapped watch-list symbol
    std::wstring name;            // description from the file ("" if none)
    bool         known   = false; // already in some watch list
    bool         holding = false; // holdings row (else transactions)
    Holding      h;
    std::array<Transaction, kMaxTxPerSymbol> tx{};   // existing + new, to be written
    size_t       txCount  = 0;
    size_t       added    = 0;    // new transactions in `tx`
    size_t       dupes    = 0;    // file rows already present
    size_t       overflow = 0;    // rows dropped at kMaxTxPerSymbol
};

struct ImportPlan {
    std::array<ImportItem, kMaxImportSymbols> items{};
    size_t count    = 0;
    size_t dropped  = 0;          // rows for symbols beyond kMaxImportSymbols
    size_t skipped  = 0;          // from ImportResult
    bool   holdings = false;
};

// Groups `in` by mapped symbol using the lists and [symbol_map] in `cfgPath`.
void BuildImportPlan(const ImportResult& in, const std::wstring& cfgPath, ImportPlan& out);

// Multi-line (CRLF) summary for the confirmation dialog.
std::wstring DescribeImportPlan(const ImportPlan& plan);

// Writes the plan into the config. Unknown symbols are added to the active
// list of `cfg` when `addUnknown` is set (while it has room), else skipped.
// `written` = symbols whose data was stored. False with `err` on I/O failure.
bool ApplyImportPlan(const ImportPlan& plan, const Config& cfg, bool addUnknown, size_t& written, std::wstring& err);

} // namespace st
