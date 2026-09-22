// Portfolio history CSV: date,list,value,cost,currency (UTF-8, CRLF).
#include "history.h"

#include "textfmt.h"

#include <windows.h>

#include <cassert>
#include <cstdlib>
#include <ctime>

namespace st {

namespace {

constexpr size_t kMaxFileBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxLines     = 65536;
constexpr char   kHeader[]     = "date,list,value,cost,currency\r\n";
constexpr char   kDefaultList[] = "(default)";

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (size_t i = 0; i < w.size() && i < 256; ++i) { s += (w[i] < 0x80) ? static_cast<char>(w[i]) : '?'; }
    return s;
}

std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size() && i < 256; ++i) { w += static_cast<wchar_t>(static_cast<unsigned char>(s[i])); }
    return w;
}

// List names never contain commas (NormalizeListName), so no quoting is needed.
std::string ListField(const std::wstring& list) {
    return list.empty() ? std::string(kDefaultList) : Narrow(list);
}

bool ReadAll(const std::wstring& path, std::string& out, bool& missing, std::wstring& err) {
    out.clear();
    missing = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        missing = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
        if (!missing) { err = L"Cannot open " + path; }
        return missing;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > static_cast<LONGLONG>(kMaxFileBytes)) {
        CloseHandle(h);
        err = L"History file is too large: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != out.size()) {
        err = L"Cannot read " + path;
        return false;
    }
    return true;
}

bool WriteAll(const std::wstring& path, const std::string& text, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = L"Cannot write " + path;
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != text.size()) {
        err = L"Cannot write " + path;
        return false;
    }
    return true;
}

// One data line -> fields. False for the header, blanks and malformed rows.
struct Row {
    int64_t     day = 0;
    std::string list;
    double      value = 0.0;
    double      cost  = 0.0;
};

bool ParseRow(const std::string& line, Row& r) {
    size_t a = line.find(',');
    if (a == std::string::npos) { return false; }
    size_t b = line.find(',', a + 1);
    if (b == std::string::npos) { return false; }
    size_t c = line.find(',', b + 1);
    if (c == std::string::npos) { return false; }
    size_t d = line.find(',', c + 1);   // currency column (optional)
    if (!ParseIsoDate(Widen(line.substr(0, a)), r.day)) { return false; }
    r.list = line.substr(a + 1, b - a - 1);
    char* end = nullptr;
    r.value = std::strtod(line.c_str() + b + 1, &end);
    if (end == nullptr || *end != ',') { return false; }
    const std::string costText = line.substr(c + 1, (d == std::string::npos ? line.size() : d) - c - 1);
    r.cost = std::strtod(costText.c_str(), nullptr);
    return true;
}

// Calls fn(line) for each non-empty line; bounded.
template <typename Fn>
void ForEachLine(const std::string& text, Fn fn) {
    size_t pos = 0;
    for (size_t guard = 0; guard < kMaxLines && pos < text.size(); ++guard) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) { eol = text.size(); }
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        fn(line);
    }
}

} // namespace

std::wstring HistoryPath(const std::wstring& cfgPath) {
    const size_t slash = cfgPath.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"" : cfgPath.substr(0, slash + 1);
    return dir + L"portfolio-history.csv";
}

int64_t LocalCalendarDay(int64_t unixTime) {
    const __time64_t t = static_cast<__time64_t>(unixTime);
    tm local{};
    if (_localtime64_s(&local, &t) != 0) { return unixTime - unixTime % 86400; }
    tm day{};
    day.tm_year = local.tm_year;
    day.tm_mon  = local.tm_mon;
    day.tm_mday = local.tm_mday;
    const __time64_t utcMidnight = _mkgmtime64(&day);
    assert(utcMidnight >= 0);
    return static_cast<int64_t>(utcMidnight);
}

bool LoadHistory(const std::wstring& path, const std::wstring& list, History& out, std::wstring& err) {
    out = History{};
    std::string text;
    bool missing = false;
    if (!ReadAll(path, text, missing, err)) { return false; }
    if (missing) { return true; }
    const std::string want = ListField(list);
    ForEachLine(text, [&](const std::string& line) {
        Row r;
        if (!ParseRow(line, r) || r.list != want) { return; }
        if (out.count > 0 && r.day <= out.pts[out.count - 1].day) {
            // Out of order or duplicate day: the later line wins for the same day.
            if (r.day == out.pts[out.count - 1].day) { out.pts[out.count - 1] = HistoryPoint{ r.day, r.value, r.cost }; }
            return;
        }
        if (out.count == kMaxHistoryPoints) {
            // Full: drop the oldest to make room for the newest.
            for (size_t i = 1; i < kMaxHistoryPoints; ++i) { out.pts[i - 1] = out.pts[i]; }
            --out.count;
        }
        out.pts[out.count] = HistoryPoint{ r.day, r.value, r.cost };
        ++out.count;
    });
    assert(out.count <= kMaxHistoryPoints);
    return true;
}

bool RecordHistory(const std::wstring& path, const std::wstring& list, const HistoryPoint& p,
                   const std::wstring& currency, std::wstring& err) {
    assert(p.day > 0);
    std::string text;
    bool missing = false;
    if (!ReadAll(path, text, missing, err)) { return false; }
    const std::string me = ListField(list);
    const std::string today = Narrow(FormatIsoDate(p.day));

    // Keep every line except a previous row for (today, list); count this
    // list's rows so the oldest can be dropped once the cap is reached.
    std::string out;
    out.reserve(text.size() + 64);
    size_t mine = 0;
    ForEachLine(text, [&](const std::string& line) {
        Row r;
        if (ParseRow(line, r) && r.list == me) {
            if (r.day == p.day) { return; }
            ++mine;
        }
        out += line + "\r\n";
    });
    if (mine >= kMaxHistoryPoints) {
        // Drop this list's oldest row (the first one in file order).
        std::string trimmed;
        trimmed.reserve(out.size());
        bool dropped = false;
        ForEachLine(out, [&](const std::string& line) {
            Row r;
            if (!dropped && ParseRow(line, r) && r.list == me) { dropped = true; return; }
            trimmed += line + "\r\n";
        });
        out = trimmed;
    }
    if (out.rfind(kHeader, 0) != 0) { out = kHeader + out; }
    char row[160] = {};
    sprintf_s(row, sizeof(row), "%s,%s,%.2f,%.2f,%s\r\n", today.c_str(), me.c_str(), p.value, p.cost, Narrow(currency).c_str());
    out += row;
    return WriteAll(path, out, err);
}

} // namespace st
