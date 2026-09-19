#include "config.h"

#include <windows.h>

#include <cassert>
#include <cwctype>

namespace st {
namespace {

constexpr wchar_t kDefaultUrlTemplate[] =
    L"https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    L"?range={range}&interval={interval}&includePrePost=false";

constexpr char kDefaultConfigText[] =
    "; StockTool configuration.\r\n"
    "; Symbols use Yahoo Finance notation: TSX = .TO, TSX Venture = .V,\r\n"
    "; NYSE/Nasdaq = bare symbol (e.g. AAPL). Keep this file ASCII.\r\n"
    "\r\n"
    "[settings]\r\n"
    "refresh_seconds=60\r\n"
    "default_range=1Y\r\n"
    "url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
    "?range={range}&interval={interval}&includePrePost=false\r\n"
    "\r\n"
    "[stocks]\r\n"
    "RY.TO=Royal Bank of Canada\r\n"
    "DOL.TO=Dollarama Inc.\r\n"
    "BMO.TO=Bank of Montreal\r\n"
    "BNS.TO=Bank of Nova Scotia\r\n"
    "BN.TO=Brookfield Corporation\r\n";

constexpr size_t kSectionBufChars = 8192;

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::iswspace(s[b]) != 0) { ++b; }
    while (e > b && std::iswspace(s[e - 1]) != 0) { --e; }
    assert(b <= e);
    return s.substr(b, e - b);
}

size_t RangeIndexFromLabel(const std::wstring& label, size_t fallback) {
    assert(fallback < kRanges.size());
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

bool LoadStocks(const std::wstring& path, Config& out, std::wstring& err) {
    std::array<wchar_t, kSectionBufChars> buf{};
    const DWORD n = GetPrivateProfileSectionW(L"stocks", buf.data(),
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

    const UINT refresh = GetPrivateProfileIntW(L"settings", L"refresh_seconds", 60, path.c_str());
    out.refreshSeconds = (refresh < 10u) ? 10u : (refresh > 3600u ? 3600u : refresh);

    std::array<wchar_t, 64> rangeBuf{};
    GetPrivateProfileStringW(L"settings", L"default_range", L"1Y", rangeBuf.data(),
                             static_cast<DWORD>(rangeBuf.size()), path.c_str());
    out.defaultRange = RangeIndexFromLabel(Trim(rangeBuf.data()), 5);

    std::array<wchar_t, 1024> urlBuf{};
    GetPrivateProfileStringW(L"settings", L"url_template", kDefaultUrlTemplate, urlBuf.data(),
                             static_cast<DWORD>(urlBuf.size()), path.c_str());
    out.urlTemplate = Trim(urlBuf.data());
    if (out.urlTemplate.find(L"{symbol}") == std::wstring::npos) {
        err = L"url_template must contain {symbol}";
        return false;
    }

    if (!LoadStocks(path, out, err)) { return false; }
    assert(out.defaultRange < kRanges.size());
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
    if (!WritePrivateProfileStringW(L"stocks", entry.symbol.c_str(), entry.name.c_str(), path.c_str())) {
        err = L"Could not write " + path;
        return false;
    }
    return true;
}

bool DeleteStockEntry(const std::wstring& path, const std::wstring& symbol, std::wstring& err) {
    assert(!path.empty());
    assert(!symbol.empty());
    if (!WritePrivateProfileStringW(L"stocks", symbol.c_str(), nullptr, path.c_str())) {
        err = L"Could not update " + path;
        return false;
    }
    return true;
}

} // namespace st
