// Broker CSV import: tolerant CSV reader, header detection, symbol mapping.
#include "brokerimport.h"

#include "textfmt.h"

#include <windows.h>

#include <cassert>
#include <cmath>
#include <cwctype>

namespace st {

namespace {

constexpr size_t kMaxCols       = 64;
constexpr size_t kMaxLines      = 4096;
constexpr size_t kMaxHeaderScan = 64;    // preamble lines allowed before the header

using Row = std::array<std::wstring, kMaxCols>;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) { return {}; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) { return {}; }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return {}; }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::iswspace(s[b])) { ++b; }
    while (e > b && std::iswspace(s[e - 1])) { --e; }
    return s.substr(b, e - b);
}

std::wstring Upper(std::wstring s) {
    for (wchar_t& c : s) { c = static_cast<wchar_t>(std::towupper(c)); }
    return s;
}

// Lower-cased letters and digits only: "Trade Date" -> "tradedate".
std::wstring Key(const std::wstring& s) {
    std::wstring out;
    for (const wchar_t c : s) {
        if (std::iswalnum(c)) { out += static_cast<wchar_t>(std::towlower(c)); }
    }
    return out;
}

// Splits one CSV record (quotes, "" escapes). Returns the column count.
size_t SplitCsv(const std::wstring& line, Row& out) {
    size_t cols = 0;
    std::wstring cell;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if (quoted) {
            if (c == L'"' && i + 1 < line.size() && line[i + 1] == L'"') { cell += L'"'; ++i; }
            else if (c == L'"') { quoted = false; }
            else { cell += c; }
        } else if (c == L'"') {
            quoted = true;
        } else if (c == L',') {
            if (cols < kMaxCols) { out[cols] = Trim(cell); }
            ++cols;
            cell.clear();
        } else {
            cell += c;
        }
    }
    if (cols < kMaxCols) { out[cols] = Trim(cell); }
    ++cols;
    return cols < kMaxCols ? cols : kMaxCols;
}

// Which columns hold what. -1 = absent.
struct Columns {
    int symbol = -1;
    int market = -1;
    int name   = -1;
    int qty    = -1;
    int price  = -1;   // trade price or average cost
    int book   = -1;   // book value (holdings without an average-cost column)
    int date   = -1;
    int type   = -1;
    int amount = -1;   // net amount (activity price fallback)
    int dateRank = 99; // lower = better date column
};

// Candidate header names, best first where several may appear.
bool IsSymbolCol(const std::wstring& k)  { return k == L"symbol" || k == L"ticker" || k == L"tickersymbol"; }
bool IsMarketCol(const std::wstring& k)  { return k == L"market" || k == L"exchange" || k == L"mkt"; }
bool IsNameCol(const std::wstring& k)    { return k == L"description" || k == L"name" || k == L"security" || k == L"securityname"; }
bool IsQtyCol(const std::wstring& k)     { return k == L"quantity" || k == L"qty" || k == L"shares" || k == L"units"; }
bool IsPriceCol(const std::wstring& k)   { return k == L"price" || k == L"averagecost" || k == L"avgcost" || k == L"averageprice" || k == L"unitcost" || k == L"tradeprice"; }
bool IsBookCol(const std::wstring& k)    { return k == L"bookvalue" || k == L"bookcost" || k == L"totalcost" || k == L"costbasis"; }
bool IsTypeCol(const std::wstring& k)    { return k == L"transactiontype" || k == L"type" || k == L"action" || k == L"activity" || k == L"activitytype" || k == L"transaction"; }
bool IsAmountCol(const std::wstring& k)  { return k == L"netamount" || k == L"amount" || k == L"net"; }

int DateRank(const std::wstring& k) {
    if (k == L"tradedate") { return 0; }
    if (k == L"date" || k == L"transactiondate" || k == L"activitydate") { return 1; }
    if (k == L"settlementdate" || k == L"settledate") { return 2; }
    return -1;
}

Columns Classify(const Row& row, size_t cols) {
    Columns c;
    for (size_t i = 0; i < cols; ++i) {
        const std::wstring k = Key(row[i]);
        const int idx = static_cast<int>(i);
        if (c.symbol < 0 && IsSymbolCol(k))      { c.symbol = idx; }
        else if (c.market < 0 && IsMarketCol(k)) { c.market = idx; }
        else if (c.name < 0 && IsNameCol(k))     { c.name = idx; }
        else if (c.qty < 0 && IsQtyCol(k))       { c.qty = idx; }
        else if (c.price < 0 && IsPriceCol(k))   { c.price = idx; }
        else if (c.book < 0 && IsBookCol(k))     { c.book = idx; }
        else if (c.type < 0 && IsTypeCol(k))     { c.type = idx; }
        else if (c.amount < 0 && IsAmountCol(k)) { c.amount = idx; }
        else {
            const int rank = DateRank(k);
            if (rank >= 0 && rank < c.dateRank) { c.date = idx; c.dateRank = rank; }
        }
    }
    return c;
}

