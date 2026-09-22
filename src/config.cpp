#include "config.h"
#include "textfmt.h"

#include <shlobj.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cwchar>
#include <cwctype>

using std::max;
using std::min;

namespace st {
namespace {

constexpr wchar_t kDefaultUrlTemplate[] =
    L"https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    L"?range={range}&interval={interval}&includePrePost=false&events=div";

constexpr wchar_t kDefaultQuoteUrlTemplate[] =
    L"https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}";

constexpr wchar_t kDefaultSearchUrlTemplate[] =
    L"https://query2.finance.yahoo.com/v1/finance/search?q={query}&quotesCount=12&newsCount=0&listsCount=0";

constexpr wchar_t kDefaultNewsUrlTemplate[] =
    L"https://query2.finance.yahoo.com/v1/finance/search?q={symbol}&quotesCount=0&newsCount=8&listsCount=0";

constexpr wchar_t kDefaultTmxUrl[] = L"https://app-money.tmx.com/graphql";

// Google News, Canadian English edition, searched by company name.
constexpr wchar_t kDefaultNewsRssTemplate[] =
    L"https://news.google.com/rss/search?q={query}&hl=en-CA&gl=CA&ceid=CA:en";

constexpr char kDefaultConfigText[] =
    "; StockTool configuration.\r\n"
    "; Symbols use Yahoo Finance notation: TSX = .TO, TSX Venture = .V,\r\n"
    "; NYSE/Nasdaq = bare symbol (e.g. AAPL). Keep this file ASCII.\r\n"
    "\r\n"
    "[settings]\r\n"
    "; How often (seconds) prices are re-fetched (10..3600), and while every exchange in the list is closed (60..3600).\r\n"
    "refresh_seconds=60\r\n"
    "closed_refresh_seconds=900\r\n"
    "default_range=1Y\r\n"
    "; Trend inset in the chart corner: 5Y or 1Y\r\n"
    "inset_range=5Y\r\n"
    "; system | light | dark\r\n"
    "theme=system\r\n"
    "start_minimized=0\r\n"
    "minimize_to_tray=0\r\n"
    "; Portfolio totals are converted into this currency.\r\n"
    "portfolio_currency=CAD\r\n"
    "; Index drawn over the chart when View > Benchmark is on (^GSPTSE = S&P/TSX Composite, ^GSPC = S&P 500).\r\n"
    "benchmark=^GSPTSE\r\n"
    "url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    "?range={range}&interval={interval}&includePrePost=false&events=div\r\n"
    "; Fundamentals (market cap, P/E, yield). Leave empty to disable.\r\n"
    "quote_url_template=https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}\r\n"
    "; Symbol search in the Add dialog. Leave empty to disable.\r\n"
    "search_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={query}&quotesCount=12&newsCount=0&listsCount=0\r\n"
    "; Headlines pane: google (Google News, Canadian edition, by company name), yahoo, or none.\r\n"
    "news_source=google\r\n"
    "news_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={symbol}&quotesCount=0&newsCount=8&listsCount=0\r\n"
    "news_rss_template=https://news.google.com/rss/search?q={query}&hl=en-CA&gl=CA&ceid=CA:en\r\n"
    "; Second chart source when Yahoo fails (daily bars): tmx (TMX Money) or none.\r\n"
    "fallback_provider=tmx\r\n"
    "tmx_url=https://app-money.tmx.com/graphql\r\n"
    "\r\n"
    "; Default watch list. Extra lists live in [stocks.<name>] sections.\r\n"
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
    "[alerts]\r\n"
    "\r\n"
    "; symbol=CAD   (currency override when the provider labels a listing oddly)\r\n"
    "[currency]\r\n"
    "\r\n"
    "; SYMBOL.N=YYYY-MM-DD,shares,price   (written by Ticker > Transactions...; negative shares = sell)\r\n"
    "[transactions]\r\n"
    "\r\n"
    "; broker symbol=watch-list symbol   (File > Import from TD...; e.g. TDB902=0P0000A30L)\r\n"
    "[symbol_map]\r\n"
    "\r\n"
    "; watch list=uninvested cash, added to that list portfolio total\r\n"
    "[cash]\r\n";

constexpr size_t kSectionBufChars = 8192;
constexpr wchar_t kSettings[] = L"settings";
constexpr wchar_t kStocks[]   = L"stocks";
constexpr wchar_t kHoldings[] = L"holdings";
constexpr wchar_t kAlerts[]   = L"alerts";
constexpr wchar_t kCurrency[] = L"currency";
constexpr wchar_t kTransactions[] = L"transactions";
constexpr wchar_t kSymbolMap[] = L"symbol_map";
constexpr wchar_t kCash[]      = L"cash";
// [cash] is keyed by watch-list name, and an INI key cannot be empty.
constexpr wchar_t kDefaultListKey[] = L"(default)";
constexpr wchar_t kState[]    = L"state";
constexpr wchar_t kListPrefix[] = L"stocks.";

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

// INI section holding a watch list: [stocks] for the default, [stocks.Name] otherwise.
std::wstring ListSection(const std::wstring& listName) {
    return listName.empty() ? std::wstring(kStocks) : std::wstring(kListPrefix) + listName;
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

// Reads the "key=value" entries of `section` into `out`. False = too large.
bool ReadStockSection(const std::wstring& path, const std::wstring& section, Config& out, std::wstring& err) {
    std::array<wchar_t, kSectionBufChars> buf{};
    const DWORD n = GetPrivateProfileSectionW(section.c_str(), buf.data(),
                                              static_cast<DWORD>(buf.size()), path.c_str());
    if (n >= buf.size() - 2) {
        err = L"[" + section + L"] section is too large";
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
        e.currency = ReadString(path, kCurrency, e.symbol.c_str(), L"");
        std::wstring ignored;
        if (!NormalizeCurrency(e.currency, ignored)) { e.currency.clear(); }
        ReadTransactions(path, e);
        out.stocks[out.stockCount] = e;
        ++out.stockCount;
    }
    assert(out.stockCount <= kMaxStocks);
    return true;
}

// Fills cfg.lists from the section names: "" (default) first, then every
// [stocks.<name>] in file order.
void ReadListNames(const std::wstring& path, Config& cfg) {
    cfg.listCount = 1;
    cfg.lists[0].clear();
    std::array<wchar_t, kSectionBufChars> buf{};
    const DWORD n = GetPrivateProfileSectionNamesW(buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
    size_t pos = 0;
    const size_t prefixLen = wcslen(kListPrefix);
    for (size_t guard = 0; guard < kSectionBufChars && pos < n && cfg.listCount < kMaxLists; ++guard) {
        const wchar_t* name = buf.data() + pos;
        const size_t len = wcsnlen_s(name, buf.size() - pos);
        if (len == 0) { break; }
        pos += len + 1;
        if (_wcsnicmp(name, kListPrefix, prefixLen) != 0 || len <= prefixLen) { continue; }
        cfg.lists[cfg.listCount] = std::wstring(name + prefixLen);
        ++cfg.listCount;
    }
    assert(cfg.listCount >= 1 && cfg.listCount <= kMaxLists);
}

bool ListExists(const Config& cfg, const std::wstring& name) {
    for (size_t i = 0; i < cfg.listCount; ++i) {
        if (_wcsicmp(cfg.lists[i].c_str(), name.c_str()) == 0) { return true; }
    }
    return false;
}

bool DirectoryWritable(const std::wstring& dir) {
    const std::wstring probe = dir + L"stocktool.probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    CloseHandle(h);
    return true;
}

} // namespace

// [transactions] SYMBOL.N=YYYY-MM-DD,qty,price for N = 1.. until a key is missing.
void ReadTransactions(const std::wstring& path, StockEntry& e) {
    e.txCount = 0;
    for (size_t n = 1; n <= kMaxTxPerSymbol; ++n) {
        const std::wstring key = e.symbol + L"." + std::to_wstring(n);
        const std::wstring value = ReadString(path, kTransactions, key.c_str(), L"");
        if (value.empty()) { break; }
        Transaction t;
        std::array<wchar_t, 16> date{};
        if (swscanf_s(value.c_str(), L"%15[^,],%lf,%lf", date.data(), static_cast<unsigned>(date.size()), &t.qty, &t.price) != 3) { continue; }
        if (!ParseIsoDate(date.data(), t.date) || t.qty == 0.0 || t.price < 0.0) { continue; }
        e.tx[e.txCount] = t;
        ++e.txCount;
    }
    assert(e.txCount <= kMaxTxPerSymbol);
}

std::wstring DefaultConfigPath() {
    std::array<wchar_t, MAX_PATH> exe{};
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    assert(n > 0 && n < exe.size());
    std::wstring dir(exe.data(), n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) { dir.resize(slash + 1); }
    const std::wstring beside = dir + L"stocktool.cfg";
    if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES || DirectoryWritable(dir)) {
        return beside;   // portable / development layout
    }
    // Installed under Program Files: keep the config in the user's profile.
    std::array<wchar_t, MAX_PATH> appdata{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appdata.data()))) {
        return beside;   // no profile folder: fall back and let the caller report the error
    }
    const std::wstring folder = std::wstring(appdata.data()) + L"\\StockTool";
    CreateDirectoryW(folder.c_str(), nullptr);   // may already exist
    return folder + L"\\stocktool.cfg";
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

bool LoadConfig(const std::wstring& path, const std::wstring& listName, Config& out, std::wstring& err) {
    assert(!path.empty());
    out = Config{};
    out.path = path;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"Config file not found: " + path;
        return false;
    }

