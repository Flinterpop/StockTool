#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cassert>
#include <cwchar>

namespace st {
namespace {

// Widen the CMake-supplied version string for the User-Agent.
#define ST_WIDE_(s) L##s
#define ST_WIDE(s)  ST_WIDE_(s)
constexpr wchar_t kAgent[] = L"StockTool/" ST_WIDE(STOCKTOOL_VERSION);

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h != nullptr) { WinHttpCloseHandle(h); } }
};

std::wstring ErrorText(const wchar_t* stage, DWORD code) {
    assert(stage != nullptr);
    std::array<wchar_t, 256> msg{};
    const HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             mod, code, 0, msg.data(), static_cast<DWORD>(msg.size()), nullptr);
    if (n == 0) {
        swprintf_s(msg.data(), msg.size(), L"error %lu", code);
        n = static_cast<DWORD>(wcslen(msg.data()));
    }
    // Strip the trailing CR/LF FormatMessage appends.
    while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n')) { msg[--n] = L'\0'; }
    return std::wstring(stage) + L": " + msg.data();
}

bool ReadBody(HINTERNET req, char* buf, size_t cap, size_t& len, std::wstring& err) {
    assert(req != nullptr);
    assert(buf != nullptr);
    len = 0;
    constexpr int kMaxChunks = 4096;
    for (int i = 0; i < kMaxChunks; ++i) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            err = ErrorText(L"WinHttpQueryDataAvailable", GetLastError());
            return false;
        }
        if (avail == 0) { return true; }
        if (len + avail > cap) {
            err = L"Response exceeds receive buffer";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + len, avail, &got)) {
            err = ErrorText(L"WinHttpReadData", GetLastError());
            return false;
        }
        if (got == 0) { return true; }
        len += got;
        assert(len <= cap);
    }
    err = L"Response too fragmented";
    return false;
}

} // namespace

bool HttpGetUrl(const std::wstring& url, char* buf, size_t cap,
                HttpResult& out, std::wstring& err) {
    assert(buf != nullptr);
    assert(cap > 0);
    out = HttpResult{};
    if (url.empty() || url.size() > 4096) {
        err = L"Bad URL";
        return false;
    }

    std::array<wchar_t, 256>  host{};
    std::array<wchar_t, 2048> path{};
    std::array<wchar_t, 2048> extra{};
    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host.data();
    uc.dwHostNameLength  = static_cast<DWORD>(host.size());
    uc.lpszUrlPath       = path.data();
    uc.dwUrlPathLength   = static_cast<DWORD>(path.size());
    uc.lpszExtraInfo     = extra.data();
    uc.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        err = ErrorText(L"WinHttpCrackUrl", GetLastError());
        return false;
    }
    const std::wstring target = std::wstring(path.data()) + extra.data();

    Handle session;
    session.h = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.h == nullptr) {
        err = ErrorText(L"WinHttpOpen", GetLastError());
        return false;
    }
    // resolve, connect, send, receive (ms)
    if (!WinHttpSetTimeouts(session.h, 10000, 10000, 10000, 15000)) {
        err = ErrorText(L"WinHttpSetTimeouts", GetLastError());
        return false;
    }

    Handle conn;
    conn.h = WinHttpConnect(session.h, host.data(), uc.nPort, 0);
    if (conn.h == nullptr) {
        err = ErrorText(L"WinHttpConnect", GetLastError());
        return false;
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    Handle req;
    req.h = WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (req.h == nullptr) {
        err = ErrorText(L"WinHttpOpenRequest", GetLastError());
        return false;
    }

    // Transparent gzip/deflate; best effort, unsupported on very old builds.
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    const BOOL decompOk = WinHttpSetOption(req.h, WINHTTP_OPTION_DECOMPRESSION,
                                           &decomp, sizeof(decomp));
    (void)decompOk;

    const std::wstring headers =
        std::wstring(L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) ") + kAgent +
        L"\r\nAccept: application/json\r\n";
    if (!WinHttpAddRequestHeaders(req.h, headers.c_str(), static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        err = ErrorText(L"WinHttpAddRequestHeaders", GetLastError());
        return false;
    }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = ErrorText(L"WinHttpSendRequest", GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(req.h, nullptr)) {
        err = ErrorText(L"WinHttpReceiveResponse", GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD size   = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        err = ErrorText(L"WinHttpQueryHeaders", GetLastError());
        return false;
    }
    out.status = status;
    return ReadBody(req.h, buf, cap, out.length, err);
}

} // namespace st
