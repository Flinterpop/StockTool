#include "quote_parser.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>

using std::min;

namespace st {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxTextBytes = 4096;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) { return {}; }
    if (s.size() > kMaxTextBytes) { return L"?"; }
    std::wstring out(s.size(), L'\0');  // UTF-16 units never exceed UTF-8 bytes
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      out.data(), static_cast<int>(out.size()));
    if (n <= 0) { return L"?"; }
    out.resize(static_cast<size_t>(n));
    return out;
}

// Returns the non-null child `key` of object `j`, or nullptr.
const json* Child(const json& j, const char* key) {
    assert(key != nullptr);
    if (!j.is_object()) { return nullptr; }
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) { return nullptr; }
    return &*it;
}

double NumOr(const json& j, const char* key, double def) {
    const json* c = Child(j, key);
    return (c != nullptr && c->is_number()) ? c->get<double>() : def;
}

std::wstring StrOr(const json& j, const char* key, const wchar_t* def) {
    assert(def != nullptr);
    const json* c = Child(j, key);
    return (c != nullptr && c->is_string()) ? Utf8ToWide(c->get<std::string>()) : def;
}

// Reads element i of a numeric array; false if missing/null/not a number.
bool ElemNum(const json* arr, size_t i, double& v) {
    if (arr == nullptr || !arr->is_array() || i >= arr->size()) { return false; }
    const json& e = (*arr)[i];
    if (!e.is_number()) { return false; }
    v = e.get<double>();
    return true;
}

void ParseMeta(const json& m, QuoteMeta& out) {
    out.currency = StrOr(m, "currency", L"");
    out.exchange = StrOr(m, "fullExchangeName", L"");
    if (out.exchange.empty()) { out.exchange = StrOr(m, "exchangeName", L""); }
    out.longName = StrOr(m, "longName", L"");
    if (out.longName.empty()) { out.longName = StrOr(m, "shortName", L""); }
    out.price          = NumOr(m, "regularMarketPrice", 0.0);
    out.chartPrevClose = NumOr(m, "chartPreviousClose", 0.0);
    out.dayHigh        = NumOr(m, "regularMarketDayHigh", 0.0);
    out.dayLow         = NumOr(m, "regularMarketDayLow", 0.0);
    out.dayVolume      = NumOr(m, "regularMarketVolume", 0.0);
    out.wk52High       = NumOr(m, "fiftyTwoWeekHigh", 0.0);
    out.wk52Low        = NumOr(m, "fiftyTwoWeekLow", 0.0);
    out.marketTime     = static_cast<int64_t>(NumOr(m, "regularMarketTime", 0.0));
    out.gmtOffsetSec   = static_cast<int32_t>(NumOr(m, "gmtoffset", 0.0));
}

bool ParseSeries(const json& r, Series& s, std::wstring& err) {
    s.count = 0;
    const json* ts = Child(r, "timestamp");
    if (ts == nullptr || !ts->is_array()) { return true; }  // no bars (e.g. holiday)

    const json* ind    = Child(r, "indicators");
    const json* quotes = (ind != nullptr) ? Child(*ind, "quote") : nullptr;
    if (quotes == nullptr || !quotes->is_array() || quotes->empty()) {
        err = L"Response has no quote block";
        return false;
    }
    const json& q      = (*quotes)[0];
    const json* open   = Child(q, "open");
    const json* high   = Child(q, "high");
    const json* low    = Child(q, "low");
    const json* close  = Child(q, "close");
    const json* volume = Child(q, "volume");

    const size_t n     = ts->size();
    const size_t start = (n > kMaxPoints) ? n - kMaxPoints : 0;  // keep the newest
    for (size_t i = start; i < n && s.count < kMaxPoints; ++i) {
        Candle c;
        double t = 0.0;
        if (!ElemNum(ts, i, t)) { continue; }
        if (!ElemNum(close, i, c.close)) { continue; }  // null close = no trade
        c.time = static_cast<int64_t>(t);
        if (!ElemNum(open, i, c.open))     { c.open = c.close; }
        if (!ElemNum(high, i, c.high))     { c.high = c.close; }
        if (!ElemNum(low, i, c.low))       { c.low  = c.close; }
        if (!ElemNum(volume, i, c.volume)) { c.volume = 0.0; }
        s.pts[s.count] = c;
        ++s.count;
    }
    assert(s.count <= kMaxPoints);
    return true;
}

} // namespace

