#include "config.h"

#include <cassert>
#include <cwchar>
#include <cwctype>

namespace st {
namespace {

constexpr wchar_t kDefaultUrlTemplate[] =
    L"https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    L"?range={range}&interval={interval}&includePrePost=false";

constexpr wchar_t kDefaultQuoteUrlTemplate[] =
    L"https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}";

constexpr char kDefaultConfigText[] =
    "; StockTool configuration.\r\n"
    "; Symbols use Yahoo Finance notation: TSX = .TO, TSX Venture = .V,\r\n"
    "; NYSE/Nasdaq = bare symbol (e.g. AAPL). Keep this file ASCII.\r\n"
    "\r\n"
    "[settings]\r\n"
    "refresh_seconds=60\r\n"
    "default_range=1Y\r\n"
    "; Trend inset in the chart corner: 1Y or 5Y\r\n"
    "inset_range=1Y\r\n"
    "; system | light | dark\r\n"
    "theme=system\r\n"
    "start_minimized=0\r\n"
    "minimize_to_tray=0\r\n"
    "url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    "?range={range}&interval={interval}&includePrePost=false\r\n"
    "; Fundamentals (market cap, P/E, yield). Leave empty to disable.\r\n"
    "quote_url_template=https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}\r\n"
    "\r\n"
    "[stocks]\r\n"
    "RY.TO=Royal Bank of Canada\r\n"
    "DOL.TO=Dollarama Inc.\r\n"
    "BMO.TO=Bank of Montreal\r\n"
    "BNS.TO=Bank of Nova Scotia\r\n"
    "BN.TO=Brookfield Corporation\r\n"
    "\r\n"
    "; symbol=shares,average cost   (written by Ticker > Holding...)\r\n"
    "[holdings]\r\n"
    "\r\n"
    "; symbol=above,below   (written by Ticker > Alerts...; 0 = unset)\r\n"
    "[alerts]\r\n";

constexpr size_t kSectionBufChars = 8192;
constexpr wchar_t kSettings[] = L"settings";
constexpr wchar_t kStocks[]   = L"stocks";
constexpr wchar_t kHoldings[] = L"holdings";
constexpr wchar_t kAlerts[]   = L"alerts";
constexpr wchar_t kState[]    = L"state";

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::iswspace(s[b]) != 0) { ++b; }
    while (e > b && std::iswspace(s[e - 1]) != 0) { --e; }
    assert(b <= e);
    return s.substr(b, e - b);
}

std::wstring ReadString(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                        const wchar_t* def) {
    std::array<wchar_t, 1024> buf{};
    GetPrivateProfileStringW(section, key, def, buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
    return Trim(buf.data());
}

bool ReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(section, key, def ? 1 : 0, path.c_str()) != 0;
}

bool WriteString(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                 const wchar_t* value, std::wstring& err) {
    if (!WritePrivateProfileStringW(section, key, value, path.c_str())) {
        err = L"Could not write " + path;
        return false;
    }
    return true;
}

size_t RangeIndexFromLabel(const std::wstring& label, size_t fallback) {
    for (size_t i = 0; i < kRanges.size(); ++i) {
        if (_wcsicmp(label.c_str(), kRanges[i].label) == 0) { return i; }
    }
    return fallback;
}

// Splits one "key=value" entry; returns false for comments / malformed lines.
bool SplitEntry(const wchar_t* entry, StockEntry& out) {
    assert(entry != nullptr);
    const std::wstring line(entry);
    if (line.empty() || line[0] == L';' || line[0] == L'#') { return false; }
    const size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) { return false; }
    out.symbol = Trim(line.substr(0, eq));
    out.name   = Trim(line.substr(eq + 1));
    if (out.name.empty()) { out.name = out.symbol; }
    return !out.symbol.empty();
}

// Parses "a,b" into two doubles (missing/blank = 0).
void ParsePair(const std::wstring& text, double& a, double& b) {
    a = 0.0;
    b = 0.0;
    if (text.empty()) { return; }
    const size_t comma = text.find(L',');
    a = _wtof(text.substr(0, comma).c_str());
    if (comma != std::wstring::npos) { b = _wtof(text.substr(comma + 1).c_str()); }
}

std::wstring FormatPair(double a, double b) {
    std::array<wchar_t, 64> buf{};
    swprintf_s(buf.data(), buf.size(), L"%.4f,%.4f", a, b);
    return buf.data();
}

