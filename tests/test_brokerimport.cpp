// Broker CSV import: header detection, activity and holdings files, symbol mapping.
#include "brokerimport.h"
#include "textfmt.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <array>
#include <fstream>

using namespace st;
using Catch::Approx;

namespace {

int64_t Day(const wchar_t* iso) {
    int64_t t = 0;
    REQUIRE(ParseIsoDate(iso, t));
    return t;
}

// A scratch config file that is deleted when the test ends.
struct TempIni {
    std::wstring path;
    TempIni() {
        std::array<wchar_t, MAX_PATH> dir{};
        GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
        std::array<wchar_t, MAX_PATH> file{};
        GetTempFileNameW(dir.data(), L"sti", 0, file.data());
        path = file.data();
    }
    ~TempIni() { DeleteFileW(path.c_str()); }
    void Write(const char* text) const {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << text;
    }
};

constexpr char kCfg[] =
    "[stocks]\r\n"
    "RY.TO=Royal Bank of Canada\r\n"
    "[stocks.Banks]\r\n"
    "TD.TO=Toronto-Dominion Bank\r\n"
    "RY.TO=Royal Bank of Canada\r\n"
    "[symbol_map]\r\n"
    "TDB902=0P0000A30L\r\n"
    "bad=not a symbol!\r\n"
    "[transactions]\r\n"
    "RY.TO.1=2026-03-12,100.0000,132.5000\r\n";

} // namespace

TEST_CASE("all-list symbols and the symbol map are read from the config") {
    TempIni ini;
    ini.Write(kCfg);
    std::array<std::wstring, kMaxKnownSymbols> known{};
    const size_t n = ReadAllSymbols(ini.path, known.data(), known.size());
    REQUIRE(n == 2);                                    // RY.TO once, TD.TO once
    CHECK(known[0] == L"RY.TO");
    CHECK(known[1] == L"TD.TO");
    std::array<SymbolMapEntry, kMaxSymbolMap> map{};
    const size_t m = ReadSymbolMap(ini.path, map.data(), map.size());
    REQUIRE(m == 1);                                    // the malformed line is dropped
    CHECK(map[0].from == L"TDB902");
    CHECK(map[0].to == L"0P0000A30L");
}

TEST_CASE("an activity import merges with existing transactions and can add unknown symbols") {
    TempIni ini;
    ini.Write(kCfg);
    const std::string csv =
        "Trade Date,Transaction Type,Description,Symbol,Quantity,Price\n"
        "03/12/2026,Buy,ROYAL BANK,RY,100,132.50\n"          // already in the config
        "06/12/2026,Sell,ROYAL BANK,RY,40,175.00\n"
        "06/01/2026,Buy,TD US INDEX E,TDB902,50,20.00\n"
        "06/02/2026,Buy,TORONTO DOMINION,TD,10,80.00\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    ImportPlan plan;
    BuildImportPlan(r, ini.path, plan);
    REQUIRE(plan.count == 3);
    CHECK_FALSE(plan.holdings);
    CHECK(plan.items[0].symbol == L"RY.TO");
    CHECK(plan.items[0].known);
    CHECK(plan.items[0].dupes == 1);
    CHECK(plan.items[0].added == 1);
    CHECK(plan.items[0].txCount == 2);
    CHECK(plan.items[1].broker == L"TDB902");
    CHECK(plan.items[1].symbol == L"0P0000A30L");       // via [symbol_map]
    CHECK_FALSE(plan.items[1].known);
    CHECK(plan.items[2].symbol == L"TD.TO");             // base match in the Banks list
    CHECK(plan.items[2].known);

    const std::wstring text = DescribeImportPlan(plan);
    CHECK(text.find(L"3 symbols") != std::wstring::npos);
    CHECK(text.find(L"1 already recorded") != std::wstring::npos);
    CHECK(text.find(L" * TDB902 -> 0P0000A30L") != std::wstring::npos);

    // Without the add box the unknown fund is skipped; with it, it joins the active list.
    Config cfg;
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    size_t written = 0;
    REQUIRE(ApplyImportPlan(plan, cfg, false, written, err));
    CHECK(written == 2);
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    REQUIRE(cfg.stockCount == 1);
    CHECK(cfg.stocks[0].txCount == 2);
    CHECK(cfg.stocks[0].tx[1].qty == Approx(-40.0));

    REQUIRE(ApplyImportPlan(plan, cfg, true, written, err));
    CHECK(written == 3);
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    REQUIRE(cfg.stockCount == 2);
    CHECK(cfg.stocks[1].symbol == L"0P0000A30L");
    CHECK(cfg.stocks[1].name == L"TD US INDEX E");
    CHECK(cfg.stocks[1].txCount == 1);
    CHECK(cfg.stocks[1].tx[0].price == Approx(20.0));

    // Importing the same file again changes nothing.
    ImportPlan again;
    BuildImportPlan(r, ini.path, again);
    for (size_t i = 0; i < again.count; ++i) { CHECK(again.items[i].added == 0); }
    REQUIRE(LoadConfig(ini.path, L"Banks", cfg, err));
    REQUIRE(cfg.stockCount == 2);
    CHECK(cfg.stocks[0].txCount == 1);                   // TD.TO's buy, seen from the other list too
}

