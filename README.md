# StockTool

[![Release][release-badge]][release-latest]

[release-badge]: https://img.shields.io/badge/release-v0.6.0-blue
[release-latest]: https://github.com/Flinterpop/StockTool/releases/latest

*Last updated: 22 Sep 2026*

A Win32 C++ desktop app that tracks configurable watch lists of stocks and funds and shows the usual quote-page views: price chart (line or candles) with range presets, SMA/Bollinger/RSI indicators, dividend markers, a 1Y/5Y trend inset, a compare-all overlay, volume bars, hover crosshair, day/52-week/fundamentals stats, a headlines pane, holdings with a currency-converted portfolio total, price alerts, CSV export, a tray icon and a light/dark theme. No MFC, no frameworks — plain Win32, GDI+, WinHTTP, and `nlohmann/json` from vcpkg.

## Installing

- **Installer:** run `StockTool-<version>-setup.exe` from the release. It installs to Program Files with a Start-menu entry (desktop and sign-in-startup shortcuts optional). The config then lives in `%APPDATA%\StockTool\stocktool.cfg`.
- **Portable:** unzip `StockTool-<version>-win64.zip` anywhere and run `StockTool.exe`; the config sits next to the exe.

## Configuration

`stocktool.cfg` is a plain INI file (read with `GetPrivateProfile*`, so keep it ASCII). You rarely need to edit it by hand: the **Ticker** and **List** menus, the buttons under the watch list and **View > Theme** all write through to it, and **Reload cfg** re-reads it after a hand edit without a restart.

```ini
[settings]
refresh_seconds=60        ; 10..3600, while a market in the list is open
closed_refresh_seconds=900 ; 60..3600, while every market in the list is closed
default_range=1Y          ; 1D 5D 1M 6M YTD 1Y 5Y MAX (the last-used range wins once saved)
inset_range=5Y            ; trend inset: 5Y or 1Y
theme=system              ; system | light | dark
start_minimized=0
minimize_to_tray=0
portfolio_currency=CAD    ; portfolio totals are converted into this
benchmark=^GSPTSE         ; index overlaid by View > Benchmark index (^GSPC = S&P 500)
url_template=https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?range={range}&interval={interval}&includePrePost=false&events=div
quote_url_template=https://query2.finance.yahoo.com/v7/finance/quote?symbols={symbols}&crumb={crumb}
search_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={query}&quotesCount=12&newsCount=0&listsCount=0
news_source=google        ; google | yahoo | none
news_url_template=https://query2.finance.yahoo.com/v1/finance/search?q={symbol}&quotesCount=0&newsCount=8&listsCount=0
news_rss_template=https://news.google.com/rss/search?q={query}&hl=en-CA&gl=CA&ceid=CA:en
fallback_provider=tmx     ; tmx | none — second chart source when Yahoo fails
tmx_url=https://app-money.tmx.com/graphql

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

[transactions]            ; SYMBOL.N=YYYY-MM-DD,shares,price   (negative shares = sell)
RY.TO.1=2024-03-01,100,132.50
RY.TO.2=2025-06-12,-40,175.00

[symbol_map]              ; broker symbol=watch-list symbol, for File > Import from TD
TDB902=0P0000A30L

[state]                   ; written by the app on exit: window placement, range, toggles, selection, list
```

- Symbols use Yahoo Finance notation: `.TO` for TSX, `.V` for TSX Venture, bare symbol for NYSE/Nasdaq; class shares use a dash (`BRK-B`, `RCI-B.TO`). Canadian **mutual funds** are not listed under their fund codes (`TDB902` finds nothing) — search by name in the Add dialog and pick the fund; the symbol is a Morningstar-style ID such as `0P000071WA.TO` (TD Canadian Index – e, i.e. TDB900). Funds have a daily NAV only, so intraday ranges are empty and volume/open/high/low show `-`.
- Up to 32 symbols per list, up to 8 lists. The order in a `[stocks...]` section is the list order (Ticker > Move up/down rewrites the section). Holdings, alerts, currency overrides and transactions are keyed by symbol and shared by every list that contains it. When a symbol has transactions they take precedence over its `[holdings]` line: shares and average cost are computed from the buys and sells.
- `url_template` (chart bars) is substituted with `{symbol}`, `{range}` and `{interval}` (`events=div` is appended if missing, for dividends); `quote_url_template` (fundamentals) with `{symbols}` and `{crumb}`; `search_url_template` with `{query}`. News comes from `news_source`: **google** (default) uses `news_rss_template` — Google News RSS, Canadian English edition, searched by the ticker's display name in quotes (falling back to the symbol), which pulls in the Globe and Mail, Financial Post, BNN Bloomberg, CBC, Canadian Press and the wires; **yahoo** uses `news_url_template` (`{symbol}`), Yahoo's own tagging; **none** turns the pane off. Change the `hl`/`gl`/`ceid` parameters for another edition.