bool LoadStocks(const std::wstring& path, Config& out, std::wstring& err) {
    std::array<wchar_t, kSectionBufChars> buf{};
    const DWORD n = GetPrivateProfileSectionW(kStocks, buf.data(),
                                              static_cast<DWORD>(buf.size()), path.c_str());
    if (n >= buf.size() - 2) {
        err = L"[stocks] section is too large";
        return false;
    }
    out.stockCount = 0;
    size_t pos = 0;
    // Each entry is NUL-terminated; the list ends with an extra NUL.
    for (size_t guard = 0; guard < kSectionBufChars && pos < n; ++guard) {
        const wchar_t* entry = buf.data() + pos;
        const size_t len = wcsnlen_s(entry, buf.size() - pos);
        if (len == 0) { break; }
        pos += len + 1;
        StockEntry e;
        if (!SplitEntry(entry, e)) { continue; }
        if (out.stockCount >= kMaxStocks) { break; }  // silently cap
        ParsePair(ReadString(path, kHoldings, e.symbol.c_str(), L""), e.holding.qty, e.holding.cost);
        ParsePair(ReadString(path, kAlerts, e.symbol.c_str(), L""), e.alert.above, e.alert.below);
        out.stocks[out.stockCount] = e;
        ++out.stockCount;
    }
    if (out.stockCount == 0) {
        err = L"No stocks listed under [stocks] in " + path;
        return false;
    }
    assert(out.stockCount <= kMaxStocks);
    return true;
}

} // namespace

std::wstring DefaultConfigPath() {
    std::array<wchar_t, MAX_PATH> exe{};
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    assert(n > 0 && n < exe.size());
    std::wstring path(exe.data(), n);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) { path.resize(slash + 1); }
    path += L"stocktool.cfg";
    return path;
}

bool WriteDefaultConfig(const std::wstring& path, std::wstring& err) {
    assert(!path.empty());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"Cannot create " + path;
        return false;
    }
    const DWORD want = static_cast<DWORD>(sizeof(kDefaultConfigText) - 1);
    DWORD written = 0;
    const BOOL ok = WriteFile(h, kDefaultConfigText, want, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != want) {
        err = L"Cannot write " + path;
        return false;
    }
    return true;
}

bool LoadConfig(const std::wstring& path, Config& out, std::wstring& err) {
    assert(!path.empty());
    out = Config{};
    out.path = path;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"Config file not found: " + path;
        return false;
    }

    const UINT refresh = GetPrivateProfileIntW(kSettings, L"refresh_seconds", 60, path.c_str());
    out.refreshSeconds = (refresh < 10u) ? 10u : (refresh > 3600u ? 3600u : refresh);
    out.defaultRange   = RangeIndexFromLabel(ReadString(path, kSettings, L"default_range", L"1Y"), kRange1Y);
    out.insetRange     = RangeIndexFromLabel(ReadString(path, kSettings, L"inset_range", L"1Y"), kRange1Y);
    if (out.insetRange != kRange1Y && out.insetRange != kRange5Y) { out.insetRange = kRange1Y; }

    const std::wstring theme = ReadString(path, kSettings, L"theme", L"system");
    if (_wcsicmp(theme.c_str(), L"light") == 0)     { out.theme = ThemeMode::Light; }
    else if (_wcsicmp(theme.c_str(), L"dark") == 0) { out.theme = ThemeMode::Dark; }
    else                                            { out.theme = ThemeMode::System; }
    out.startMinimized = ReadBool(path, kSettings, L"start_minimized", false);
    out.minimizeToTray = ReadBool(path, kSettings, L"minimize_to_tray", false);

    out.urlTemplate = ReadString(path, kSettings, L"url_template", kDefaultUrlTemplate);
    if (out.urlTemplate.find(L"{symbol}") == std::wstring::npos) {
        err = L"url_template must contain {symbol}";
        return false;
    }
    out.quoteUrlTemplate = ReadString(path, kSettings, L"quote_url_template", kDefaultQuoteUrlTemplate);
    if (!out.quoteUrlTemplate.empty() && out.quoteUrlTemplate.find(L"{symbols}") == std::wstring::npos) {
        err = L"quote_url_template must contain {symbols} (or be empty)";
        return false;
    }

    if (!LoadStocks(path, out, err)) { return false; }
    assert(out.defaultRange < kRanges.size());
    return true;
}

void LoadViewState(const std::wstring& path, ViewState& out) {
    out = ViewState{};
    const std::wstring win = ReadString(path, kState, L"window", L"");
    int l = 0;
    int t = 0;
    int w = 0;
    int h = 0;
    if (swscanf_s(win.c_str(), L"%d,%d,%d,%d", &l, &t, &w, &h) == 4 && w > 200 && h > 150) {
        out.window    = { l, t, l + w, t + h };
        out.hasWindow = true;
    }
    out.maximized = ReadBool(path, kState, L"maximized", false);
    out.range     = RangeIndexFromLabel(ReadString(path, kState, L"range", L""), kRanges.size());
    out.candles   = ReadBool(path, kState, L"candles", false);
    out.compare   = ReadBool(path, kState, L"compare", false);
    out.sma20     = ReadBool(path, kState, L"sma20", false);
    out.sma50     = ReadBool(path, kState, L"sma50", false);
    out.bollinger = ReadBool(path, kState, L"bollinger", false);
    out.rsi       = ReadBool(path, kState, L"rsi", false);
    out.inset     = ReadBool(path, kState, L"inset", true);
    out.selected  = ReadString(path, kState, L"selected", L"");
}

