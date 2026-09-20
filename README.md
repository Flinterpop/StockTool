# StockTool

[![Release][release-badge]][release-latest]

[release-badge]: https://img.shields.io/badge/release-v0.2.2-blue
[release-latest]: https://github.com/Flinterpop/StockTool/releases/latest

*Last updated: 19 Sep 2026*

A Win32 C++ desktop app that tracks a configurable list of stocks and shows the usual quote-page views: price chart (line or candles) with range presets, SMA/Bollinger/RSI indicators, a 1Y/5Y trend inset, a compare-all overlay, volume bars, hover crosshair, day/52-week/fundamentals stats, a watch list with last price and day change, portfolio value, price alerts, a tray icon and a light/dark theme. No MFC, no frameworks — plain Win32, GDI+, WinHTTP, and `nlohmann/json` from vcpkg.

## Configuration

Everything lives in `stocktool.cfg` next to the executable (created with defaults on first run if missing). It is a plain INI file read with `GetPrivateProfile*`, so keep it ASCII. You rarely need to edit it by hand: the **Ticker** menu, the buttons under the watch list and **View > Theme** all write through to it, and **Reload cfg** re-reads it after a hand edit without a restart.

```ini
[settings]
refresh_seconds=60        ; 10..3600
default_range=1Y          ; 1D 5D 1M 6M YTD 1Y 5Y MAX (the last-used range wins once saved)
inset_range=5Y            ; trend inset: 5Y or 1Y
theme=system              ; system | light | dark
start_minimized=0
minimize_to_tray=0
url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?range={range}&interval={interval}&includePrePost=false
quote_url_template=https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}
search_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={query}&quotesCount=12&newsCount=0&listsCount=0

[stocks]
RY.TO=Royal Bank of Canada
DOL.TO=Dollarama Inc.

[holdings]                ; symbol=shares,average cost
RY.TO=100,250

[alerts]                  ; symbol=above,below  (0 = unset)
RY.TO=300,0

[state]                   ; written by the app on exit: window placement, range, toggles, selection
```

- Symbols use Yahoo Finance notation: `.TO` for TSX, `.V` for TSX Venture, bare symbol for NYSE/Nasdaq; class shares use a dash (`BRK-B`, `RCI-B.TO`). Canadian **mutual funds** are not listed under their fund codes (`TDB902` finds nothing) — search by name in the Add dialog and pick the fund; the symbol is a Morningstar-style ID such as `0P0000A30L` (TD U.S. Index Fund e-Series). Funds have a daily NAV only, so intraday ranges (1D/5D) are empty and volume/open/high/low show `-`. TMX Group is the company that owns the Toronto Stock Exchange; the exchange was abbreviated TSE until 2002 and is TSX now, so `money.tmx.com/en/quote/RY` and `RY.TO` are the same listing.
- Up to 32 symbols; extra entries are ignored. The order in `[stocks]` is the list order (Ticker > Move up/down rewrites the section).
- `url_template` (chart bars) is substituted with `{symbol}`, `{range}` and `{interval}`; `quote_url_template` (fundamentals) with `{symbols}` (comma-separated) and `{crumb}`. Leave `quote_url_template` empty to disable fundamentals. `search_url_template` (symbol search in the Add dialog) is substituted with `{query}`; leave it empty to disable search.

## Building and testing

Requires Visual Studio 2026 (MSVC), CMake 3.25+, and vcpkg at `C:\vcpkg` with `nlohmann-json` and `catch2` for `x64-windows-static`.

```powershell
cmake --preset default
cmake --build --preset release      # or: --preset debug
build\Release\StockTool.exe
build\Release\stocktool_tests.exe   # Catch2 unit tests (parser, config, indicators, formatting)
```

The build uses `/W4 /WX /permissive-` and the static CRT (vcpkg `x64-windows-static`), so the exe has no VC++ redistributable dependency. The first build seeds `stocktool.cfg` next to the exe; later builds leave it alone, because the app writes holdings, alerts and window state back into it. The UI-free parts are a static library (`stocktool_core`) shared by the app and the tests.

## Releasing

The version is set once, in `project(StockTool VERSION x.y.z)` in `CMakeLists.txt`; it flows into the window title, the HTTP `User-Agent`, and the exe's `VERSIONINFO` resource (`src/StockTool.rc`). The only other copy is the badge at the top of this README — bump both in the same commit, then tag `vx.y.z` and attach `build\Release\StockTool.exe` plus `stocktool.cfg` to the GitHub release.

## Layout of the code