    const UINT refresh = GetPrivateProfileIntW(kSettings, L"refresh_seconds", 60, path.c_str());
    out.refreshSeconds = (refresh < 10u) ? 10u : (refresh > 3600u ? 3600u : refresh);
    const UINT closed = GetPrivateProfileIntW(kSettings, L"closed_refresh_seconds", 900, path.c_str());
    out.closedRefreshSeconds = (closed < 60u) ? 60u : (closed > 3600u ? 3600u : closed);
    if (out.closedRefreshSeconds < out.refreshSeconds) { out.closedRefreshSeconds = out.refreshSeconds; }
    out.defaultRange   = RangeIndexFromLabel(ReadString(path, kSettings, L"default_range", L"1Y"), kRange1Y);
    out.insetRange     = RangeIndexFromLabel(ReadString(path, kSettings, L"inset_range", L"5Y"), kRange5Y);
    if (out.insetRange != kRange1Y && out.insetRange != kRange5Y) { out.insetRange = kRange5Y; }

    const std::wstring theme = ReadString(path, kSettings, L"theme", L"system");
    if (_wcsicmp(theme.c_str(), L"light") == 0)     { out.theme = ThemeMode::Light; }
    else if (_wcsicmp(theme.c_str(), L"dark") == 0) { out.theme = ThemeMode::Dark; }
    else                                            { out.theme = ThemeMode::System; }
    out.startMinimized = ReadBool(path, kSettings, L"start_minimized", false);
    out.minimizeToTray = ReadBool(path, kSettings, L"minimize_to_tray", false);
    out.portfolioCurrency = ReadString(path, kSettings, L"portfolio_currency", L"CAD");
    std::wstring ignored;
    if (!NormalizeCurrency(out.portfolioCurrency, ignored) || out.portfolioCurrency.empty()) {
        out.portfolioCurrency = L"CAD";
    }

