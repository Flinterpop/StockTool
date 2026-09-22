// Minimal blocking HTTPS GET/POST on top of WinHTTP.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace st {

struct HttpResult {
    uint32_t status    = 0;   // HTTP status code
    size_t   length    = 0;   // bytes written to the caller's buffer
    uint32_t elapsedMs = 0;   // wall time of the whole request
};

// One WinHTTP session, reused across requests so cookies persist (the
// fundamentals endpoint needs a cookie + crumb pair). Not thread-safe:
// owned and used by the worker thread only.
class HttpClient {
public:
    HttpClient() = default;
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Fetches `url` into `buf` (capacity `cap`). Returns false on transport
    // failure or if the body does not fit; `err` describes why. Any HTTP
    // status counts as success here; the caller inspects `out.status`.
    bool Get(const std::wstring& url, char* buf, size_t cap, HttpResult& out, std::wstring& err);

    // POSTs `body` (UTF-8) with the given Content-Type and optional extra
    // headers ("Name: value\r\n" lines). Same contract as Get otherwise.
    bool Post(const std::wstring& url, const std::wstring& contentType, const std::wstring& extraHeaders,
              const std::string& body, char* buf, size_t cap, HttpResult& out, std::wstring& err);

private:
    bool Request(const wchar_t* method, const std::wstring& url, const std::wstring& extraHeaders,
                 const std::string& body, char* buf, size_t cap, HttpResult& out, std::wstring& err);
    bool EnsureSession(std::wstring& err);
    void* session_ = nullptr;   // HINTERNET
};

// Percent-encodes everything except unreserved characters.
std::wstring UrlEncode(const std::wstring& s);

} // namespace st
