// The in-app user guide: a modeless window showing src/help.rtf.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace st {

// Shows (or brings to the front) the help window. Only one exists at a time.
void ShowHelpWindow(HINSTANCE inst, HWND owner, bool dark);

// A modeless monospace text window that re-asks `text()` every two seconds
// while open: the data-source health panel. Only one exists at a time.
using TextFn = std::function<std::wstring()>;
void ShowHealthWindow(HINSTANCE inst, HWND owner, bool dark, const TextFn& text);

} // namespace st