TEST_CASE("a real WebBroker holdings export: preamble, cash, fund code, long quantities") {
    // The shape TD Direct Investing actually produces, values changed.
    const std::string csv =
        "As of Date,2026-09-22 14:54:54\r\n"
        "Account,TD Direct Investing - 20X0X0X\r\n"
        "Cash,22747.06\r\n"
        "Investments,16514.49\r\n"
        "Total Value,39261.55\r\n"
        "Margin,,\r\n"
        ",\r\n"
        "Symbol,Market,Description,Quantity,Average Cost,Price,Book Cost,Market Value,Unrealized $,Unrealized %,"
        "% of Positions,Loan Value,Change Today $,Change Today %,Bid,Bid Lots,Ask,Ask Lots,Volume,Day Low,Day High,"
        "52-wk Low,52-wk High\r\n"
        "BN,CA,\"BROOKFIELD CORP CL-A LVS\",3735.52484000000000,53.5427,53.55,200009.99,200037.36,27.37,0.01,40.71,,"
        "0.18,0.34,53.5400,18,53.5600,16,1481504,53.1600,54.0600,51.3100,68.4350\r\n"
        "TDB900,,\"TD CDN INDX -E   /NL'FRAC\",266.79300000000000,35.0415,61.90,9348.82,16514.49,7165.67,76.65,42.06,,,,,,,,,,,,\r\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    CHECK(r.holdings);
    CHECK(r.hasCash);
    CHECK(r.cash == Approx(22747.06));          // from the preamble, not a position row
    REQUIRE(r.count == 2);
    CHECK(r.rows[0].symbol == L"BN");
    CHECK(r.rows[0].market == L"CA");
    CHECK(r.rows[0].tx.qty == Approx(3735.52484));
    CHECK(r.rows[0].tx.price == Approx(53.5427));   // Average Cost, not the market Price
    CHECK(r.rows[1].symbol == L"TDB900");
    CHECK(r.rows[1].market.empty());                 // funds carry no market
    CHECK(r.rows[1].tx.price == Approx(35.0415));
}

TEST_CASE("cash from the preamble is written to the active list and read back") {
    TempIni ini;
    ini.Write(kCfg);
    const std::string csv =
        "Cash,1234.50\n"
        "Symbol,Market,Quantity,Average Cost\n"
        "RY,CA,10,100.00\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    ImportPlan plan;
    BuildImportPlan(r, ini.path, plan);
    CHECK(plan.hasCash);
    CHECK(plan.cash == Approx(1234.50));
    CHECK(DescribeImportPlan(plan).find(L"Uninvested cash 1,234.50") != std::wstring::npos);

    Config cfg;
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    CHECK(cfg.cash == 0.0);
    size_t written = 0;
    REQUIRE(ApplyImportPlan(plan, cfg, false, written, err));
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    CHECK(cfg.cash == Approx(1234.50));

    // Cash belongs to the list it was imported into, not to every list.
    Config banks;
    REQUIRE(LoadConfig(ini.path, L"Banks", banks, err));
    CHECK(banks.cash == 0.0);
    REQUIRE(WriteCash(ini.path, L"Banks", 99.0, err));
    REQUIRE(LoadConfig(ini.path, L"Banks", banks, err));
    CHECK(banks.cash == Approx(99.0));
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    CHECK(cfg.cash == Approx(1234.50));

    REQUIRE(WriteCash(ini.path, L"", 0.0, err));     // zero clears the line
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    CHECK(cfg.cash == 0.0);
}

