// Modal dialogs: add ticker, edit holding, edit alerts.
#pragma once

#include "config.h"
#include "fetcher.h"

#include <windows.h>

#include <functional>

namespace st {

// Called for each ticker the user adds; returns false with `err` set if the
// ticker could not be added (the dialog shows the message and stays open).
using AddTickerFn = std::function<bool(const StockEntry& entry, std::wstring& err)>;

// Runs the Add-ticker dialog. It stays open until the user closes it, so
// several tickers can be added in one go; `onAdd` is invoked for each.
// `cfg` is re-read for duplicate checks after every add. Returns how many
// tickers were added.
size_t RunAddTickerDialog(HINSTANCE inst, HWND owner, const Config& cfg, Fetcher& fetcher, const AddTickerFn& onAdd);

// Same dialog in edit mode for cfg.stocks[index]: fields are pre-filled and
// Save returns the new symbol/name in `entry` (holding/alert untouched).
// The duplicate check ignores the entry being edited. False on Cancel.
bool RunEditTickerDialog(HINSTANCE inst, HWND owner, const Config& cfg, Fetcher& fetcher,
                         size_t index, StockEntry& entry);

// Edits `h` in place; returns true if the user pressed Save with valid input.
bool RunHoldingDialog(HINSTANCE inst, HWND owner, const std::wstring& symbol, Holding& h);

// Edits `a` in place; returns true if the user pressed Save with valid input.
bool RunAlertsDialog(HINSTANCE inst, HWND owner, const std::wstring& symbol, Alert& a);

// Asks for a watch-list name (pre-filled with `name`); true on OK.
bool RunListNameDialog(HINSTANCE inst, HWND owner, const std::wstring& title, std::wstring& name);

} // namespace st
