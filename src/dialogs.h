// Modal dialogs: add ticker, edit holding, edit alerts.
#pragma once

#include "config.h"
#include "fetcher.h"

#include <windows.h>

namespace st {

// Returns true with `out` filled when the user confirmed a valid entry.
// `fetcher` runs the symbol search (may have search disabled).
bool RunAddTickerDialog(HINSTANCE inst, HWND owner, const Config& cfg, Fetcher& fetcher, StockEntry& out);

// Edits `h` in place; returns true if the user pressed Save with valid input.
bool RunHoldingDialog(HINSTANCE inst, HWND owner, const std::wstring& symbol, Holding& h);

// Edits `a` in place; returns true if the user pressed Save with valid input.
bool RunAlertsDialog(HINSTANCE inst, HWND owner, const std::wstring& symbol, Alert& a);

} // namespace st