TEST_CASE("a holdings import writes [holdings] for known symbols") {
    TempIni ini;
    ini.Write(kCfg);
    const std::string csv =
        "Symbol,Market,Quantity,Average Cost\n"
        "RY,CA,60,132.50\n"
        "MSFT,US,5,400.00\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    ImportPlan plan;
    BuildImportPlan(r, ini.path, plan);
    REQUIRE(plan.count == 2);
    CHECK(plan.holdings);
    CHECK(plan.items[0].holding);
    CHECK(plan.items[1].symbol == L"MSFT");
    CHECK_FALSE(plan.items[1].known);
    Config cfg;
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    size_t written = 0;
    REQUIRE(ApplyImportPlan(plan, cfg, false, written, err));
    CHECK(written == 1);
    REQUIRE(LoadConfig(ini.path, L"", cfg, err));
    CHECK(cfg.stocks[0].holding.qty == Approx(60.0));
    CHECK(cfg.stocks[0].holding.cost == Approx(132.5));
}

TEST_CASE("activity export: preamble, quoted names, money formatting, non-trades skipped") {
    const std::string csv =
        "\xEF\xBB\xBF" "Account: TFSA 123456\r\n"
        "As of: September 20 2026\r\n"
        "\r\n"
        "Trade Date,Settlement Date,Transaction Type,Description,Symbol,Quantity,Price,Commission,Net Amount\r\n"
        "03/12/2026,03/14/2026,Buy,\"ROYAL BANK OF CANADA, COMMON\",RY,100,\"$132.50\",9.99,\"($13,259.99)\"\r\n"
        "2026-06-12,2026-06-14,Sell,\"ROYAL BANK OF CANADA, COMMON\",RY,40,175.00,9.99,\"$6,990.01\"\r\n"
        "04/01/2026,04/01/2026,Dividend,\"ROYAL BANK OF CANADA, COMMON\",RY,100,,,$148.00\r\n"
        "04/02/2026,04/02/2026,Interest,\"CASH\",,0,,,$1.23\r\n"
        "05/05/2026,05/05/2026,Reinvestment,\"BANK OF MONTREAL\",BMO,1.234,\"$101.10\",,\"($124.76)\"\r\n"
        "06/01/2026,06/03/2026,Buy,\"TD US INDEX FUND - E\",TDB902,50,,, \"($2,500.00)\"\r\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    CHECK_FALSE(r.holdings);
    REQUIRE(r.count == 4);
    CHECK(r.skipped == 2);

    CHECK(r.rows[0].symbol == L"RY");
    CHECK(r.rows[0].name == L"ROYAL BANK OF CANADA, COMMON");
    CHECK(r.rows[0].tx.date == Day(L"2026-03-12"));
    CHECK(r.rows[0].tx.qty == Approx(100.0));
    CHECK(r.rows[0].tx.price == Approx(132.50));

    CHECK(r.rows[1].tx.date == Day(L"2026-06-12"));
    CHECK(r.rows[1].tx.qty == Approx(-40.0));      // a sell is negative
    CHECK(r.rows[1].tx.price == Approx(175.0));

    CHECK(r.rows[2].symbol == L"BMO");               // DRIP counts as a buy
    CHECK(r.rows[2].tx.qty == Approx(1.234));

    CHECK(r.rows[3].symbol == L"TDB902");            // no price: net amount / quantity
    CHECK(r.rows[3].tx.price == Approx(50.0));
}

TEST_CASE("activity export without a type column infers buys and sells from the description or sign") {
    const std::string csv =
        "Date,Description,Symbol,Quantity,Price\n"
        "2026-01-05,BOUGHT 10 SHARES,ABC,10,5.00\n"
        "2026-01-06,SOLD 4 SHARES,ABC,4,6.00\n"
        "2026-01-07,,ABC,-2,7.00\n"
        "2026-01-08,CASH DIVIDEND,ABC,10,0.10\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    REQUIRE(r.count == 3);
    CHECK(r.rows[0].tx.qty == Approx(10.0));
    CHECK(r.rows[1].tx.qty == Approx(-4.0));
    CHECK(r.rows[2].tx.qty == Approx(-2.0));
    CHECK(r.skipped == 1);
}

TEST_CASE("holdings export: quantity and average cost, or book value divided by quantity") {
    const std::string csv =
        "Symbol,Market,Description,Quantity,Average Cost,Book Value,Market Price,Market Value\n"
        "RY,CA,ROYAL BANK OF CANADA,60,\"$132.50\",\"$7,950.00\",284.45,\"$17,067.00\"\n"
        "AAPL,US,APPLE INC,10,,\"$1,500.00\",230.00,\"$2,300.00\"\n"
        "Total,,,,,,,\"$19,367.00\"\n";
    ImportResult r;
    std::wstring err;
    REQUIRE(ParseBrokerCsv(csv, r, err));
    CHECK(r.holdings);
    REQUIRE(r.count == 2);
    CHECK(r.rows[0].isHolding);
    CHECK(r.rows[0].market == L"CA");
    CHECK(r.rows[0].tx.qty == Approx(60.0));
    CHECK(r.rows[0].tx.price == Approx(132.50));
    CHECK(r.rows[1].tx.price == Approx(150.0));
    CHECK(r.skipped == 1);                            // the Total line
}

