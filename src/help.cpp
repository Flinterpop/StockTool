#include "help.h"

#include "resource.h"

#include <richedit.h>

#include <cassert>

namespace st {
namespace {

constexpr wchar_t kHelpClass[] = L"StockToolHelpWindow";
constexpr int     kEditId      = 1;

HWND g_help = nullptr;   // the one help window, if open

// The RTF resource is streamed into the control in chunks.
struct StreamState {
    const char* data = nullptr;
    DWORD       size = 0;
    DWORD       pos  = 0;
};

DWORD CALLBACK StreamIn(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* read) {
    auto* st = reinterpret_cast<StreamState*>(cookie);
    assert(st != nullptr && read != nullptr);
    const DWORD left = st->size - st->pos;
    const DWORD n    = (static_cast<DWORD>(cb) < left) ? static_cast<DWORD>(cb) : left;
    if (n > 0) { memcpy(buf, st->data + st->pos, n); }
    st->pos += n;
    *read = static_cast<LONG>(n);
    return 0;
}

bool LoadHelpText(HINSTANCE inst, HWND edit) {
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(IDR_HELP), RT_RCDATA);
    if (res == nullptr) { return false; }
    HGLOBAL h = LoadResource(inst, res);
    if (h == nullptr) { return false; }
    StreamState st;
    st.data = static_cast<const char*>(LockResource(h));
    st.size = SizeofResource(inst, res);
    if (st.data == nullptr || st.size == 0) { return false; }
    EDITSTREAM es{};
    es.dwCookie    = reinterpret_cast<DWORD_PTR>(&st);
    es.pfnCallback = &StreamIn;
    SendMessageW(edit, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
    return es.dwError == 0;
}

LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        HWND edit = GetDlgItem(hwnd, kEditId);
        if (edit != nullptr) { MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE); }
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(GetDlgItem(hwnd, kEditId));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); }
        return 0;
    case WM_NOTIFY: {
        // Esc inside the RichEdit closes the window too.
        const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
        if (hdr->idFrom == kEditId && hdr->code == EN_MSGFILTER) {
            const auto* mf = reinterpret_cast<const MSGFILTER*>(lp);
            if (mf->msg == WM_KEYDOWN && mf->wParam == VK_ESCAPE) { DestroyWindow(hwnd); }
        }
        return 0;
    }
    case WM_DESTROY:
        g_help = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void ShowHelpWindow(HINSTANCE inst, HWND owner, bool dark) {
    assert(inst != nullptr);
    if (g_help != nullptr) {
        ShowWindow(g_help, SW_SHOW);
        SetForegroundWindow(g_help);
        return;
    }
    static HMODULE richEdit = LoadLibraryW(L"msftedit.dll");   // registers RICHEDIT50W
    if (richEdit == nullptr) { return; }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &HelpProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = kHelpClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) { return; }
    }

    // Size relative to the owner so it lands on the same monitor.
    RECT own{};
    GetWindowRect(owner, &own);
    const int w = (own.right - own.left) * 2 / 3;
    const int h = (own.bottom - own.top) * 4 / 5;
    const int x = own.left + ((own.right - own.left) - w) / 2;
    const int y = own.top + ((own.bottom - own.top) - h) / 2;
    g_help = CreateWindowExW(WS_EX_TOOLWINDOW, kHelpClass, L"StockTool - user guide",
                             WS_OVERLAPPEDWINDOW, x, y, w, h, owner, nullptr, inst, nullptr);
    if (g_help == nullptr) { return; }

    HWND edit = CreateWindowExW(0, MSFTEDIT_CLASS, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, w, h, g_help, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), inst, nullptr);
    assert(edit != nullptr);
    SendMessageW(edit, EM_SETEVENTMASK, 0, ENM_KEYEVENTS);
    SendMessageW(edit, EM_SETBKGNDCOLOR, 0, dark ? RGB(32, 33, 36) : RGB(255, 255, 255));
    const bool loaded = LoadHelpText(inst, edit);
    assert(loaded);
    (void)loaded;
    if (dark) {
        // The RTF colours are for the light theme; recolour the text for dark.
        CHARFORMAT2W cf{};
        cf.cbSize      = sizeof(cf);
        cf.dwMask      = CFM_COLOR;
        cf.crTextColor = RGB(232, 234, 237);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
        SendMessageW(edit, EM_SETSEL, 0, 0);
    }
    // A little breathing room inside the control (survives resizing).
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(16, 16));
    ShowWindow(g_help, SW_SHOW);
    SetFocus(edit);
}