bool ParseChartJson(const char* data, size_t len, QuoteData& out, std::wstring& err) {
    assert(data != nullptr);
    out = QuoteData{};
    if (len == 0) {
        err = L"Empty response";
        return false;
    }
    const json doc = json::parse(data, data + len, nullptr, false);
    if (doc.is_discarded()) {
        err = L"Response is not valid JSON";
        return false;
    }
    const json* chart = Child(doc, "chart");
    if (chart == nullptr) {
        err = L"Response has no chart block";
        return false;
    }
    const json* perr = Child(*chart, "error");
    if (perr != nullptr) {
        err = L"Provider: " + StrOr(*perr, "description", L"unknown error");
        return false;
    }
    const json* results = Child(*chart, "result");
    if (results == nullptr || !results->is_array() || results->empty()) {
        err = L"Response has no result";
        return false;
    }
    const json& r = (*results)[0];
    const json* meta = Child(r, "meta");
    if (meta != nullptr) { ParseMeta(*meta, out.meta); }
    if (!ParseSeries(r, out.series, err)) { return false; }
    out.valid = true;
    assert(err.empty() || !out.valid);
    return true;
}

bool ParseQuoteBatchJson(const char* data, size_t len,
                         std::array<QuoteStats, kMaxStocks>& out, size_t& count, std::wstring& err) {
    assert(data != nullptr);
    count = 0;
    if (len == 0) {
        err = L"Empty response";
        return false;
    }
    const json doc = json::parse(data, data + len, nullptr, false);
    if (doc.is_discarded()) {
        err = L"Response is not valid JSON";
        return false;
    }
    const json* qr = Child(doc, "quoteResponse");
    if (qr == nullptr) {
        // Yahoo reports auth problems as {"finance":{"error":{...}}}.
        const json* fin  = Child(doc, "finance");
        const json* ferr = (fin != nullptr) ? Child(*fin, "error") : nullptr;
        err = (ferr != nullptr) ? L"Provider: " + StrOr(*ferr, "description", L"unknown error")
                                : L"Response has no quoteResponse block";
        return false;
    }
    const json* perr = Child(*qr, "error");
    if (perr != nullptr) {
        err = L"Provider: " + StrOr(*perr, "description", L"unknown error");
        return false;
    }
    const json* results = Child(*qr, "result");
    if (results == nullptr || !results->is_array()) {
        err = L"Response has no result";
        return false;
    }
    const size_t n = min(results->size(), kMaxStocks);
    for (size_t i = 0; i < n; ++i) {
        const json& r = (*results)[i];
        QuoteStats& s = out[count];
        s = QuoteStats{};
        s.symbol = StrOr(r, "symbol", L"");
        if (s.symbol.empty()) { continue; }
        s.marketCap        = NumOr(r, "marketCap", 0.0);
        s.trailingPE       = NumOr(r, "trailingPE", 0.0);
        s.forwardPE        = NumOr(r, "forwardPE", 0.0);
        s.eps              = NumOr(r, "epsTrailingTwelveMonths", 0.0);
        s.dividendYieldPct = NumOr(r, "dividendYield", 0.0);
        if (s.dividendYieldPct <= 0.0) {
            s.dividendYieldPct = NumOr(r, "trailingAnnualDividendYield", 0.0) * 100.0;
        }
        s.dividendRate     = NumOr(r, "trailingAnnualDividendRate", 0.0);
        s.avgVolume3M      = NumOr(r, "averageDailyVolume3Month", 0.0);
        s.priceToBook      = NumOr(r, "priceToBook", 0.0);
        s.open             = NumOr(r, "regularMarketOpen", 0.0);
        s.prevClose        = NumOr(r, "regularMarketPreviousClose", 0.0);
        s.valid = true;
        ++count;
    }
    assert(count <= kMaxStocks);
    return true;
}

bool ParseSearchJson(const char* data, size_t len,
                     std::array<SearchHit, kMaxSearchHits>& out, size_t& count, std::wstring& err) {
    assert(data != nullptr);
    count = 0;
    if (len == 0) {
        err = L"Empty response";
        return false;
    }
    const json doc = json::parse(data, data + len, nullptr, false);
    if (doc.is_discarded()) {
        err = L"Response is not valid JSON";
        return false;
    }
    const json* quotes = Child(doc, "quotes");
    if (quotes == nullptr || !quotes->is_array()) {
        const json* fin  = Child(doc, "finance");
        const json* ferr = (fin != nullptr) ? Child(*fin, "error") : nullptr;
        err = (ferr != nullptr) ? L"Provider: " + StrOr(*ferr, "description", L"unknown error")
                                : L"Response has no quotes block";
        return false;
    }
    const size_t n = min(quotes->size(), kMaxSearchHits);
    for (size_t i = 0; i < n; ++i) {
        const json& q = (*quotes)[i];
        SearchHit& h = out[count];
        h = SearchHit{};
        h.symbol = StrOr(q, "symbol", L"");
        if (h.symbol.empty()) { continue; }
        h.name = StrOr(q, "longname", L"");
        if (h.name.empty()) { h.name = StrOr(q, "shortname", L""); }
        h.exchange = StrOr(q, "exchDisp", L"");
        if (h.exchange.empty()) { h.exchange = StrOr(q, "exchange", L""); }
        h.type = StrOr(q, "typeDisp", L"");
        if (h.type.empty()) { h.type = StrOr(q, "quoteType", L""); }
        ++count;
    }
    assert(count <= kMaxSearchHits);
    return true;
}

} // namespace st
