cmake_minimum_required(VERSION 3.25)
# Single source of truth for the version: it flows into the code
# (STOCKTOOL_VERSION*) and the exe's VERSIONINFO resource from here.
project(StockTool VERSION 0.7.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match the vcpkg x64-windows-static triplet (static CRT).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Catch2 CONFIG)

set(ST_DEFINES
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    $<$<COMPILE_LANGUAGE:CXX>:STOCKTOOL_VERSION="${PROJECT_VERSION}">
    STOCKTOOL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    STOCKTOOL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    STOCKTOOL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Power of 10: strictest warnings, treated as errors. Third-party headers
# (vcpkg) are pulled in as SYSTEM includes so /external:W0 silences them.
set(ST_OPTIONS /W4 /WX /permissive- /utf-8 /EHsc /external:W0 /Zc:__cplusplus)

# ---- UI-free core: parsing, config, formatting, indicators (unit-tested) ----
add_library(stocktool_core STATIC
    src/brokerimport.cpp
    src/chart.cpp
    src/config.cpp
    src/history.cpp
    src/http.cpp
    src/indicators.cpp
    src/position.cpp
    src/quote_parser.cpp
    src/textfmt.cpp
    src/theme.cpp
)
target_include_directories(stocktool_core PUBLIC src)
target_compile_definitions(stocktool_core PUBLIC ${ST_DEFINES})
target_compile_options(stocktool_core PRIVATE ${ST_OPTIONS})
target_link_libraries(stocktool_core PUBLIC nlohmann_json::nlohmann_json winhttp gdiplus shell32 advapi32)

# ---- Application -----------------------------------------------------------
add_executable(StockTool WIN32
    src/main.cpp
    src/app.cpp
    src/dialogs.cpp
    src/fetcher.cpp
    src/help.cpp
    src/StockTool.rc
)
target_compile_options(StockTool PRIVATE ${ST_OPTIONS})
target_link_libraries(StockTool PRIVATE
    stocktool_core
    comctl32 comdlg32 dwmapi uxtheme
)

# Seed the default config next to the executable on the first build only:
# the app writes holdings, alerts and window state back into that file, so
# rebuilding must not overwrite it.
add_custom_command(TARGET StockTool POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/stocktool.cfg
        -DDST=$<TARGET_FILE_DIR:StockTool>/stocktool.cfg
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake
)

# ---- Tests -----------------------------------------------------------------
if(Catch2_FOUND)
    enable_testing()
    add_executable(stocktool_tests
        tests/test_brokerimport.cpp
        tests/test_chart_geometry.cpp
        tests/test_config.cpp
        tests/test_history.cpp
        tests/test_indicators.cpp
        tests/test_position.cpp
        tests/test_quote_parser.cpp
        tests/test_textfmt.cpp
    )
    target_compile_options(stocktool_tests PRIVATE ${ST_OPTIONS})
    target_link_libraries(stocktool_tests PRIVATE stocktool_core Catch2::Catch2WithMain)
    add_test(NAME stocktool_tests COMMAND stocktool_tests)
else()
    message(STATUS "Catch2 not found - tests disabled")
endif()