// ---------------------------------------------------------------------------
// Health panel

namespace {

constexpr wchar_t kHealthClass[] = L"StockToolHealthWindow";
constexpr UINT_PTR kHealthTimer  = 1;

HWND   g_health = nullptr;
HFONT  g_healthFont = nullptr;
HBRUSH g_healthBrush = nullptr;
bool   g_healthDark = false;
TextFn g_healthText;   // set for the lifetime of the window

void RefreshHealth(HWND hwnd) {
    HWND edit = GetDlgItem(hwnd, kEditId);
    if (edit == nullptr || !g_healthText) { return; }
    // Keep the caret/scroll position stable across refreshes.
    const LRESULT firstLine = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextW(edit, g_healthText().c_str());
    SendMessageW(edit, EM_LINESCROLL, 0, firstLine);
}

LRESULT CALLBACK HealthProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        HWND edit = GetDlgItem(hwnd, kEditId);
        if (edit != nullptr) { MoveWindow(edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE); }
        return 0;
    }
    case WM_TIMER:
        if (wp == kHealthTimer) { RefreshHealth(hwnd); }
        return 0;
    case WM_CTLCOLORSTATIC:   // a read-only EDIT asks with this
        SetBkColor(reinterpret_cast<HDC>(wp), g_healthDark ? RGB(32, 33, 36) : RGB(255, 255, 255));
        SetTextColor(reinterpret_cast<HDC>(wp), g_healthDark ? RGB(232, 234, 237) : RGB(32, 33, 36));
        return reinterpret_cast<LRESULT>(g_healthBrush);
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kHealthTimer);
        g_health = nullptr;
        g_healthText = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void ShowHealthWindow(HINSTANCE inst, HWND owner, bool dark, const TextFn& text) {
    assert(inst != nullptr && static_cast<bool>(text));
    g_healthText = text;
    g_healthDark = dark;
    if (g_health != nullptr) {
        RefreshHealth(g_health);
        ShowWindow(g_health, SW_SHOW);
        SetForegroundWindow(g_health);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &HealthProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon         = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = kHealthClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) { return; }
    }
    if (g_healthBrush != nullptr) { DeleteObject(g_healthBrush); }
    g_healthBrush = CreateSolidBrush(dark ? RGB(32, 33, 36) : RGB(255, 255, 255));
    if (g_healthFont == nullptr) {
        const int dpi = static_cast<int>(GetDpiForWindow(owner));
        g_healthFont = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   FIXED_PITCH | FF_MODERN, L"Consolas");
    }

    RECT own{};
    GetWindowRect(owner, &own);
    const int w = (own.right - own.left) * 3 / 5;
    const int h = (own.bottom - own.top) / 2;
    const int x = own.left + ((own.right - own.left) - w) / 2;
    const int y = own.top + ((own.bottom - own.top) - h) / 2;
    g_health = CreateWindowExW(WS_EX_TOOLWINDOW, kHealthClass, L"StockTool - data source health",
                               WS_OVERLAPPEDWINDOW, x, y, w, h, owner, nullptr, inst, nullptr);
    if (g_health == nullptr) { return; }
    HWND edit = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                0, 0, w, h, g_health, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), inst, nullptr);
    assert(edit != nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_healthFont), TRUE);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
    RefreshHealth(g_health);
    SetTimer(g_health, kHealthTimer, 2000, nullptr);
    ShowWindow(g_health, SW_SHOW);
}

} // namespace st
