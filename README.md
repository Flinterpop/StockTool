# StockTool

[![Release][release-badge]][release-latest]

[release-badge]: https://img.shields.io/badge/release-v0.1.0-blue
[release-latest]: https://github.com/Flinterpop/StockTool/releases/latest

*Last updated: 19 Sep 2026*

A small Win32 C++ desktop app that tracks a configurable list of stocks and shows the usual quote-page views: price chart (line or candles) with range presets, volume bars, hover crosshair, day/52-week stats, and a watch list with last price and day change. No MFC, no frameworks — plain Win32, GDI+, WinHTTP, and `nlohmann/json` from vcpkg.

## Configuration

Tickers live in `stocktool.cfg` next to the executable (created with defaults on first run if missing). It is a plain INI file read with `GetPrivateProfile*`, so keep it ASCII.

```ini
[settings]
refresh_seconds=60        ; 10..3600
default_range=1Y          ; 1D 5D 1M 6M YTD 1Y 5Y MAX
url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?range={range}&interval={interval}&includePrePost=false

[stocks]
RY.TO=Royal Bank of Canada
DOL.TO=Dollarama Inc.
BMO.TO=Bank of Montreal
BNS.TO=Bank of Nova Scotia
BN.TO=Brookfield Corporation
```

- Symbols use Yahoo Finance notation: `.TO` for TSX, `.V` for TSX Venture, bare symbol for NYSE/Nasdaq. TMX Group is the company that owns the Toronto Stock Exchange; the exchange was abbreviated TSE until 2002 and is TSX now, so `money.tmx.com/en/quote/RY` and `RY.TO` are the same listing.
- Up to 32 symbols; extra entries are ignored.
- `url_template` is substituted with `{symbol}`, `{range}` and `{interval}` at run time, so another provider with the same JSON shape can be dropped in without a rebuild.

## Building

Requires Visual Studio 2026 (MSVC), CMake 3.25+, and vcpkg at `C:\vcpkg` with `nlohmann-json:x64-windows-static` installed.

```powershell
cmake --preset default
cmake --build --preset release      # or: --preset debug
build\Release\StockTool.exe
```

The build uses `/W4 /WX /permissive-` and the static CRT (vcpkg `x64-windows-static`), so the exe has no VC++ redistributable dependency.

## Releasing

The version is set once, in `project(StockTool VERSION x.y.z)` in `CMakeLists.txt`; it flows into the window title, the HTTP `User-Agent`, and the exe's `VERSIONINFO` resource (`src/StockTool.rc`). The only other copy is the badge at the top of this README — bump both in the same commit, then tag `vx.y.z` and attach `build\Release\StockTool.exe` plus `stocktool.cfg` to the GitHub release.

## Layout of the code

| File | Role |
|---|---|
| `src/main.cpp` | Entry point: DPI awareness, GDI+ start/stop, creates `App`. |
| `src/app.*` | Main window, controls, layout, painting of header/stats/status/list items. |
| `src/chart.*` | GDI+ chart renderer: price grid, date axis, line/area or candles, volume, last-price tag, hover tooltip. |
| `src/fetcher.*` | Worker thread with a bounded job queue; posts `WM_APP_*` messages to the UI thread. |
| `src/http.*` | Blocking HTTPS GET on WinHTTP into a caller-supplied buffer. |
| `src/quote_parser.*` | Provider JSON to `QuoteData` (meta + OHLCV bars). |
| `src/config.*` | `stocktool.cfg` reading and default-file creation. |
| `src/textfmt.*` | Price/change/volume/date formatting. |
| `src/common.h` | Fixed-capacity data types and the range presets. |

Notes:

- Two kinds of fetch: a **summary** (`5d` daily bars) for every symbol feeds the list and the stats panel; a **chart** fetch for the selected symbol/range feeds the plot. Duplicate queued jobs are coalesced.
- Day change is derived from the daily bars: if the newest bar is today's, previous close is the bar before it; otherwise the newest bar is the previous close.
- Per-monitor DPI v2 aware; everything is laid out from a DPI scale factor.
- Written to the NASA/JPL Power of 10 style: fixed-size arrays (`kMaxStocks`, `kMaxPoints`), bounded loops, asserts on preconditions, no recursion, warnings as errors. The only unbounded loops are the message pump and the worker's service loop, both of which end on shutdown.

## Usage

- Click a symbol in the list to select it; range buttons switch the chart range; **Candles** toggles line/candlestick; **Refresh (F5)** re-fetches everything now. Prices auto-refresh on the configured interval.
- Hover over the chart for a crosshair with date, O/H/L/C and volume.