    out.benchmark = ReadString(path, kSettings, L"benchmark", L"^GSPTSE");
    std::wstring benchErr;
    if (!out.benchmark.empty() && !NormalizeSymbol(out.benchmark, benchErr)) { out.benchmark.clear(); }

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
    out.searchUrlTemplate = ReadString(path, kSettings, L"search_url_template", kDefaultSearchUrlTemplate);
    if (!out.searchUrlTemplate.empty() && out.searchUrlTemplate.find(L"{query}") == std::wstring::npos) {
        err = L"search_url_template must contain {query} (or be empty)";
        return false;
    }
    out.newsUrlTemplate = ReadString(path, kSettings, L"news_url_template", kDefaultNewsUrlTemplate);
    if (!out.newsUrlTemplate.empty() && out.newsUrlTemplate.find(L"{symbol}") == std::wstring::npos) {
        err = L"news_url_template must contain {symbol} (or be empty)";
        return false;
    }
    out.newsRssTemplate = ReadString(path, kSettings, L"news_rss_template", kDefaultNewsRssTemplate);
    if (!out.newsRssTemplate.empty() && out.newsRssTemplate.find(L"{query}") == std::wstring::npos) {
        err = L"news_rss_template must contain {query} (or be empty)";
        return false;
    }
    const std::wstring source = ReadString(path, kSettings, L"news_source", L"google");
    if (_wcsicmp(source.c_str(), L"yahoo") == 0)      { out.newsSource = NewsSource::Yahoo; }
    else if (_wcsicmp(source.c_str(), L"none") == 0)  { out.newsSource = NewsSource::None; }
    else                                               { out.newsSource = NewsSource::Google; }
    if ((out.newsSource == NewsSource::Yahoo && out.newsUrlTemplate.empty()) ||
        (out.newsSource == NewsSource::Google && out.newsRssTemplate.empty())) {
        out.newsSource = NewsSource::None;   // the chosen source has no template
    }