const std::wstring& Cell(const Row& row, size_t cols, int idx) {
    static const std::wstring kEmpty;
    return (idx >= 0 && static_cast<size_t>(idx) < cols) ? row[static_cast<size_t>(idx)] : kEmpty;
}

// Other = the text says nothing about the action; Ignore = it names a
// non-trade (dividend, interest, fee, transfer ...).
enum class Action { Buy, Sell, Other, Ignore };

Action ActionOf(const std::wstring& text) {
    const std::wstring k = Key(text);
    if (k.empty()) { return Action::Other; }
    // Reinvested dividends add shares at a price, so they are buys.
    if (k.find(L"reinvest") != std::wstring::npos || k.find(L"drip") != std::wstring::npos) { return Action::Buy; }
    if (k.find(L"sell") != std::wstring::npos || k.find(L"sold") != std::wstring::npos) { return Action::Sell; }
    if (k.find(L"buy") != std::wstring::npos || k.find(L"bought") != std::wstring::npos ||
        k.find(L"purchase") != std::wstring::npos) {
        return Action::Buy;
    }
    static const wchar_t* const kNonTrade[] = { L"div", L"interest", L"contribution", L"withdraw", L"fee",
                                                L"transfer", L"deposit", L"tax", L"journal", L"exchange",
                                                L"cash", L"rebate", L"distribution", L"return" };
    for (const wchar_t* word : kNonTrade) {
        if (k.find(word) != std::wstring::npos) { return Action::Ignore; }
    }
    return Action::Other;
}

int MonthFromName(const std::wstring& upper) {
    static const wchar_t* const kNames[12] = { L"JAN", L"FEB", L"MAR", L"APR", L"MAY", L"JUN",
                                               L"JUL", L"AUG", L"SEP", L"OCT", L"NOV", L"DEC" };
    for (int m = 0; m < 12; ++m) {
        if (upper.compare(0, 3, kNames[m]) == 0) { return m + 1; }
    }
    return 0;
}

bool MakeDate(int y, int m, int d, int64_t& unixTime) {
    std::array<wchar_t, 16> iso{};
    swprintf_s(iso.data(), iso.size(), L"%04d-%02d-%02d", y, m, d);
    return ParseIsoDate(iso.data(), unixTime);
}

} // namespace

bool ParseBrokerNumber(const std::wstring& text, double& value) {
    std::wstring clean;
    bool negative = false;
    for (const wchar_t c : text) {
        if (std::iswdigit(c) || c == L'.') { clean += c; }
        else if (c == L'-' || c == L'(') { negative = true; }
        // '$', ',', ')', spaces and currency codes are dropped
    }
    if (clean.empty() || clean == L".") { return false; }
    wchar_t* end = nullptr;
    const double v = wcstod(clean.c_str(), &end);
    if (end == nullptr || *end != L'\0' || !std::isfinite(v)) { return false; }
    value = negative ? -v : v;
    return true;
}

bool ParseBrokerDate(const std::wstring& text, int64_t& unixTime) {
    const std::wstring t = Upper(Trim(text));
    if (t.empty()) { return false; }
    int a = 0;
    int b = 0;
    int c = 0;
    std::array<wchar_t, 16> word{};
    // 2026-03-12 or 2026/03/12 (time of day, if any, is ignored)
    if (swscanf_s(t.c_str(), L"%d-%d-%d", &a, &b, &c) == 3 && a > 31) { return MakeDate(a, b, c, unixTime); }
    if (swscanf_s(t.c_str(), L"%d/%d/%d", &a, &b, &c) == 3) {
        if (a > 31) { return MakeDate(a, b, c, unixTime); }                 // YYYY/MM/DD
        if (a > 12) { return MakeDate(c, b, a, unixTime); }                 // DD/MM/YYYY
        return MakeDate(c, a, b, unixTime);                                 // MM/DD/YYYY (North American default)
    }
    // 12 MAR 2026 / 12-MAR-2026
    if (swscanf_s(t.c_str(), L"%d%*[ -]%3[A-Z]%*[ -]%d", &a, word.data(), static_cast<unsigned>(word.size()), &c) == 3) {
        const int m = MonthFromName(word.data());
        return m > 0 && MakeDate(c, m, a, unixTime);
    }
    // MAR 12, 2026 / MARCH 12 2026
    if (swscanf_s(t.c_str(), L"%15[A-Z] %d%*[, ]%d", word.data(), static_cast<unsigned>(word.size()), &a, &c) == 3) {
        const int m = MonthFromName(word.data());
        return m > 0 && MakeDate(c, m, a, unixTime);
    }
    return false;
}

