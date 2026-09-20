# StockTool

[![Release][release-badge]][release-latest]

[release-badge]: https://img.shields.io/badge/release-v0.3.0-blue
[release-latest]: https://github.com/Flinterpop/StockTool/releases/latest

*Last updated: 20 Sep 2026*

A Win32 C++ desktop app that tracks configurable watch lists of stocks and funds and shows the usual quote-page views: price chart (line or candles) with range presets, SMA/Bollinger/RSI indicators, dividend markers, a 1Y/5Y trend inset, a compare-all overlay, volume bars, hover crosshair, day/52-week/fundamentals stats, a headlines pane, holdings with a currency-converted portfolio total, price alerts, CSV export, a tray icon and a light/dark theme. No MFC, no frameworks — plain Win32, GDI+, WinHTTP, and `nlohmann/json` from vcpkg.

## Installing

- **Installer:** run `StockTool-<version>-setup.exe` from the release. It installs to Program Files with a Start-menu entry (desktop and sign-in-startup shortcuts optional). The config then lives in `%APPDATA%\StockTool\stocktool.cfg`.
- **Portable:** unzip `StockTool-<version>-win64.zip` anywhere and run `StockTool.exe`; the config sits next to the exe.

## Configuration

`stocktool.cfg` is a plain INI file (read with `GetPrivateProfile*`, so keep it ASCII). You rarely need to edit it by hand: the **Ticker** and **List** menus, the buttons under the watch list and **View > Theme** all write through to it, and **Reload cfg** re-reads it after a hand edit without a restart.

```ini
[settings]
refresh_seconds=60        ; 10..3600
default_range=1Y          ; 1D 5D 1M 6M YTD 1Y 5Y MAX (the last-used range wins once saved)
inset_range=5Y            ; trend inset: 5Y or 1Y
theme=system              ; system | light | dark
start_minimized=0
minimize_to_tray=0
portfolio_currency=CAD    ; portfolio totals are converted into this
url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?range={range}&interval={interval}&includePrePost=false&events=div
quote_url_template=https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}
search_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={query}&quotesCount=12&newsCount=0&listsCount=0
news_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={symbol}&quotesCount=0&newsCount=8&listsCount=0

[stocks]                  ; the default watch list
RY.TO=Royal Bank of Canada
DOL.TO=Dollarama Inc.

[stocks.Banks]            ; an extra watch list (List > New list...)
TD=Toronto-Dominion Bank

[holdings]                ; symbol=shares,average cost   (shared by all lists)
RY.TO=100,250

[alerts]                  ; symbol=above,below  (0 = unset)
RY.TO=300,0

[currency]                ; symbol=CAD  override when the provider labels a listing oddly
0P0000A30L=CAD

[state]                   ; written by the app on exit: window placement, range, toggles, selection, list
```

- Symbols use Yahoo Finance notation: `.TO` for TSX, `.V` for TSX Venture, bare symbol for NYSE/Nasdaq; class shares use a dash (`BRK-B`, `RCI-B.TO`). Canadian **mutual funds** are not listed under their fund codes (`TDB902` finds nothing) — search by name in the Add dialog and pick the fund; the symbol is a Morningstar-style ID such as `0P000071WA.TO` (TD Canadian Index – e, i.e. TDB900). Funds have a daily NAV only, so intraday ranges are empty and volume/open/high/low show `-`.
- Up to 32 symbols per list, up to 8 lists. The order in a `[stocks...]` section is the list order (Ticker > Move up/down rewrites the section). Holdings, alerts and currency overrides are keyed by symbol and shared by every list that contains it.
- `url_template` (chart bars) is substituted with `{symbol}`, `{range}` and `{interval}` (`events=div` is appended if missing, for dividends); `quote_url_template` (fundamentals) with `{symbols}` and `{crumb}`; `search_url_template` with `{query}`; `news_url_template` with `{symbol}`. Leave any of the last three empty to disable that feature.

## Building and testing

Requires Visual Studio 2026 (MSVC), CMake 3.25+, and vcpkg at `C:\vcpkg` with `nlohmann-json` and `catch2` for `x64-windows-static`. Inno Setup 6 is needed only to build the installer.

```powershell
cmake --preset default
cmake --build --preset release      # or: --preset debug
build\Release\StockTool.exe
build\Release\stocktool_tests.exe   # Catch2 unit tests
ISCC.exe /DAppVersion=0.3.0 installer\StockTool.iss   # -> installer\Output\StockTool-0.3.0-setup.exe
```

The build uses `/W4 /WX /permissive-` and the static CRT (vcpkg `x64-windows-static`), so the exe has no VC++ redistributable dependency. The first build seeds `stocktool.cfg` next to the exe; later builds leave it alone, because the app writes holdings, alerts and window state back into it. The UI-free parts (config, parsers, indicators, formatting, chart geometry, themes) are a static library (`stocktool_core`) shared by the app and the tests.

## Releasing

The version is set once, in `project(StockTool VERSION x.y.z)` in `CMakeLists.txt`; it flows into the window title, the HTTP `User-Agent`, and the exe's `VERSIONINFO` resource (`src/StockTool.rc`). The only other copies are the badge at the top of this README and the `/DAppVersion=` passed to `ISCC.exe` — bump them in the same commit, tag `vx.y.z`, then attach the portable zip (`StockTool.exe` + `stocktool.cfg` + `README.md`) and the installer to the GitHub release.

## Layout of the code