## Building and testing

Requires Visual Studio 2026 (MSVC), CMake 3.25+, and vcpkg at `C:\vcpkg` with `nlohmann-json` and `catch2` for `x64-windows-static`. Inno Setup 6 is needed only to build the installer.

```powershell
cmake --preset default
cmake --build --preset release      # or: --preset debug
build\Release\StockTool.exe
build\Release\stocktool_tests.exe   # Catch2 unit tests
ISCC.exe /DAppVersion=0.6.0 installer\StockTool.iss   # -> installer\Output\StockTool-0.6.0-setup.exe
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
| `src/help.*`, `src/help.rtf` | The in-app user guide window (RichEdit) and its text. |
| `src/chart.*` | GDI+ chart renderer: price grid, date axis, line/area or candles, SMA/Bollinger overlays, RSI pane, volume, dividend markers, last-price tag, trend inset, compare overlay, hover tooltip. |
| `src/indicators.*` | SMA, Bollinger bands, RSI (pure functions, unit-tested). |
| `src/theme.*` | Light/dark palettes, Windows theme preference, compare-series colours. |
| `src/fetcher.*` | Worker thread with a bounded job queue (summary, chart, inset, fundamentals, search, FX, news); posts `WM_APP_*` messages to the UI thread; cookie/crumb handshake for fundamentals. |
| `src/http.*` | Blocking HTTPS GET on WinHTTP with a persistent session (cookies). |
| `src/quote_parser.*` | Provider JSON to `QuoteData` (meta + OHLCV bars + dividends), `QuoteStats`, `SearchHit`, `NewsItem`; a small RSS reader for Google News. |
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

- Click a symbol to select it; range buttons (or **Ctrl+1…8**) switch the chart range; **Candles** toggles line/candlestick; **Compare** overlays every ticker as % change over the range; **Refresh (F5)** re-fetches everything now. Prices auto-refresh every `refresh_seconds` while any market in the list is open; once every exchange in the list has closed (the provider reports each ticker's regular session) the app drops to `closed_refresh_seconds` (15 min by default), refreshes again the moment a known session opens, and says so in the header (**Market open / closed**) and the status line (**markets closed, next check in …**). F5 always refreshes.
- **View** menu: SMA 20, SMA 50, Bollinger bands (20, 2σ), RSI (14) pane, the trend inset, the news pane, the **benchmark index** (the `benchmark` symbol, S&P/TSX Composite by default, rebased to the chart's first close and drawn as a dashed line; hover shows both % changes), theme (system/light/dark) and minimize-to-tray. All toggles are remembered. The **toolbar** across the top of the chart area holds the same plot toggles as buttons (Candles, Compare, SMA 20, SMA 50, Bollinger, RSI, Inset, News, Bench, History); it and the menu stay in sync.
- **View > Portfolio history** (the **History** button) replaces the price chart with the active list's portfolio value over time: value (solid), cost base (dotted) and the benchmark rebased to the first value in view (dashed), with the range buttons choosing the window and a hover tooltip per day. The value is recorded once a day, in `portfolio-history.csv` next to the config (`date,list,value,cost,currency`, one row per list per day, the last value of the day wins), whenever StockTool is running with holdings and every price and FX rate is in; there is nothing to see until a second day has been recorded.
- **Tooltips** explain every button (and each range button's interval and shortcut). The ones over the **portfolio strip** and the **header** are built on the spot and list the full figures — value, today, cost, unrealised, realised, dividends received and income — which is where to look when a panel is too narrow to show them all.
- **Help > User guide (F1)** opens an in-app guide (`src/help.rtf`, embedded as a resource): a quick start, what SMA, Bollinger bands and RSI mean, what the benchmark index is and how to read it, and a step-by-step walkthrough of setting up and using the portfolio.
- The trend inset can be **dragged** anywhere inside the plot; its position is remembered. Green **D** markers on the chart are ex-dividend dates; hover for the amount.
- **Add… (Ctrl+N / Ctrl+F)** has a search box: type a company name or partial symbol and pick from the results — the fields fill in; double-click adds straight away. The dialog stays open after each add; **Close** (or Esc) dismisses it. A currency code can be entered to override what the provider reports for that listing.
- **Ticker** menu (also right-click on the list): **Add…**, **Edit… (F2, or double-click a row)**, **Remove**, **Move up/down (Ctrl+Up/Down)**, **Holding…** (shares + average cost → portfolio strip above the list, converted into `portfolio_currency`, with a dividend-income estimate; holding line in the header), **Transactions…** (dated buys and sells; average cost, cost base, unrealised and realised gain and dividends received are computed from them and replace the manual holding) and **Alerts…** (price above/below).
- **List** menu / tabs above the watch list: **New list…**, **Rename list…**, **Delete list**, and one entry per list to switch. The default list cannot be renamed or deleted. The portfolio strip covers the active list.
- **File > Export**: the watch list (symbol, name, currency, last, change, holding, value, 52-week range) or the current chart's bars (date, OHLCV, dividend) as UTF-8 CSV.
- **File > Import from TD (CSV)…** reads a TD Direct Investing (WebBroker) export. An **Activity** export becomes `[transactions]` (buys, sells and dividend reinvestments; dividends, interest, fees and transfers are ignored; a missing price is taken from the net amount); a **Holdings** export becomes `[holdings]` (quantity and average cost, or book value ÷ quantity). The parser finds the header row by its column names (Symbol, Quantity, Price/Average Cost, Trade Date, Transaction Type, Market, Description), so preamble lines and column order do not matter, and it accepts `$1,234.50`, `(12.50)`, and ISO, `MM/DD/YYYY` or `12 Mar 2026` dates; UTF-8, UTF-16 and ANSI files all work. Symbols are mapped to Yahoo notation: `[symbol_map]` first, then a match against every watch list (`RY` → `RY.TO` when that is listed), then the Market column (`CA` → `.TO`, `TSXV` → `.V`). A summary is shown before anything is written; rows already recorded are skipped, so re-importing the same file changes nothing, and a checkbox adds symbols that are in no watch list to the current one. Nothing leaves the machine and no credentials are involved — export from WebBroker (Accounts > Holdings / Activity > Export), then import the file.
- The **news pane** (View > News pane) lists the latest headlines for the selected ticker; click one to open it in your browser. Google News (Canadian edition) by default; `news_source=yahoo` switches to Yahoo's feed.
- Hover over the chart for a crosshair with date, O/H/L/C, volume, dividend and RSI; in Compare mode the tooltip lists every ticker's % change at that date.
- **Help > Data source health…** shows, per endpoint (chart, fundamentals, search, FX, news, crumb), the last HTTP status, latency, time since the last success, consecutive failures and the last error, plus the fetch-queue depth and the config path in use; it refreshes every 2 s. When a provider answers **HTTP 429** (rate limited) the app backs off that endpoint for 1, 2, 4 then 8 minutes, doubling on repeats, and says so in the status bar and the health window.
- **Fallback data source.** When the chart endpoint fails for a ticker (unreachable, HTTP error, rate-limit backoff, unparseable reply) the same bars are fetched from **TMX Money** (`fallback_provider=tmx`, the site behind money.tmx.com): daily/weekly/monthly bars only, so 1D/5D are empty from the fallback, and no fundamentals. Symbols are translated automatically (`RY.TO` → `RY`, `RCI-B.TO` → `RCI.B`, `AAPL` → `AAPL:US`, `^GSPTSE` → `^TSX`); mutual-fund IDs and FX pairs have no equivalent. Data that came from the fallback is labelled in the header (**TMX Money (fallback)**) and the status line, and the health panel has a `tmx-fallback` row. FX rates for CAD pairs fall back to the **Bank of Canada** daily average (Valet API). `fallback_provider=none` turns both off.
- The tray icon's tooltip shows every ticker's last price; left-click shows the window, right-click gives Show / Refresh / Exit.
