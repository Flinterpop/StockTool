// Config round-trips through a temporary INI file.
#include "config.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>

using namespace st;
using Catch::Approx;

namespace {

// A scratch file that is deleted when the test ends.
struct TempIni {
    std::wstring path;
    TempIni() {
        std::array<wchar_t, MAX_PATH> dir{};
        GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
        std::array<wchar_t, MAX_PATH> file{};
        GetTempFileNameW(dir.data(), L"stt", 0, file.data());
        path = file.data();
    }
    ~TempIni() { DeleteFileW(path.c_str()); }
    void Write(const char* text) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << text;
    }
};

constexpr char kIni[] =
    "[settings]\r\n"
    "refresh_seconds=5\r\n"          // below the floor: clamped to 10
    "default_range=5y\r\n"           // case-insensitive
    "inset_range=1D\r\n"             // not allowed: falls back to 5Y
    "theme=Dark\r\n"
    "start_minimized=1\r\n"
    "url_template=https://example.test/{symbol}?r={range}&i={interval}\r\n"
    "quote_url_template=\r\n"
    "\r\n"
    "[stocks]\r\n"
    "; a comment line\r\n"
    "RY.TO=Royal Bank of Canada\r\n"
    "DOL.TO=\r\n"
    "\r\n"
    "[holdings]\r\n"
    "RY.TO=100,250.5\r\n"
    "\r\n"
    "[alerts]\r\n"
    "DOL.TO=0,150\r\n";

} // namespace

TEST_CASE("LoadConfig reads settings, stocks, holdings and alerts") {
    TempIni ini;
    ini.Write(kIni);
    Config cfg;
    std::wstring err;
    REQUIRE(LoadConfig(ini.path, cfg, err));
    CHECK(cfg.refreshSeconds == 10u);
    CHECK(cfg.defaultRange == kRange5Y);
    CHECK(cfg.insetRange == kRange5Y);
    CHECK(cfg.theme == ThemeMode::Dark);
    CHECK(cfg.startMinimized);
    CHECK_FALSE(cfg.minimizeToTray);
    CHECK(cfg.urlTemplate == L"https://example.test/{symbol}?r={range}&i={interval}");
    CHECK(cfg.quoteUrlTemplate.empty());

    REQUIRE(cfg.stockCount == 2);
    CHECK(cfg.stocks[0].symbol == L"RY.TO");
    CHECK(cfg.stocks[0].name == L"Royal Bank of Canada");
    CHECK(cfg.stocks[0].holding.qty == Approx(100.0));
    CHECK(cfg.stocks[0].holding.cost == Approx(250.5));
    CHECK(cfg.stocks[1].symbol == L"DOL.TO");
    CHECK(cfg.stocks[1].name == L"DOL.TO");        // blank name falls back to symbol
    CHECK(cfg.stocks[1].alert.above == 0.0);
    CHECK(cfg.stocks[1].alert.below == Approx(150.0));
}

TEST_CASE("LoadConfig rejects a file without stocks or a bad template") {
    TempIni ini;
    Config cfg;
    std::wstring err;
    ini.Write("[settings]\r\nrefresh_seconds=60\r\n");
    CHECK_FALSE(LoadConfig(ini.path, cfg, err));
    CHECK(err.find(L"No stocks") != std::wstring::npos);

    ini.Write("[settings]\r\nurl_template=https://x/no-placeholder\r\n[stocks]\r\nA=A\r\n");
    CHECK_FALSE(LoadConfig(ini.path, cfg, err));
    CHECK(err.find(L"{symbol}") != std::wstring::npos);

    CHECK_FALSE(LoadConfig(L"C:\\does\\not\\exist\\x.cfg", cfg, err));
}