TEST_CASE("a file without a symbol/quantity header is rejected") {
    ImportResult r;
    std::wstring err;
    CHECK_FALSE(ParseBrokerCsv("just,some,text\n1,2,3\n", r, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(ParseBrokerCsv("", r, err));
}

TEST_CASE("broker numbers and dates in the formats exports use") {
    double v = 0.0;
    CHECK((ParseBrokerNumber(L"$1,234.50", v) && v == Approx(1234.5)));
    CHECK((ParseBrokerNumber(L"(12.5)", v) && v == Approx(-12.5)));
    CHECK((ParseBrokerNumber(L"-7", v) && v == Approx(-7.0)));
    CHECK((ParseBrokerNumber(L"1 234,00 CAD", v) && v == Approx(123400.0)));   // no locale guessing
    CHECK_FALSE(ParseBrokerNumber(L"-", v));
    CHECK_FALSE(ParseBrokerNumber(L"", v));

    int64_t t = 0;
    CHECK((ParseBrokerDate(L"2026-03-12", t) && t == Day(L"2026-03-12")));
    CHECK((ParseBrokerDate(L"2026/03/12", t) && t == Day(L"2026-03-12")));
    CHECK((ParseBrokerDate(L"03/12/2026", t) && t == Day(L"2026-03-12")));   // MM/DD/YYYY
    CHECK((ParseBrokerDate(L"25/03/2026", t) && t == Day(L"2026-03-25")));   // DD/MM/YYYY when unambiguous
    CHECK((ParseBrokerDate(L"12 Mar 2026", t) && t == Day(L"2026-03-12")));
    CHECK((ParseBrokerDate(L"12-MAR-2026", t) && t == Day(L"2026-03-12")));
    CHECK((ParseBrokerDate(L"Mar 12, 2026", t) && t == Day(L"2026-03-12")));
    CHECK((ParseBrokerDate(L"March 12 2026", t) && t == Day(L"2026-03-12")));
    CHECK_FALSE(ParseBrokerDate(L"", t));
    CHECK_FALSE(ParseBrokerDate(L"soon", t));
    CHECK_FALSE(ParseBrokerDate(L"2026-02-30", t));
}

TEST_CASE("symbol mapping: map section, watch-list match, market suffix") {
    ImportContext ctx;
    ctx.known[0] = L"RY.TO";
    ctx.known[1] = L"AAPL";
    ctx.known[2] = L"0P0000A30L";
    ctx.known[3] = L"BRK-B";
    ctx.knownCount = 4;
    ctx.map[0] = SymbolMapEntry{ L"TDB902", L"0P0000A30L" };
    ctx.mapCount = 1;

    CHECK(MapBrokerSymbol(L"tdb902", L"", ctx) == L"0P0000A30L");     // explicit map wins
    CHECK(MapBrokerSymbol(L"RY", L"CA", ctx) == L"RY.TO");             // base match in a list
    CHECK(MapBrokerSymbol(L"RY", L"", ctx) == L"RY.TO");
    CHECK(MapBrokerSymbol(L"AAPL", L"US", ctx) == L"AAPL");
    CHECK(MapBrokerSymbol(L"BRK-B", L"US", ctx) == L"BRK-B");
    CHECK(MapBrokerSymbol(L"TD", L"CA", ctx) == L"TD.TO");             // unknown Canadian: .TO
    CHECK(MapBrokerSymbol(L"XYZ", L"TSXV", ctx) == L"XYZ.V");
    CHECK(MapBrokerSymbol(L"MSFT", L"US", ctx) == L"MSFT");            // unknown US: as is
    CHECK(MapBrokerSymbol(L"MSFT", L"", ctx) == L"MSFT");
    CHECK(MapBrokerSymbol(L"RY", L"US", ctx) == L"RY");                // US listing, not the TSX one
    CHECK(MapBrokerSymbol(L"", L"CA", ctx).empty());
}

TEST_CASE("text decoding: UTF-8 BOM, UTF-16 and ANSI all come back as UTF-8") {
    CHECK(DecodeTextFile("\xEF\xBB\xBF" "Symbol") == "Symbol");
    CHECK(DecodeTextFile(std::string("\xFF\xFE" "S\0y\0m\0", 8)) == "Sym");
    CHECK(DecodeTextFile("Caf\xE9") == "Caf\xC3\xA9");                  // CP1252 e-acute
    CHECK(DecodeTextFile("").empty());
}
