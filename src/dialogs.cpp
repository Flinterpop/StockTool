#include "dialogs.h"

#include "resource.h"
#include "textfmt.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cwctype>

namespace st {
namespace {

bool SameSymbol(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Centres a dialog over its owner (the template has no DS_CENTER).
void CentreOnOwner(HWND dlg) {
    RECT owner{};
    RECT self{};
    const HWND parent = GetParent(dlg);
    if (parent != nullptr && GetWindowRect(parent, &owner) && GetWindowRect(dlg, &self)) {
        const int x = owner.left + ((owner.right - owner.left) - (self.right - self.left)) / 2;
        const int y = owner.top  + ((owner.bottom - owner.top) - (self.bottom - self.top)) / 2;
        SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

std::wstring FieldText(HWND dlg, int id) {
    std::array<wchar_t, 128> buf{};
    GetDlgItemTextW(dlg, id, buf.data(), static_cast<int>(buf.size()));
    return buf.data();
}

// Parses a non-negative number; blank = 0. False on junk.
bool ParseAmount(const std::wstring& text, double& out) {
    out = 0.0;
    std::wstring t;
    for (wchar_t c : text) {
        if (c == L',' || std::iswspace(c) != 0) { continue; }  // allow 1,234.5
        t += c;
    }
    if (t.empty()) { return true; }
    wchar_t* end = nullptr;
    const double v = wcstod(t.c_str(), &end);
    if (end == nullptr || *end != L'\0' || !std::isfinite(v) || v < 0.0) { return false; }
    out = v;
    return true;
}

std::wstring AmountText(double v) {
    if (v == 0.0) { return L""; }
    std::array<wchar_t, 64> buf{};
    swprintf_s(buf.data(), buf.size(), (std::floor(v) == v) ? L"%.0f" : L"%.4f", v);
    // trim trailing zeros of the 4-decimal form
    std::wstring s = buf.data();
    if (s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') { s.pop_back(); }
        if (!s.empty() && s.back() == L'.') { s.pop_back(); }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Add ticker

constexpr UINT_PTR kSearchTimer   = 1;
constexpr UINT     kSearchDelayMs = 350;   // typing debounce

struct AddState {
    const Config* cfg     = nullptr;
    Fetcher*      fetcher = nullptr;
    StockEntry    result;
    std::array<SearchHit, kMaxSearchHits> hits{};
    size_t        hitCount = 0;
};

AddState* StateOf(HWND dlg) {
    return reinterpret_cast<AddState*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

void StartSearch(HWND dlg, AddState& st) {
    assert(st.fetcher != nullptr);
    KillTimer(dlg, kSearchTimer);
    std::wstring q = FieldText(dlg, IDC_QUERY);
    // trim
    while (!q.empty() && std::iswspace(q.front()) != 0) { q.erase(q.begin()); }
    while (!q.empty() && std::iswspace(q.back()) != 0) { q.pop_back(); }
    if (q.size() < 2) { return; }
    if (st.fetcher->EnqueueSearch(q, dlg)) {
        SetDlgItemTextW(dlg, IDC_HINT, L"Searching…");
    }
}

void ShowSearchResults(HWND dlg, AddState& st) {
    assert(st.fetcher != nullptr);
    std::wstring query;
    std::wstring err;
    if (!st.fetcher->CopySearch(st.hits, st.hitCount, query, err)) { return; }
    const HWND list = GetDlgItem(dlg, IDC_RESULTS);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (!err.empty()) {
        st.hitCount = 0;
        SetDlgItemTextW(dlg, IDC_HINT, (L"Search failed: " + err).c_str());
        return;
    }
    for (size_t i = 0; i < st.hitCount && i < kMaxSearchHits; ++i) {
        const SearchHit& h = st.hits[i];
        std::wstring row = h.symbol + L"\t" + h.name;
        std::wstring where = h.exchange;
        if (!h.type.empty()) { where += (where.empty() ? L"" : L", ") + h.type; }
        if (!where.empty()) { row += L"  (" + where + L")"; }
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
    }
    SetDlgItemTextW(dlg, IDC_HINT, (st.hitCount == 0)
                        ? (L"No matches for \"" + query + L"\"").c_str()
                        : L"Pick a result, or type a symbol below.");
}

void UseSearchResult(HWND dlg, AddState& st, size_t index) {
    if (index >= st.hitCount) { return; }
    const SearchHit& h = st.hits[index];
    SetDlgItemTextW(dlg, IDC_SYMBOL, h.symbol.c_str());
    SetDlgItemTextW(dlg, IDC_NAME, h.name.c_str());
    SetDlgItemTextW(dlg, IDC_HINT, L"");
}

bool ReadAddFields(HWND dlg, AddState& st) {
    assert(st.cfg != nullptr);
    std::wstring symbol = FieldText(dlg, IDC_SYMBOL);
    std::wstring err;
    if (!NormalizeSymbol(symbol, err)) {
        SetDlgItemTextW(dlg, IDC_HINT, err.c_str());
        return false;
    }
    for (size_t i = 0; i < st.cfg->stockCount; ++i) {
        if (SameSymbol(st.cfg->stocks[i].symbol, symbol)) {
            SetDlgItemTextW(dlg, IDC_HINT, (symbol + L" is already in the list").c_str());
            return false;
        }
    }
    st.result        = StockEntry{};
    st.result.symbol = symbol;
    st.result.name   = NormalizeName(FieldText(dlg, IDC_NAME), symbol);
    assert(!st.result.symbol.empty() && !st.result.name.empty());
    return true;
}

INT_PTR CALLBACK AddProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
        auto* st = reinterpret_cast<AddState*>(lp);
        assert(st != nullptr && st->fetcher != nullptr);
        SendDlgItemMessageW(dlg, IDC_QUERY, EM_LIMITTEXT, 64, 0);
        SendDlgItemMessageW(dlg, IDC_SYMBOL, EM_LIMITTEXT, 31, 0);
        SendDlgItemMessageW(dlg, IDC_NAME, EM_LIMITTEXT, 63, 0);
        const int tab = 60;  // dialog units: symbol column, then name
        SendDlgItemMessageW(dlg, IDC_RESULTS, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(&tab));
        if (!st->fetcher->SearchEnabled()) {
            EnableWindow(GetDlgItem(dlg, IDC_QUERY), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_SEARCH), FALSE);
            EnableWindow(GetDlgItem(dlg, IDC_RESULTS), FALSE);
            SetDlgItemTextW(dlg, IDC_HINT, L"Search is disabled (search_url_template is empty).");
            SetFocus(GetDlgItem(dlg, IDC_SYMBOL));
            CentreOnOwner(dlg);
            return FALSE;  // we set the focus ourselves
        }
        CentreOnOwner(dlg);
        return TRUE;  // focus goes to the first control: the search box
    }
    case WM_TIMER:
        if (wp == kSearchTimer) {
            AddState* st = StateOf(dlg);
            if (st != nullptr) { StartSearch(dlg, *st); }
        }
        return TRUE;
    case WM_APP_SEARCH_READY: {
        AddState* st = StateOf(dlg);
        if (st != nullptr) { ShowSearchResults(dlg, *st); }
        return TRUE;
    }
    case WM_COMMAND: {
        const int  id   = LOWORD(wp);
        const UINT code = HIWORD(wp);
        AddState* st = StateOf(dlg);
        if (st == nullptr) { return FALSE; }
        if (id == IDC_QUERY && code == EN_CHANGE) {
            SetTimer(dlg, kSearchTimer, kSearchDelayMs, nullptr);   // restart the debounce
            return TRUE;
        }
        if (id == IDC_SEARCH) {
            StartSearch(dlg, *st);
            return TRUE;
        }
        if (id == IDC_RESULTS && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
            const LRESULT sel = SendDlgItemMessageW(dlg, IDC_RESULTS, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) { UseSearchResult(dlg, *st, static_cast<size_t>(sel)); }
            if (code == LBN_DBLCLK && sel != LB_ERR && ReadAddFields(dlg, *st)) { EndDialog(dlg, IDOK); }
            return TRUE;
        }
        if (id == IDOK) {
            // Enter in the search box searches; elsewhere it adds.
            if (GetFocus() == GetDlgItem(dlg, IDC_QUERY)) {
                StartSearch(dlg, *st);
                return TRUE;
            }
            if (ReadAddFields(dlg, *st)) { EndDialog(dlg, IDOK); }
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    case WM_DESTROY:
        KillTimer(dlg, kSearchTimer);
        return FALSE;
    default:
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// Holding

struct HoldingState {
    std::wstring symbol;
    Holding      value;
};

INT_PTR CALLBACK HoldingProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
        auto* st = reinterpret_cast<HoldingState*>(lp);
        assert(st != nullptr);
        SetDlgItemTextW(dlg, IDC_HTITLE, (L"Position in " + st->symbol).c_str());
        SetDlgItemTextW(dlg, IDC_QTY, AmountText(st->value.qty).c_str());
        SetDlgItemTextW(dlg, IDC_COST, AmountText(st->value.cost).c_str());
        SendDlgItemMessageW(dlg, IDC_QTY, EM_LIMITTEXT, 24, 0);
        SendDlgItemMessageW(dlg, IDC_COST, EM_LIMITTEXT, 24, 0);
        CentreOnOwner(dlg);
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDOK) {
            auto* st = reinterpret_cast<HoldingState*>(GetWindowLongPtrW(dlg, DWLP_USER));
            assert(st != nullptr);
            Holding h;
            if (st == nullptr || !ParseAmount(FieldText(dlg, IDC_QTY), h.qty) ||
                !ParseAmount(FieldText(dlg, IDC_COST), h.cost)) {
                SetDlgItemTextW(dlg, IDC_HHINT, L"Enter plain non-negative numbers.");
                return TRUE;
            }
            if (h.qty == 0.0) { h.cost = 0.0; }  // no shares = no position
            st->value = h;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// Alerts

struct AlertState {
    std::wstring symbol;
    Alert        value;
};

INT_PTR CALLBACK AlertsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
        auto* st = reinterpret_cast<AlertState*>(lp);
        assert(st != nullptr);
        SetDlgItemTextW(dlg, IDC_ATITLE, (L"Alerts for " + st->symbol).c_str());
        SetDlgItemTextW(dlg, IDC_ABOVE, AmountText(st->value.above).c_str());
        SetDlgItemTextW(dlg, IDC_BELOW, AmountText(st->value.below).c_str());
        SendDlgItemMessageW(dlg, IDC_ABOVE, EM_LIMITTEXT, 24, 0);
        SendDlgItemMessageW(dlg, IDC_BELOW, EM_LIMITTEXT, 24, 0);
        CentreOnOwner(dlg);
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDOK) {
            auto* st = reinterpret_cast<AlertState*>(GetWindowLongPtrW(dlg, DWLP_USER));
            assert(st != nullptr);
            Alert a;
            if (st == nullptr || !ParseAmount(FieldText(dlg, IDC_ABOVE), a.above) ||
                !ParseAmount(FieldText(dlg, IDC_BELOW), a.below)) {
                SetDlgItemTextW(dlg, IDC_AHINT, L"Enter plain non-negative prices.");
                return TRUE;
            }
            if (a.above > 0.0 && a.below > 0.0 && a.below >= a.above) {
                SetDlgItemTextW(dlg, IDC_AHINT, L"The lower alert must be below the upper one.");
                return TRUE;
            }
            st->value = a;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

} // namespace

bool RunAddTickerDialog(HINSTANCE inst, HWND owner, const Config& cfg, Fetcher& fetcher, StockEntry& out) {
    assert(inst != nullptr && owner != nullptr);
    AddState st;
    st.cfg     = &cfg;
    st.fetcher = &fetcher;
    const INT_PTR rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ADDTICKER), owner, &AddProc,
                                       reinterpret_cast<LPARAM>(&st));
    if (rc != IDOK) { return false; }
    out = st.result;
    assert(!out.symbol.empty());
    return true;
}

bool RunHoldingDialog(HINSTANCE inst, HWND owner, const std::wstring& symbol, Holding& h) {
    assert(inst != nullptr && owner != nullptr && !symbol.empty());
    HoldingState st;
    st.symbol = symbol;
    st.value  = h;
    const INT_PTR rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_HOLDING), owner, &HoldingProc,
                                       reinterpret_cast<LPARAM>(&st));
    if (rc != IDOK) { return false; }
    h = st.value;
    return true;
}

bool RunAlertsDialog(HINSTANCE inst, HWND owner, const std::wstring& symbol, Alert& a) {
    assert(inst != nullptr && owner != nullptr && !symbol.empty());
    AlertState st;
    st.symbol = symbol;
    st.value  = a;
    const INT_PTR rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ALERTS), owner, &AlertsProc,
                                       reinterpret_cast<LPARAM>(&st));
    if (rc != IDOK) { return false; }
    a = st.value;
    return true;
}

} // namespace st