std::string DecodeTextFile(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const size_t chars = (bytes.size() - 2) / 2;
        std::wstring wide(chars, L'\0');
        memcpy(wide.data(), bytes.data() + 2, chars * sizeof(wchar_t));
        return WideToUtf8(wide);
    }
    std::string body = bytes;
    if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) { body.erase(0, 3); }
    if (body.empty()) { return body; }
    // Valid UTF-8 as is; otherwise it is the ANSI code page (Excel's default).
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.data(), static_cast<int>(body.size()), nullptr, 0) > 0) {
        return body;
    }
    const int n = MultiByteToWideChar(CP_ACP, 0, body.data(), static_cast<int>(body.size()), nullptr, 0);
    if (n <= 0) { return body; }
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, body.data(), static_cast<int>(body.size()), wide.data(), n);
    return WideToUtf8(wide);
}

bool ParseBrokerCsv(const std::string& utf8, ImportResult& out, std::wstring& err) {
    out = ImportResult{};
    const std::wstring text = Utf8ToWide(utf8);
    Columns cols;
    bool haveHeader = false;
    size_t pos = 0;
    size_t lineNo = 0;
    for (size_t guard = 0; guard < kMaxLines && pos <= text.size(); ++guard) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) { eol = text.size(); }
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == L'\r') { line.pop_back(); }
        if (Trim(line).empty()) { continue; }
        ++lineNo;

        Row row;
        const size_t n = SplitCsv(line, row);
        if (!haveHeader) {
            if (lineNo > kMaxHeaderScan) { break; }
            const Columns c = Classify(row, n);
            if (c.symbol < 0 || c.qty < 0) { continue; }         // preamble ("As of ...", account name)
            cols = c;
            haveHeader = true;
            // An activity file dates and types its rows; a holdings file has neither.
            out.holdings = (c.date < 0 && c.type < 0) && (c.price >= 0 || c.book >= 0);
            continue;
        }

        ImportRow r;
        r.symbol = Upper(Cell(row, n, cols.symbol));
        r.market = Upper(Cell(row, n, cols.market));
        r.name   = Cell(row, n, cols.name);
        double qty = 0.0;
        if (r.symbol.empty() || !ParseBrokerNumber(Cell(row, n, cols.qty), qty) || qty == 0.0) {
            ++out.skipped;        // totals, cash lines, blank rows
            continue;
        }
        double price = 0.0;
        const bool havePrice = ParseBrokerNumber(Cell(row, n, cols.price), price) && price > 0.0;
        if (out.holdings) {
            double book = 0.0;
            if (!havePrice && ParseBrokerNumber(Cell(row, n, cols.book), book) && book > 0.0) {
                price = book / std::fabs(qty);
            } else if (!havePrice) {
                ++out.skipped;
                continue;
            }
            r.isHolding = true;
            r.tx.qty    = std::fabs(qty);
            r.tx.price  = price;
        } else {
            // The action comes from the type column, else the description, else the sign of the quantity.
            Action act = ActionOf(Cell(row, n, cols.type));
            if (act == Action::Other) { act = ActionOf(r.name); }
            if (act == Action::Other) { act = qty < 0.0 ? Action::Sell : Action::Buy; }
            double amount = 0.0;
            if (!havePrice && ParseBrokerNumber(Cell(row, n, cols.amount), amount) && amount != 0.0) {
                price = std::fabs(amount) / std::fabs(qty);   // includes commission
            } else if (!havePrice) {
                act = Action::Ignore;                          // a share transfer, not a trade
            }
            if (act == Action::Ignore || !ParseBrokerDate(Cell(row, n, cols.date), r.tx.date)) {
                ++out.skipped;    // dividends, interest, fees, transfers, undated rows
                continue;
            }
            r.tx.qty   = (act == Action::Sell ? -1.0 : 1.0) * std::fabs(qty);
            r.tx.price = price;
        }
        if (out.count >= kMaxImportRows) {
            ++out.skipped;
            continue;
        }
        out.rows[out.count] = r;
        ++out.count;
    }
    if (!haveHeader) {
        err = L"No header row with Symbol and Quantity columns was found. Export the file again from WebBroker as CSV.";
        return false;
    }
    assert(out.count <= kMaxImportRows);
    return true;
}

