cmake_minimum_required(VERSION 3.25)
project(StockTool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match the vcpkg x64-windows-static triplet (static CRT).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(StockTool WIN32
    src/main.cpp
    src/app.cpp
    src/chart.cpp
    src/config.cpp
    src/fetcher.cpp
    src/http.cpp
    src/quote_parser.cpp
    src/textfmt.cpp
)

target_compile_definitions(StockTool PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# Power of 10: strictest warnings, treated as errors. Third-party headers
# (vcpkg) are pulled in as SYSTEM includes so /external:W0 silences them.
target_compile_options(StockTool PRIVATE
    /W4 /WX /permissive- /utf-8 /EHsc /external:W0 /Zc:__cplusplus
)

target_link_libraries(StockTool PRIVATE
    nlohmann_json::nlohmann_json
    winhttp gdiplus comctl32
)

# Ship the default config next to the executable.
add_custom_command(TARGET StockTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/stocktool.cfg" "$<TARGET_FILE_DIR:StockTool>/stocktool.cfg"
)
