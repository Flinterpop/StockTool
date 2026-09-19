// Minimal blocking HTTPS GET on top of WinHTTP.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace st {

struct HttpResult {
    uint32_t status = 0;   // HTTP status code
    size_t   length = 0;   // bytes written to the caller's buffer
};

// Fetches `url` into `buf` (capacity `cap`). Returns false on transport
// failure or if the body does not fit; `err` describes why.
bool HttpGetUrl(const std::wstring& url, char* buf, size_t cap,
                HttpResult& out, std::wstring& err);

} // namespace st