namespace {

std::wstring Base(const std::wstring& symbol) {
    const size_t dot = symbol.find(L'.');
    return dot == std::wstring::npos ? symbol : symbol.substr(0, dot);
}

bool IsCanadianMarket(const std::wstring& m) {
    return m == L"CA" || m == L"CAD" || m == L"CDN" || m == L"CAN" || m == L"CANADA" || m == L"TSX" ||
           m == L"TSE" || m == L"TOR" || m == L"TORONTO" || m == L"TSXV" || m == L"TSX-V" || m == L"CVE" ||
           m == L"VENTURE" || m == L"CNQ" || m == L"CSE" || m == L"NEO";
}

bool IsVentureMarket(const std::wstring& m) {
    return m == L"TSXV" || m == L"TSX-V" || m == L"CVE" || m == L"VENTURE";
}

} // namespace

std::wstring MapBrokerSymbol(const std::wstring& symbol, const std::wstring& market, const ImportContext& ctx) {
    assert(ctx.knownCount <= kMaxKnownSymbols && ctx.mapCount <= kMaxSymbolMap);
    const std::wstring sym = Upper(Trim(symbol));
    if (sym.empty()) { return sym; }
    for (size_t i = 0; i < ctx.mapCount; ++i) {
        if (_wcsicmp(ctx.map[i].from.c_str(), sym.c_str()) == 0) { return Upper(ctx.map[i].to); }
    }
    const std::wstring mkt = Upper(Trim(market));
    const bool canadian = IsCanadianMarket(mkt);
    // Exact watch-list match first (RY.TO written that way, or a US symbol).
    for (size_t i = 0; i < ctx.knownCount; ++i) {
        if (_wcsicmp(ctx.known[i].c_str(), sym.c_str()) == 0 && !canadian) { return Upper(ctx.known[i]); }
    }
    // Then a symbol whose base matches (RY -> RY.TO), unless the market says US.
    const bool us = mkt == L"US" || mkt == L"USA" || mkt == L"USD" || mkt == L"NYSE" || mkt == L"NASDAQ" ||
                    mkt == L"NAS" || mkt == L"NYS" || mkt == L"AMEX" || mkt == L"ARCA";
    if (!us && sym.find(L'.') == std::wstring::npos) {
        for (size_t i = 0; i < ctx.knownCount; ++i) {
            if (_wcsicmp(Base(ctx.known[i]).c_str(), sym.c_str()) == 0) { return Upper(ctx.known[i]); }
        }
    }
    for (size_t i = 0; i < ctx.knownCount; ++i) {
        if (_wcsicmp(ctx.known[i].c_str(), sym.c_str()) == 0) { return Upper(ctx.known[i]); }
    }
    if (canadian && sym.find(L'.') == std::wstring::npos) {
        return sym + (IsVentureMarket(mkt) ? L".V" : L".TO");
    }
    return sym;
}

// ---------------------------------------------------------------------------
// Plan

namespace {

bool SameTx(const Transaction& a, const Transaction& b) {
    return a.date == b.date && std::fabs(a.qty - b.qty) < 1e-6 && std::fabs(a.price - b.price) < 0.005;
}

ImportItem* FindOrAddItem(ImportPlan& plan, const ImportRow& row, const std::wstring& mapped, const ImportContext& ctx) {
    for (size_t i = 0; i < plan.count; ++i) {
        if (_wcsicmp(plan.items[i].symbol.c_str(), mapped.c_str()) == 0) { return &plan.items[i]; }
    }
    if (plan.count >= kMaxImportSymbols) { return nullptr; }
    ImportItem& it = plan.items[plan.count];
    it = ImportItem{};
    it.broker = row.symbol;
    it.symbol = mapped;
    it.name   = row.name;
    for (size_t k = 0; k < ctx.knownCount && !it.known; ++k) {
        it.known = _wcsicmp(ctx.known[k].c_str(), mapped.c_str()) == 0;
    }
    ++plan.count;
    return &it;
}

std::wstring Plural(size_t n, const wchar_t* word) {
    return std::to_wstring(n) + L" " + word + (n == 1 ? L"" : L"s");
}

} // namespace