bool SaveViewState(const std::wstring& path, const ViewState& s, std::wstring& err) {
    assert(!path.empty());
    std::array<wchar_t, 96> win{};
    swprintf_s(win.data(), win.size(), L"%ld,%ld,%ld,%ld", s.window.left, s.window.top,
               s.window.right - s.window.left, s.window.bottom - s.window.top);
    const wchar_t* range = (s.range < kRanges.size()) ? kRanges[s.range].label : L"";
    const struct { const wchar_t* key; std::wstring value; } items[] = {
        { L"window",    s.hasWindow ? win.data() : L"" },
        { L"maximized", s.maximized ? L"1" : L"0" },
        { L"range",     range },
        { L"candles",   s.candles ? L"1" : L"0" },
        { L"compare",   s.compare ? L"1" : L"0" },
        { L"sma20",     s.sma20 ? L"1" : L"0" },
        { L"sma50",     s.sma50 ? L"1" : L"0" },
        { L"bollinger", s.bollinger ? L"1" : L"0" },
        { L"rsi",       s.rsi ? L"1" : L"0" },
        { L"inset",     s.inset ? L"1" : L"0" },
        { L"selected",  s.selected },
    };
    for (const auto& it : items) {
        if (!WriteString(path, kState, it.key, it.value.c_str(), err)) { return false; }
    }
    return true;
}

bool NormalizeSymbol(std::wstring& symbol, std::wstring& err) {
    constexpr size_t kMaxSymbolLen = 31;
    symbol = Trim(symbol);
    if (symbol.empty()) {
        err = L"Enter a symbol";
        return false;
    }
    if (symbol.size() > kMaxSymbolLen) {
        err = L"Symbol is too long";
        return false;
    }
    for (size_t i = 0; i < symbol.size(); ++i) {
        wchar_t& c = symbol[i];
        c = static_cast<wchar_t>(std::towupper(c));
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                        c == L'.' || c == L'-' || c == L'^' || c == L'=';
        if (!ok) {
            err = L"Symbols may only contain letters, digits, . - ^ =";
            return false;
        }
    }
    assert(!symbol.empty());
    return true;
}

std::wstring NormalizeName(const std::wstring& name, const std::wstring& fallback) {
    constexpr size_t kMaxNameLen = 63;
    std::wstring out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size() && i < 4096; ++i) {
        const wchar_t c = name[i];
        if (c == L'\r' || c == L'\n' || c == L'\0') { continue; }
        out += c;
    }
    out = Trim(out);
    if (out.size() > kMaxNameLen) { out.resize(kMaxNameLen); }
    return out.empty() ? fallback : out;
}

bool WriteStockEntry(const std::wstring& path, const StockEntry& entry, std::wstring& err) {
    assert(!path.empty());
    assert(!entry.symbol.empty());
    if (!WriteString(path, kStocks, entry.symbol.c_str(), entry.name.c_str(), err)) { return false; }
    if (!WriteHolding(path, entry.symbol, entry.holding, err)) { return false; }
    return WriteAlert(path, entry.symbol, entry.alert, err);
}

bool DeleteStockEntry(const std::wstring& path, const std::wstring& symbol, std::wstring& err) {
    assert(!path.empty());
    assert(!symbol.empty());
    if (!WriteString(path, kStocks, symbol.c_str(), nullptr, err)) { return false; }
    if (!WriteString(path, kHoldings, symbol.c_str(), nullptr, err)) { return false; }
    return WriteString(path, kAlerts, symbol.c_str(), nullptr, err);
}

bool WriteStockOrder(const std::wstring& path, const Config& cfg, std::wstring& err) {
    assert(cfg.stockCount > 0 && cfg.stockCount <= kMaxStocks);
    std::wstring block;
    for (size_t i = 0; i < cfg.stockCount; ++i) {
        block += cfg.stocks[i].symbol + L"=" + cfg.stocks[i].name;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (!WritePrivateProfileSectionW(kStocks, block.c_str(), path.c_str())) {
        err = L"Could not rewrite [stocks] in " + path;
        return false;
    }
    return true;
}

bool WriteSetting(const std::wstring& path, const wchar_t* key, const std::wstring& value, std::wstring& err) {
    assert(key != nullptr);
    return WriteString(path, kSettings, key, value.c_str(), err);
}

bool WriteHolding(const std::wstring& path, const std::wstring& symbol, const Holding& h, std::wstring& err) {
    assert(!symbol.empty());
    const bool unset = (h.qty == 0.0 && h.cost == 0.0);
    return WriteString(path, kHoldings, symbol.c_str(),
                       unset ? nullptr : FormatPair(h.qty, h.cost).c_str(), err);
}

bool WriteAlert(const std::wstring& path, const std::wstring& symbol, const Alert& a, std::wstring& err) {
    assert(!symbol.empty());
    const bool unset = (a.above == 0.0 && a.below == 0.0);
    return WriteString(path, kAlerts, symbol.c_str(),
                       unset ? nullptr : FormatPair(a.above, a.below).c_str(), err);
}

} // namespace st