| File | Role |
|---|---|
| `src/main.cpp` | Entry point: DPI awareness, GDI+ start/stop, creates `App`. |
| `src/app.*` | Main window, menu, controls, layout, tray icon, alerts, portfolio, painting of header/stats/status/list items. |
| `src/dialogs.*` | Add ticker, Holding and Alerts dialogs. |
| `src/chart.*` | GDI+ chart renderer: price grid, date axis, line/area or candles, SMA/Bollinger overlays, RSI pane, volume, last-price tag, trend inset, compare overlay, hover tooltip. |
| `src/indicators.*` | SMA, Bollinger bands, RSI (pure functions, unit-tested). |
| `src/theme.*` | Light/dark palettes, Windows theme preference, compare-series colours. |
| `src/fetcher.*` | Worker thread with a bounded job queue; posts `WM_APP_*` messages to the UI thread; cookie/crumb handshake for fundamentals. |
| `src/http.*` | Blocking HTTPS GET on WinHTTP with a persistent session (cookies). |
| `src/quote_parser.*` | Provider JSON to `QuoteData` (meta + OHLCV bars) and `QuoteStats` (fundamentals). |
| `src/config.*` | `stocktool.cfg` reading/writing: settings, stocks, holdings, alerts, view state. |
| `src/textfmt.*` | Price/money/percent/compact/date formatting. |
| `src/common.h` | Fixed-capacity data types and the range presets. |
| `src/StockTool.rc`, `src/resource.h` | Version resource, app icon (`src/StockTool.ico`), menu and dialog templates. |
| `tools/make_icon.py` | Regenerates the multi-size `.ico` with Pillow: `python tools/make_icon.py`. |
| `tests/*.cpp` | Catch2 tests for the core library. |

Notes:

- Fetch kinds: a **summary** (`5d` daily bars) per symbol feeds the list, tray tooltip, portfolio and stats; a **chart** fetch per symbol/range feeds the plot (all symbols when Compare is on); an **inset** fetch (1Y or 5Y) feeds the trend box; one batch **quote** fetch feeds market cap / P/E / yield. Jobs carry their symbol and fully built URL, and results echo the symbol, so the worker never reads configuration and a list change mid-fetch cannot put data in the wrong row.
- The fundamentals endpoint needs a session cookie and a "crumb"; the worker obtains both on first use and retries once with a fresh crumb on 401/403. If the provider changes this, fundamentals show `-` and the status line says why; the chart still works.
- Day change is derived from the daily bars: if the newest bar is today's, previous close is the bar before it; otherwise the newest bar is the previous close.
- Alerts fire once when the price crosses the level (tray balloon + status line + amber row), and re-arm when it crosses back.
- Per-monitor DPI v2 aware; everything is laid out from a DPI scale factor. Dark mode covers the client area, title bar and buttons; the Win32 menu bar and dialogs stay light.
- Written to the NASA/JPL Power of 10 style: fixed-size arrays (`kMaxStocks`, `kMaxPoints`), bounded loops, asserts on preconditions, no recursion, warnings as errors. The only unbounded loops are the message pump and the worker's service loop, both of which end on shutdown.

## Usage

- Click a symbol to select it; range buttons switch the chart range; **Candles** toggles line/candlestick; **Compare** overlays every ticker as % change over the range; **Refresh (F5)** re-fetches everything now. Prices auto-refresh on the configured interval.
- **View** menu: SMA 20, SMA 50, Bollinger bands (20, 2σ), RSI (14) pane, the trend inset, theme (system/light/dark) and minimize-to-tray. All toggles are remembered.
- The trend inset can be **dragged** anywhere inside the plot (the cursor changes to a move cursor over it); its position is remembered as a fraction of the plot, so it stays put across resizes and DPI changes.
- **Add… (Ctrl+N)** has a search box: type a company name or partial symbol and pick from the results (symbol, name, exchange, type) — the fields fill in; double-click adds straight away. You can still type a symbol directly. The dialog stays open after each add so you can enter several tickers in a row; **Close** (or Esc) dismisses it.
- **Ticker** menu (also right-click on the list): **Add… (Ctrl+N)**, **Edit… (F2, or double-click a row)** to change a ticker's symbol or display name with the same search box — holdings and alerts follow it and its place in the list is kept — **Remove**, **Move up/down (Ctrl+Up/Down)**, **Holding…** (shares + average cost → portfolio strip above the list, holding line in the header) and **Alerts…** (price above/below).
- Hover over the chart for a crosshair with date, O/H/L/C, volume and RSI; in Compare mode the tooltip lists every ticker's % change at that date.
- The tray icon's tooltip shows every ticker's last price; left-click shows the window, right-click gives Show / Refresh / Exit.