void BuildImportPlan(const ImportResult& in, const std::wstring& cfgPath, ImportPlan& out) {
    assert(in.count <= kMaxImportRows);
    out = ImportPlan{};
    out.skipped  = in.skipped;
    out.holdings = in.holdings;
    ImportContext ctx;
    ctx.knownCount = ReadAllSymbols(cfgPath, ctx.known.data(), ctx.known.size());
    ctx.mapCount   = ReadSymbolMap(cfgPath, ctx.map.data(), ctx.map.size());
    for (size_t i = 0; i < in.count; ++i) {
        const ImportRow& row = in.rows[i];
        const std::wstring mapped = MapBrokerSymbol(row.symbol, row.market, ctx);
        if (mapped.empty()) { continue; }
        ImportItem* it = FindOrAddItem(out, row, mapped, ctx);
        if (it == nullptr) { ++out.dropped; continue; }
        if (row.isHolding) {
            it->holding = true;
            it->h.qty   = row.tx.qty;
            it->h.cost  = row.tx.price;
            continue;
        }
        if (it->txCount == 0 && it->added == 0 && it->dupes == 0) {
            StockEntry existing;      // what the config already holds for this symbol
            existing.symbol = mapped;
            ReadTransactions(cfgPath, existing);
            it->tx      = existing.tx;
            it->txCount = existing.txCount;
        }
        bool dup = false;
        for (size_t k = 0; k < it->txCount && !dup; ++k) { dup = SameTx(it->tx[k], row.tx); }
        if (dup) { ++it->dupes; continue; }
        if (it->txCount >= kMaxTxPerSymbol) { ++it->overflow; continue; }
        it->tx[it->txCount] = row.tx;
        ++it->txCount;
        ++it->added;
    }
    assert(out.count <= kMaxImportSymbols);
}

std::wstring DescribeImportPlan(const ImportPlan& plan) {
    assert(plan.count <= kMaxImportSymbols);
    const wchar_t* const eol = L"\r\n";
    std::wstring s;
    size_t unknown = 0;
    for (size_t i = 0; i < plan.count; ++i) { if (!plan.items[i].known) { ++unknown; } }
    s += plan.holdings ? L"Holdings export: " : L"Activity export: ";
    s += Plural(plan.count, L"symbol");
    if (plan.skipped > 0) { s += L", " + Plural(plan.skipped, L"row") + L" ignored (dividends, interest, fees, totals)"; }
    s += L".";
    s += eol;
    if (unknown > 0) {
        s += Plural(unknown, L"symbol") + (unknown == 1 ? L" is" : L" are") +
             L" not in any watch list (marked *): tick the box below to add them, or map them in [symbol_map].";
        s += eol;
    }
    s += eol;
    for (size_t i = 0; i < plan.count; ++i) {
        const ImportItem& it = plan.items[i];
        s += (it.known ? L"   " : L" * ") + it.broker;
        if (_wcsicmp(it.broker.c_str(), it.symbol.c_str()) != 0) { s += L" -> " + it.symbol; }
        s += L":  ";
        if (it.holding) {
            s += FormatMoney(it.h.qty) + L" sh at " + FormatPrice(it.h.cost);
        } else {
            s += Plural(it.added, L"new transaction");
            if (it.dupes > 0)    { s += L", " + std::to_wstring(it.dupes) + L" already recorded"; }
            if (it.overflow > 0) { s += L", " + std::to_wstring(it.overflow) + L" dropped (limit " + std::to_wstring(kMaxTxPerSymbol) + L")"; }
        }
        if (!it.name.empty()) { s += L"   (" + it.name + L")"; }
        s += eol;
    }
    if (plan.dropped > 0) {
        s += eol;
        s += Plural(plan.dropped, L"row") + L" for further symbols dropped (limit " +
             std::to_wstring(kMaxImportSymbols) + L" symbols per import).";
        s += eol;
    }
    return s;
}

bool ApplyImportPlan(const ImportPlan& plan, const Config& cfg, bool addUnknown, size_t& written, std::wstring& err) {
    assert(!cfg.path.empty());
    assert(plan.count <= kMaxImportSymbols);
    written = 0;
    size_t room = cfg.stockCount < kMaxStocks ? kMaxStocks - cfg.stockCount : 0;
    for (size_t i = 0; i < plan.count; ++i) {
        const ImportItem& it = plan.items[i];
        if (!it.known) {
            if (!addUnknown || room == 0) { continue; }
            StockEntry e;
            e.symbol = it.symbol;
            e.name   = NormalizeName(it.name, it.symbol);
            if (!WriteStockEntry(cfg, e, err)) { return false; }
            --room;
        }
        if (it.holding) {
            if (!WriteHolding(cfg.path, it.symbol, it.h, err)) { return false; }
        } else if (it.added > 0) {
            if (!WriteTransactions(cfg.path, it.symbol, it.tx.data(), it.txCount, err)) { return false; }
        }
        ++written;
    }
    assert(written <= plan.count);
    return true;
}

} // namespace st