| File | Role |
|---|---|
| `src/main.cpp` | Entry point: DPI awareness, GDI+ start/stop, creates `App`. |
| `src/app.*` | Main window, menus, tabs, controls, layout, tray icon, alerts, portfolio/FX, news pane, CSV export, painting. |
| `src/dialogs.*` | Add/Edit ticker (with search), Holding, Alerts and list-name dialogs. |
| `src/chart.*` | GDI+ chart renderer: price grid, date axis, line/area or candles, SMA/Bollinger overlays, RSI pane, volume, dividend markers, last-price tag, trend inset, compare overlay, hover tooltip. |
| `src/indicators.*` | SMA, Bollinger bands, RSI (pure functions, unit-tested). |
| `src/theme.*` | Light/dark palettes, Windows theme preference, compare-series colours. |
| `src/fetcher.*` | Worker thread with a bounded job queue (summary, chart, inset, fundamentals, search, FX, news); posts `WM_APP_*` messages to the UI thread; cookie/crumb handshake for fundamentals. |
| `src/http.*` | Blocking HTTPS GET on WinHTTP with a persistent session (cookies). |
| `src/quote_parser.*` | Provider JSON to `QuoteData` (meta + OHLCV bars + dividends), `QuoteStats`, `SearchHit`, `NewsItem`. |
| `src/config.*` | `stocktool.cfg` reading/writing: settings, watch lists, holdings, alerts, currency overrides, view state; config path resolution. |
| `src/textfmt.*` | Price/money/percent/compact/date formatting. |
| `src/common.h` | Fixed-capacity data types and the range presets. |
| `src/StockTool.rc`, `src/resource.h` | Version resource, app icon (`src/StockTool.ico`), menu and dialog templates. |
| `installer/StockTool.iss` | Inno Setup script. |
| `cmake/copy_if_missing.cmake` | Seeds the config next to the exe on the first build only. |
| `tools/make_icon.py` | Regenerates the multi-size `.ico` with Pillow: `python tools/make_icon.py`. |
| `tests/*.cpp` | Catch2 tests for the core library. |

Notes:

- Fetch kinds: a **summary** (`5d` daily bars) per symbol feeds the list, tray tooltip, portfolio and stats; a **chart** fetch per symbol/range feeds the plot (all symbols when Compare is on); an **inset** fetch (5Y/1Y, with dividend events) feeds the trend box and the dividend-income estimate; one batch **quote** fetch feeds market cap / P/E / yield; **FX** fetches (`USDCAD=X` etc.) convert holdings into the portfolio currency; **news** fetches feed the headlines pane. Jobs carry their symbol and fully built URL, and results echo the symbol, so the worker never reads configuration and a list change mid-fetch cannot put data in the wrong row.
- Charts and trend insets are cached per ticker and prefetched in the background after the selected ticker loads, so switching tickers is instant; a selection also queues a silent refresh, which only repaints if the data actually changed. The rendered chart is kept in its own bitmap and only the hover crosshair is redrawn per mouse move.
- The fundamentals endpoint needs a session cookie and a "crumb"; the worker obtains both on first use and retries once with a fresh crumb on 401/403. If the provider changes this, fundamentals show `-` and the status line says why; the chart still works.
- Day change is derived from the daily bars: if the newest bar is today's, previous close is the bar before it; otherwise the newest bar is the previous close. Dividend income is the trailing 365 days of ex-dividend amounts times shares held.
- Alerts fire once when the price crosses the level (tray balloon + status line + amber row), and re-arm when it crosses back.
- Per-monitor DPI v2 aware; everything is laid out from a DPI scale factor. Dark mode covers the client area, title bar and buttons; the Win32 menu bar and dialogs stay light.
- Written to the NASA/JPL Power of 10 style: fixed-size arrays (`kMaxStocks`, `kMaxPoints`, `kMaxLists`), bounded loops, asserts on preconditions, no recursion, warnings as errors. The only unbounded loops are the message pump and the worker's service loop, both of which end on shutdown.

## Usage

- Click a symbol to select it; range buttons (or **Ctrl+1…8**) switch the chart range; **Candles** toggles line/candlestick; **Compare** overlays every ticker as % change over the range; **Refresh (F5)** re-fetches everything now. Prices auto-refresh on the configured interval.
- **View** menu: SMA 20, SMA 50, Bollinger bands (20, 2σ), RSI (14) pane, the trend inset, the news pane, theme (system/light/dark) and minimize-to-tray. All toggles are remembered.
- The trend inset can be **dragged** anywhere inside the plot; its position is remembered. Green **D** markers on the chart are ex-dividend dates; hover for the amount.
- **Add… (Ctrl+N / Ctrl+F)** has a search box: type a company name or partial symbol and pick from the results — the fields fill in; double-click adds straight away. The dialog stays open after each add; **Close** (or Esc) dismisses it. A currency code can be entered to override what the provider reports for that listing.
- **Ticker** menu (also right-click on the list): **Add…**, **Edit… (F2, or double-click a row)**, **Remove**, **Move up/down (Ctrl+Up/Down)**, **Holding…** (shares + average cost → portfolio strip above the list, converted into `portfolio_currency`, with a dividend-income estimate; holding line in the header) and **Alerts…** (price above/below).
- **List** menu / tabs above the watch list: **New list…**, **Rename list…**, **Delete list**, and one entry per list to switch. The default list cannot be renamed or deleted. The portfolio strip covers the active list.
- **File > Export**: the watch list (symbol, name, currency, last, change, holding, value, 52-week range) or the current chart's bars (date, OHLCV, dividend) as UTF-8 CSV.
- The **news pane** (View > News pane) lists the latest headlines for the selected ticker; click one to open it in your browser.
- Hover over the chart for a crosshair with date, O/H/L/C, volume, dividend and RSI; in Compare mode the tooltip lists every ticker's % change at that date.
- The tray icon's tooltip shows every ticker's last price; left-click shows the window, right-click gives Show / Refresh / Exit.