TEST_CASE("Write/Delete/Reorder round-trip through the file") {
    TempIni ini;
    ini.Write(kIni);
    Config cfg;
    std::wstring err;
    REQUIRE(LoadConfig(ini.path, cfg, err));

    StockEntry td;
    td.symbol = L"TD.TO";
    td.name   = L"Toronto-Dominion Bank";
    td.holding = { 10.0, 80.0 };
    td.alert   = { 95.0, 0.0 };
    REQUIRE(WriteStockEntry(ini.path, td, err));
    REQUIRE(LoadConfig(ini.path, cfg, err));
    REQUIRE(cfg.stockCount == 3);
    CHECK(cfg.stocks[2].symbol == L"TD.TO");
    CHECK(cfg.stocks[2].holding.qty == Approx(10.0));
    CHECK(cfg.stocks[2].alert.above == Approx(95.0));

    // Move TD.TO to the front and rewrite the section.
    std::swap(cfg.stocks[0], cfg.stocks[2]);
    REQUIRE(WriteStockOrder(ini.path, cfg, err));
    REQUIRE(LoadConfig(ini.path, cfg, err));
    REQUIRE(cfg.stockCount == 3);
    CHECK(cfg.stocks[0].symbol == L"TD.TO");
    CHECK(cfg.stocks[2].symbol == L"RY.TO");
    CHECK(cfg.stocks[2].holding.qty == Approx(100.0));  // holdings survive a reorder

    REQUIRE(DeleteStockEntry(ini.path, L"TD.TO", err));
    REQUIRE(LoadConfig(ini.path, cfg, err));
    CHECK(cfg.stockCount == 2);
    CHECK(cfg.stocks[0].symbol == L"DOL.TO");

    // Clearing a holding removes the key.
    REQUIRE(WriteHolding(ini.path, L"RY.TO", Holding{}, err));
    REQUIRE(LoadConfig(ini.path, cfg, err));
    CHECK(cfg.stocks[1].holding.qty == 0.0);
}

TEST_CASE("View state round-trips") {
    TempIni ini;
    ini.Write(kIni);
    ViewState s;
    s.hasWindow = true;
    s.window    = { 10, 20, 810, 620 };
    s.maximized = true;
    s.range     = kRange5Y;
    s.candles   = true;
    s.rsi       = true;
    s.inset     = false;
    s.selected  = L"DOL.TO";
    std::wstring err;
    REQUIRE(SaveViewState(ini.path, s, err));
    ViewState back;
    LoadViewState(ini.path, back);
    CHECK(back.hasWindow);
    CHECK(back.window.left == 10);
    CHECK(back.window.right == 810);
    CHECK(back.window.bottom == 620);
    CHECK(back.maximized);
    CHECK(back.range == kRange5Y);
    CHECK(back.candles);
    CHECK_FALSE(back.compare);
    CHECK(back.rsi);
    CHECK_FALSE(back.inset);
    CHECK(back.selected == L"DOL.TO");

    // Saved settings do not disturb LoadConfig.
    Config cfg;
    REQUIRE(LoadConfig(ini.path, cfg, err));
    CHECK(cfg.stockCount == 2);
}

TEST_CASE("NormalizeSymbol and NormalizeName") {
    std::wstring s = L"  td.to ";
    std::wstring err;
    REQUIRE(NormalizeSymbol(s, err));
    CHECK(s == L"TD.TO");
    s = L"brk-b";
    REQUIRE(NormalizeSymbol(s, err));
    CHECK(s == L"BRK-B");
    s = L"^gsptse";
    REQUIRE(NormalizeSymbol(s, err));
    CHECK(s == L"^GSPTSE");
    s = L"ab c";
    CHECK_FALSE(NormalizeSymbol(s, err));
    s = L"";
    CHECK_FALSE(NormalizeSymbol(s, err));
    s = L"A=B;C";
    CHECK_FALSE(NormalizeSymbol(s, err));
    s = std::wstring(40, L'A');
    CHECK_FALSE(NormalizeSymbol(s, err));

    CHECK(NormalizeName(L"  Royal\r\nBank  ", L"X") == L"RoyalBank");
    CHECK(NormalizeName(L"   ", L"FALLBACK") == L"FALLBACK");
    CHECK(NormalizeName(std::wstring(100, L'n'), L"X").size() == 63);
}
