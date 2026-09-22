// Portfolio value history: one row per day per watch list, kept in a CSV
// next to the config so it survives upgrades and can be opened in Excel.
#pragma once

#include "common.h"

#include <array>
#include <string>

namespace st {

constexpr size_t kMaxHistoryPoints = 4096;   // per list, oldest dropped first

struct HistoryPoint {
    int64_t day   = 0;     // Unix seconds at 00:00 UTC of the (local) calendar day
    double  value = 0.0;   // portfolio value in the portfolio currency
    double  cost  = 0.0;   // cost base at that time
};

struct History {
    std::array<HistoryPoint, kMaxHistoryPoints> pts{};
    size_t count = 0;      // ascending by day
};

// portfolio-history.csv beside the config file.
std::wstring HistoryPath(const std::wstring& cfgPath);

// Reads the rows for `list` ("" = default list), ascending. A missing file
// is an empty history (true); an unreadable one is false with `err`.
bool LoadHistory(const std::wstring& path, const std::wstring& list, History& out, std::wstring& err);

// Records `p` for `list`, replacing an existing row for the same day. The
// file is rewritten whole (it is small); rows beyond kMaxHistoryPoints per
// list are dropped oldest first.
bool RecordHistory(const std::wstring& path, const std::wstring& list, const HistoryPoint& p,
                   const std::wstring& currency, std::wstring& err);

// Midnight UTC of the calendar day that `unixTime` falls on in this
// machine's local time zone (the day the user sees).
int64_t LocalCalendarDay(int64_t unixTime);

} // namespace st
