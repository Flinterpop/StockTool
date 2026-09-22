#include "quote_parser.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>

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
    const json* period  = Child(m, "currentTradingPeriod");
    const json* regular = (period != nullptr) ? Child(*period, "regular") : nullptr;
    if (regular != nullptr) {
        out.regularStart = static_cast<int64_t>(NumOr(*regular, "start", 0.0));
        out.regularEnd   = static_cast<int64_t>(NumOr(*regular, "end", 0.0));
    }
}

// "2026-09-21T16:00:00-04:00" -> Unix seconds, and the offset in seconds.
bool ParseIsoDateTime(const std::string& text, int64_t& unixTime, int32_t& offsetSec) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, oh = 0, om = 0;
    char sign = '+';
    const int n = sscanf_s(text.c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d", &y, &mo, &d, &h, &mi, &s, &sign, 1u, &oh, &om);
    if (n < 6 || y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31) { return false; }
    tm p{};
    p.tm_year = y - 1900;
    p.tm_mon  = mo - 1;
    p.tm_mday = d;
    p.tm_hour = h;
    p.tm_min  = mi;
    p.tm_sec  = s;
    const __time64_t local = _mkgmtime64(&p);
    if (local < 0) { return false; }
    offsetSec = (n == 9) ? (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1) : 0;
    unixTime  = static_cast<int64_t>(local) - offsetSec;
    return true;
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

// events.dividends is an object keyed by timestamp: {"1700000000":{"amount":1.38,"date":1700000000}}.
void ParseDividends(const json& r, QuoteData& out) {
    out.dividendCount = 0;
    const json* events = Child(r, "events");
    const json* divs   = (events != nullptr) ? Child(*events, "dividends") : nullptr;
    if (divs == nullptr || !divs->is_object()) { return; }
    for (auto it = divs->begin(); it != divs->end() && out.dividendCount < kMaxDividends; ++it) {
        const json& d = it.value();
        const double amount = NumOr(d, "amount", 0.0);
        const double date   = NumOr(d, "date", 0.0);
        if (amount <= 0.0 || date <= 0.0) { continue; }
        out.dividends[out.dividendCount] = Dividend{ static_cast<int64_t>(date), amount };
        ++out.dividendCount;
    }
    // Keys are strings, so object order is lexical; the app wants time order.
    std::sort(out.dividends.begin(), out.dividends.begin() + static_cast<std::ptrdiff_t>(out.dividendCount),
              [](const Dividend& a, const Dividend& b) { return a.time < b.time; });
    assert(out.dividendCount <= kMaxDividends);
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
    ParseDividends(r, out);
    out.valid = true;
    assert(err.empty() || !out.valid);
    return true;
}

bool ParseTmxJson(const char* data, size_t len, QuoteData& out, std::wstring& err) {
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
    const json* errors = Child(doc, "errors");
    if (errors != nullptr && errors->is_array() && !errors->empty()) {
        err = L"TMX: " + StrOr((*errors)[0], "message", L"unknown error");
        return false;
    }
    const json* d    = Child(doc, "data");
    const json* bars = (d != nullptr) ? Child(*d, "getTimeSeriesData") : nullptr;
    if (bars == nullptr || !bars->is_array()) {
        err = L"TMX: response has no time series";
        return false;
    }
    if (bars->empty()) {
        err = L"TMX: no data for this symbol";
        return false;
    }
    // Newest first in the reply; keep the newest kMaxPoints and store ascending.
    const size_t take = min(bars->size(), kMaxPoints);
    size_t n = 0;
    for (size_t i = 0; i < take; ++i) {
        const json& b = (*bars)[i];
        const json* dt = Child(b, "dateTime");
        Candle c;
        int32_t offset = 0;
        if (dt == nullptr || !dt->is_string() || !ParseIsoDateTime(dt->get<std::string>(), c.time, offset)) { continue; }
        c.open   = NumOr(b, "open", 0.0);
        c.high   = NumOr(b, "high", 0.0);
        c.low    = NumOr(b, "low", 0.0);
        c.close  = NumOr(b, "close", 0.0);
        c.volume = NumOr(b, "volume", 0.0);
        if (c.close <= 0.0) { continue; }
        out.series.pts[take - 1 - i] = c;    // reversed into place
        if (n == 0) { out.meta.gmtOffsetSec = offset; }
        ++n;
    }
    // Gaps left by skipped bars sit at the front; slide the good ones down.
    size_t w = 0;
    for (size_t r = 0; r < take; ++r) {
        if (out.series.pts[r].close <= 0.0) { continue; }
        if (w != r) { out.series.pts[w] = out.series.pts[r]; }
        ++w;
    }
    out.series.count = w;
    if (w == 0) {
        err = L"TMX: no usable bars";
        return false;
    }
    const Candle& last = out.series.pts[w - 1];
    out.meta.price      = last.close;
    out.meta.dayHigh    = last.high;
    out.meta.dayLow     = last.low;
    out.meta.dayVolume  = last.volume;
    out.meta.marketTime = last.time;
    out.meta.exchange   = L"TMX Money (fallback)";
    out.source          = L"TMX";
    out.valid = true;
    assert(out.series.count <= kMaxPoints);
    return true;
}

std::wstring TmxSymbol(const std::wstring& yahooSymbol) {
    std::wstring s = yahooSymbol;
    if (s.empty() || s.size() > 24) { return {}; }
    if (s == L"^GSPTSE") { return L"^TSX"; }
    if (s == L"^GSPC")   { return L"^SPX"; }
    if (s[0] == L'^' || s.find(L'=') != std::wstring::npos || s.rfind(L"0P", 0) == 0) { return {}; }
    static const wchar_t* const kCanadian[] = { L".TO", L".V", L".CN", L".NE" };
    bool canadian = false;
    for (const wchar_t* suffix : kCanadian) {
        const size_t n = wcslen(suffix);
        if (s.size() > n && s.compare(s.size() - n, n, suffix) == 0) {
            s.erase(s.size() - n);
            canadian = true;
            break;
        }
    }
    if (!canadian && s.find(L'.') != std::wstring::npos) { return {}; }   // some other exchange suffix
    for (wchar_t& c : s) { if (c == L'-') { c = L'.'; } }               // share classes: RCI-B -> RCI.B
    return canadian ? s : s + L":US";
}

namespace {

// ---- A small, bounded RSS reader ------------------------------------------
// Google News feeds are plain RSS 2.0 with escaped text; this handles the
// elements the pane needs without pulling in an XML library.

using Sv = std::string_view;

// The text between <tag ...> and </tag> inside `scope`, or empty.
Sv ElementText(Sv scope, const char* tag) {
    const std::string open = std::string("<") + tag;
    const size_t start = scope.find(open);
    if (start == Sv::npos) { return {}; }
    const size_t gt = scope.find('>', start);
    if (gt == Sv::npos) { return {}; }
    if (scope[gt - 1] == '/') { return {}; }   // self-closing
    const std::string close = std::string("</") + tag + ">";
    const size_t end = scope.find(close, gt + 1);
    if (end == Sv::npos) { return {}; }
    return scope.substr(gt + 1, end - gt - 1);
}

// Strips CDATA wrappers and decodes the XML entities that occur in feeds.
std::string DecodeXml(Sv text) {
    std::string s(text);
    if (s.rfind("<![CDATA[", 0) == 0 && s.size() >= 12 && s.compare(s.size() - 3, 3, "]]>") == 0) {
        s = s.substr(9, s.size() - 12);
    }
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size() && i < 65536; ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += s[i]; continue; }
        const Sv ent(s.data() + i + 1, semi - i - 1);
        if (ent == "amp")       { out += '&'; }
        else if (ent == "lt")   { out += '<'; }
        else if (ent == "gt")   { out += '>'; }
        else if (ent == "quot") { out += '"'; }
        else if (ent == "apos") { out += '\''; }
        else if (!ent.empty() && ent[0] == '#') {
            const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            const unsigned long cp = strtoul(std::string(ent.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
            if (cp == 0 || cp > 0x10FFFF) { out += '?'; }
            else if (cp < 0x80) { out += static_cast<char>(cp); }
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        } else { out += s[i]; continue; }
        i = semi;
    }
    return out;
}

// RFC 822 date as used by RSS: "Sat, 20 Sep 2026 12:34:56 GMT". 0 if unparseable.
int64_t ParseRfc822(const std::string& text) {
    static const char* kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int day = 0;
    int year = 0;
    int hh = 0;
    int mm = 0;
    int ss = 0;
    std::array<char, 8> mon{};
    // With or without the leading weekday.
    int n = sscanf_s(text.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, mon.data(), 4, &year, &hh, &mm, &ss);
    if (n < 5) { n = sscanf_s(text.c_str(), "%d %3s %d %d:%d:%d", &day, mon.data(), 4, &year, &hh, &mm, &ss); }
    if (n < 5) { return 0; }
    int month = -1;
    for (int i = 0; i < 12; ++i) {
        if (_strnicmp(mon.data(), kMonths[i], 3) == 0) { month = i; break; }
    }
    if (month < 0 || day < 1 || day > 31 || year < 1970 || year > 2200) { return 0; }
    tm t{};
    t.tm_mday = day;
    t.tm_mon  = month;
    t.tm_year = year - 1900;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    const __time64_t utc = _mkgmtime64(&t);
    return (utc < 0) ? 0 : static_cast<int64_t>(utc);
}

} // namespace

bool ParseNewsRss(const char* data, size_t len,
                  std::array<NewsItem, kMaxNews>& out, size_t& count, std::wstring& err) {
    assert(data != nullptr);
    count = 0;
    if (len == 0) {
        err = L"Empty response";
        return false;
    }
    const Sv doc(data, len);
    if (doc.find("<rss") == Sv::npos && doc.find("<channel") == Sv::npos) {
        err = L"Response is not an RSS feed";
        return false;
    }
    size_t pos = 0;
    for (size_t guard = 0; guard < 256 && count < kMaxNews; ++guard) {
        const size_t start = doc.find("<item", pos);
        if (start == Sv::npos) { break; }
        const size_t end = doc.find("</item>", start);
        if (end == Sv::npos) { break; }
        const Sv item = doc.substr(start, end - start);
        pos = end + 7;
        NewsItem& h = out[count];
        h = NewsItem{};
        std::string title = DecodeXml(ElementText(item, "title"));
        if (title.empty()) { continue; }
        h.publisher = Utf8ToWide(DecodeXml(ElementText(item, "source")));
        // Google titles end in " - Publisher"; drop it when the source element already says so.
        if (!h.publisher.empty()) {
            const std::string suffix = " - " + std::string(DecodeXml(ElementText(item, "source")));
            if (title.size() > suffix.size() && title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0) {
                title.resize(title.size() - suffix.size());
            }
        }
        h.title = Utf8ToWide(title);
        h.link  = Utf8ToWide(DecodeXml(ElementText(item, "link")));
        h.time  = ParseRfc822(DecodeXml(ElementText(item, "pubDate")));
        ++count;
    }
    assert(count <= kMaxNews);
    return true;
}

bool ParseNewsJson(const char* data, size_t len,
                   std::array<NewsItem, kMaxNews>& out, size_t& count, std::wstring& err) {
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
    const json* news = Child(doc, "news");
    if (news == nullptr || !news->is_array()) {
        const json* fin  = Child(doc, "finance");
        const json* ferr = (fin != nullptr) ? Child(*fin, "error") : nullptr;
        err = (ferr != nullptr) ? L"Provider: " + StrOr(*ferr, "description", L"unknown error")
                                : L"Response has no news block";
        return false;
    }
    const size_t n = min(news->size(), kMaxNews);
    for (size_t i = 0; i < n; ++i) {
        const json& item = (*news)[i];
        NewsItem& h = out[count];
        h = NewsItem{};
        h.title = StrOr(item, "title", L"");
        if (h.title.empty()) { continue; }
        h.publisher = StrOr(item, "publisher", L"");
        h.link      = StrOr(item, "link", L"");
        h.time      = static_cast<int64_t>(NumOr(item, "providerPublishTime", 0.0));
        ++count;
    }
    assert(count <= kMaxNews);
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
