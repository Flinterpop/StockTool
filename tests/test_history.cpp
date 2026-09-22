// Portfolio history CSV round-trips.
#include "history.h"
#include "textfmt.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <array>
#include <fstream>

using namespace st;
using Catch::Approx;

namespace {

struct TempCsv {
    std::wstring path;
    TempCsv() {
        std::array<wchar_t, MAX_PATH> dir{};
        GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
        std::array<wchar_t, MAX_PATH> file{};
        GetTempFileNameW(dir.data(), L"sth", 0, file.data());
        path = file.data();
        DeleteFileW(path.c_str());   // start from "missing"
    }
    ~TempCsv() { DeleteFileW(path.c_str()); }
    std::string Read() const {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
};

int64_t Day(const wchar_t* iso) {
    int64_t t = 0;
    REQUIRE(ParseIsoDate(iso, t));
    return t;
}

} // namespace

TEST_CASE("history path sits beside the config") {
    CHECK(HistoryPath(L"C:\\x\\stocktool.cfg") == L"C:\\x\\portfolio-history.csv");
    CHECK(HistoryPath(L"stocktool.cfg") == L"portfolio-history.csv");
}

TEST_CASE("a missing history file is an empty history") {
    TempCsv f;
    History h;
    std::wstring err;
    REQUIRE(LoadHistory(f.path, L"", h, err));
    CHECK(h.count == 0);
}

TEST_CASE("rows are recorded per list, replaced for the same day, and read back ascending") {
    TempCsv f;
    std::wstring err;
    REQUIRE(RecordHistory(f.path, L"", HistoryPoint{ Day(L"2026-09-19"), 1000.0, 800.0 }, L"CAD", err));
    REQUIRE(RecordHistory(f.path, L"", HistoryPoint{ Day(L"2026-09-21"), 1100.0, 800.0 }, L"CAD", err));
    REQUIRE(RecordHistory(f.path, L"Banks", HistoryPoint{ Day(L"2026-09-21"), 50.0, 40.0 }, L"CAD", err));
    REQUIRE(RecordHistory(f.path, L"", HistoryPoint{ Day(L"2026-09-20"), 1050.0, 800.0 }, L"CAD", err));
    REQUIRE(RecordHistory(f.path, L"", HistoryPoint{ Day(L"2026-09-21"), 1120.0, 810.0 }, L"CAD", err));   // same day again

    const std::string text = f.Read();
    CHECK(text.rfind("date,list,value,cost,currency\r\n", 0) == 0);
    CHECK(text.find("2026-09-21,(default),1120.00,810.00,CAD") != std::string::npos);
    CHECK(text.find("2026-09-21,(default),1100.00") == std::string::npos);   // replaced
    CHECK(text.find("2026-09-21,Banks,50.00,40.00,CAD") != std::string::npos);

    History h;
    REQUIRE(LoadHistory(f.path, L"", h, err));
    REQUIRE(h.count == 3);
    CHECK(h.pts[0].day == Day(L"2026-09-19"));
    CHECK(h.pts[1].day == Day(L"2026-09-20"));
    CHECK(h.pts[2].day == Day(L"2026-09-21"));
    CHECK(h.pts[2].value == Approx(1120.0));
    CHECK(h.pts[2].cost == Approx(810.0));

    History banks;
    REQUIRE(LoadHistory(f.path, L"Banks", banks, err));
    REQUIRE(banks.count == 1);
    CHECK(banks.pts[0].value == Approx(50.0));
}

TEST_CASE("out-of-order file rows: the later line for a day wins, earlier days after later ones are skipped") {
    TempCsv f;
    {
        std::ofstream o(f.path, std::ios::binary);
        o << "date,list,value,cost,currency\r\n"
             "2026-09-20,(default),10,5,CAD\r\n"
             "2026-09-20,(default),11,5,CAD\r\n"
             "2026-09-18,(default),9,5,CAD\r\n"
             "garbage line\r\n"
             "2026-09-22,(default),12,5,CAD\r\n";
    }
    History h;
    std::wstring err;
    REQUIRE(LoadHistory(f.path, L"", h, err));
    REQUIRE(h.count == 2);
    CHECK(h.pts[0].value == Approx(11.0));
    CHECK(h.pts[1].value == Approx(12.0));
}

TEST_CASE("local calendar day is midnight UTC of the local date") {
    const int64_t t = Day(L"2026-09-21") + 15 * 3600;   // 15:00 UTC: the same date anywhere from UTC-15 to UTC+8
    CHECK(LocalCalendarDay(t) == Day(L"2026-09-21"));
    CHECK(LocalCalendarDay(t) % 86400 == 0);
}