    const std::wstring fallback = ReadString(path, kSettings, L"fallback_provider", L"tmx");
    out.fallback = (_wcsicmp(fallback.c_str(), L"none") == 0) ? FallbackProvider::None : FallbackProvider::Tmx;
    out.tmxUrl = ReadString(path, kSettings, L"tmx_url", kDefaultTmxUrl);
    if (out.tmxUrl.empty()) { out.fallback = FallbackProvider::None; }

    ReadListNames(path, out);
    out.listName = ListExists(out, listName) ? listName : L"";
    out.cash = ReadCash(path, out.listName);
    if (!ReadStockSection(path, ListSection(out.listName), out, err)) { return false; }
    if (out.stockCount == 0) {
        if (out.listName.empty()) {
            err = L"No stocks listed under [stocks] in " + path;
            return false;
        }
        // An empty named list is allowed; the app shows an empty watch list.
    }
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
    out.news      = ReadBool(path, kState, L"news", false);
    out.benchmark = ReadBool(path, kState, L"benchmark", false);
    out.portfolio = ReadBool(path, kState, L"portfolio", false);
    double ix = 0.0;
    double iy = 0.0;
    ParsePair(ReadString(path, kState, L"inset_pos", L""), ix, iy);
    out.insetX    = static_cast<float>(min(max(ix, 0.0), 1.0));
    out.insetY    = static_cast<float>(min(max(iy, 0.0), 1.0));
    out.selected  = ReadString(path, kState, L"selected", L"");
    out.list      = ReadString(path, kState, L"list", L"");
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
        { L"news",      s.news ? L"1" : L"0" },
        { L"benchmark", s.benchmark ? L"1" : L"0" },
        { L"portfolio", s.portfolio ? L"1" : L"0" },
        { L"inset_pos", FormatPair(s.insetX, s.insetY) },
        { L"selected",  s.selected },
        { L"list",      s.list },
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

bool NormalizeCurrency(std::wstring& code, std::wstring& err) {
    code = Trim(code);
    if (code.empty()) { return true; }
    if (code.size() != 3) {
        err = L"Currency must be a 3-letter code such as CAD or USD";
        return false;
    }
    for (wchar_t& c : code) {
        c = static_cast<wchar_t>(std::towupper(c));
        if (c < L'A' || c > L'Z') {
            err = L"Currency must be a 3-letter code such as CAD or USD";
            return false;
        }
    }
    return true;
}

bool NormalizeListName(std::wstring& name, std::wstring& err) {
    constexpr size_t kMaxListName = 24;
    name = Trim(name);
    if (name.empty()) {
        err = L"Enter a list name";
        return false;
    }
    if (name.size() > kMaxListName) {
        err = L"List names are at most 24 characters";
        return false;
    }
    for (wchar_t c : name) {
        const bool ok = std::iswalnum(c) != 0 || c == L' ' || c == L'-' || c == L'_';
        if (!ok) {
            err = L"List names may only contain letters, digits, spaces, - and _";
            return false;
        }
    }
    return true;
}

bool WriteStockEntry(const Config& cfg, const StockEntry& entry, std::wstring& err) {
    assert(!cfg.path.empty());
    assert(!entry.symbol.empty());
    const std::wstring section = ListSection(cfg.listName);
    if (!WriteString(cfg.path, section.c_str(), entry.symbol.c_str(), entry.name.c_str(), err)) { return false; }
    // Holding/alert/currency are shared across lists by symbol: only set
    // values are written here, so adding a symbol to a second list can never
    // wipe what the first list already stored. Clearing goes through
    // WriteHolding/WriteAlert/WriteCurrency directly.
    const bool holdingSet = entry.holding.qty != 0.0 || entry.holding.cost != 0.0;
    const bool alertSet   = entry.alert.above != 0.0 || entry.alert.below != 0.0;
    if (holdingSet && !WriteHolding(cfg.path, entry.symbol, entry.holding, err)) { return false; }
    if (alertSet && !WriteAlert(cfg.path, entry.symbol, entry.alert, err)) { return false; }
    if (!entry.currency.empty() && !WriteCurrency(cfg.path, entry.symbol, entry.currency, err)) { return false; }
    if (entry.txCount > 0 && !WriteTransactions(cfg.path, entry.symbol, entry.tx.data(), entry.txCount, err)) { return false; }
    return true;
}

bool DeleteStockEntry(const Config& cfg, const std::wstring& symbol, std::wstring& err) {
    assert(!cfg.path.empty());
    assert(!symbol.empty());
    const std::wstring section = ListSection(cfg.listName);
    if (!WriteString(cfg.path, section.c_str(), symbol.c_str(), nullptr, err)) { return false; }
    // Holdings/alerts/currency are shared across lists: keep them while the
    // symbol is still listed somewhere else.
    for (size_t i = 0; i < cfg.listCount; ++i) {
        if (_wcsicmp(cfg.lists[i].c_str(), cfg.listName.c_str()) == 0) { continue; }
        const std::wstring probe = ReadString(cfg.path, ListSection(cfg.lists[i]).c_str(), symbol.c_str(), L"\x01");
        if (probe != L"\x01") { return true; }   // still present in another list
    }
    if (!WriteString(cfg.path, kHoldings, symbol.c_str(), nullptr, err)) { return false; }
    if (!WriteString(cfg.path, kAlerts, symbol.c_str(), nullptr, err)) { return false; }
    if (!WriteString(cfg.path, kCurrency, symbol.c_str(), nullptr, err)) { return false; }
    return WriteTransactions(cfg.path, symbol, nullptr, 0, err);
}

bool WriteStockOrder(const Config& cfg, std::wstring& err) {
    assert(cfg.stockCount <= kMaxStocks);
    std::wstring block;
    for (size_t i = 0; i < cfg.stockCount; ++i) {
        block += cfg.stocks[i].symbol + L"=" + cfg.stocks[i].name;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    const std::wstring section = ListSection(cfg.listName);
    if (!WritePrivateProfileSectionW(section.c_str(), block.c_str(), cfg.path.c_str())) {
        err = L"Could not rewrite [" + section + L"] in " + cfg.path;
        return false;
    }
    return true;
}

bool CreateList(const std::wstring& path, const std::wstring& name, std::wstring& err) {
    assert(!name.empty());
    // A section with a placeholder comment line is enough to make it exist.
    const std::wstring section = ListSection(name);
    const wchar_t block[] = L"; tickers for this list\0";
    if (!WritePrivateProfileSectionW(section.c_str(), block, path.c_str())) {
        err = L"Could not create list " + name;
        return false;
    }
    return true;
}

bool RenameList(const std::wstring& path, const std::wstring& from, const std::wstring& to, std::wstring& err) {
    assert(!from.empty() && !to.empty());
    std::array<wchar_t, kSectionBufChars> buf{};
    const std::wstring oldSection = ListSection(from);
    const DWORD n = GetPrivateProfileSectionW(oldSection.c_str(), buf.data(),
                                              static_cast<DWORD>(buf.size()), path.c_str());
    if (n >= buf.size() - 2) {
        err = L"List " + from + L" is too large to rename";
        return false;
    }
    buf[n] = L'\0';
    buf[n + 1] = L'\0';
    const std::wstring newSection = ListSection(to);
    if (!WritePrivateProfileSectionW(newSection.c_str(), buf.data(), path.c_str())) {
        err = L"Could not create list " + to;
        return false;
    }
    return WriteString(path, oldSection.c_str(), nullptr, nullptr, err);   // deletes the old section
}

bool DeleteList(const std::wstring& path, const std::wstring& name, std::wstring& err) {
    assert(!name.empty());   // the default list cannot be deleted
    return WriteString(path, ListSection(name).c_str(), nullptr, nullptr, err);
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

size_t ReadAllSymbols(const std::wstring& path, std::wstring* out, size_t max) {
    assert(out != nullptr || max == 0);
    Config names;
    ReadListNames(path, names);
    size_t count = 0;
    for (size_t l = 0; l < names.listCount && count < max; ++l) {
        std::array<wchar_t, kSectionBufChars> buf{};
        const DWORD n = GetPrivateProfileSectionW(ListSection(names.lists[l]).c_str(), buf.data(),
                                                  static_cast<DWORD>(buf.size()), path.c_str());
        size_t pos = 0;
        for (size_t guard = 0; guard < kSectionBufChars && pos < n && count < max; ++guard) {
            const wchar_t* entry = buf.data() + pos;
            const size_t len = wcsnlen_s(entry, buf.size() - pos);
            if (len == 0) { break; }
            pos += len + 1;
            StockEntry e;
            if (!SplitEntry(entry, e)) { continue; }
            bool dup = false;
            for (size_t k = 0; k < count && !dup; ++k) { dup = _wcsicmp(out[k].c_str(), e.symbol.c_str()) == 0; }
            if (dup) { continue; }
            out[count] = e.symbol;
            ++count;
        }
    }
    assert(count <= max);
    return count;
}

size_t ReadSymbolMap(const std::wstring& path, SymbolMapEntry* out, size_t max) {
    assert(out != nullptr || max == 0);
    std::array<wchar_t, kSectionBufChars> buf{};
    const DWORD n = GetPrivateProfileSectionW(kSymbolMap, buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
    size_t count = 0;
    size_t pos = 0;
    for (size_t guard = 0; guard < kSectionBufChars && pos < n && count < max; ++guard) {
        const wchar_t* entry = buf.data() + pos;
        const size_t len = wcsnlen_s(entry, buf.size() - pos);
        if (len == 0) { break; }
        pos += len + 1;
        const std::wstring line(entry);
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || line[0] == L';') { continue; }
        std::wstring from = Trim(line.substr(0, eq));
        std::wstring to   = Trim(line.substr(eq + 1));
        std::wstring ignored;
        if (from.empty() || !NormalizeSymbol(to, ignored)) { continue; }
        for (wchar_t& c : from) { c = static_cast<wchar_t>(std::towupper(c)); }
        out[count] = SymbolMapEntry{ from, to };
        ++count;
    }
    assert(count <= max);
    return count;
}

bool WriteTransactions(const std::wstring& path, const std::wstring& symbol,
                       const Transaction* tx, size_t count, std::wstring& err) {
    assert(!symbol.empty());
    assert(count == 0 || tx != nullptr);
    for (size_t n = 1; n <= kMaxTxPerSymbol; ++n) {
        const std::wstring key = symbol + L"." + std::to_wstring(n);
        const wchar_t* value = nullptr;
        std::array<wchar_t, 96> buf{};
        if (n <= count) {
            const Transaction& t = tx[n - 1];
            swprintf_s(buf.data(), buf.size(), L"%s,%.4f,%.4f", FormatIsoDate(t.date).c_str(), t.qty, t.price);
            value = buf.data();
        }
        if (!WriteString(path, kTransactions, key.c_str(), value, err)) { return false; }
    }
    return true;
}

bool WriteCash(const std::wstring& path, const std::wstring& list, double amount, std::wstring& err) {
    assert(std::isfinite(amount));
    const std::wstring key = list.empty() ? kDefaultListKey : list;
    std::array<wchar_t, 32> buf{};
    swprintf_s(buf.data(), buf.size(), L"%.2f", amount);
    return WriteString(path, kCash, key.c_str(), (amount == 0.0) ? nullptr : buf.data(), err);
}

double ReadCash(const std::wstring& path, const std::wstring& list) {
    const std::wstring key  = list.empty() ? kDefaultListKey : list;
    const std::wstring text = ReadString(path, kCash, key.c_str(), L"");
    if (text.empty()) { return 0.0; }
    wchar_t* end = nullptr;
    const double v = wcstod(text.c_str(), &end);
    return std::isfinite(v) ? v : 0.0;
}

bool WriteCurrency(const std::wstring& path, const std::wstring& symbol, const std::wstring& code, std::wstring& err) {
    assert(!symbol.empty());
    return WriteString(path, kCurrency, symbol.c_str(), code.empty() ? nullptr : code.c_str(), err);
}

} // namespace st
