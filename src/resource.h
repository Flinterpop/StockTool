// Resource IDs shared between StockTool.rc and the C++ sources.
#pragma once

#define IDI_APPICON       101
#define IDR_MAINMENU      102

// "Add ticker" dialog
#define IDD_ADDTICKER     200
#define IDC_SYMBOL        201
#define IDC_NAME          202
#define IDC_HINT          203
#define IDC_QUERY         204
#define IDC_SEARCH        205
#define IDC_RESULTS       206
#define IDC_CURRENCY      207

// "List name" dialog (new / rename watch list)
#define IDD_LISTNAME      230
#define IDC_LISTNAME      231
#define IDC_LTITLE        232
#define IDC_LHINT         233

// "Holding" dialog
#define IDD_HOLDING       210
#define IDC_QTY           211
#define IDC_COST          212
#define IDC_HHINT         213
#define IDC_HTITLE        214

// "Alerts" dialog
#define IDD_ALERTS        220
#define IDC_ABOVE         221
#define IDC_BELOW         222
#define IDC_AHINT         223
#define IDC_ATITLE        224

// Menu commands
#define IDM_RELOAD        1001
#define IDM_REFRESH       1002
#define IDM_EXIT          1003
#define IDM_CANDLES       1010
#define IDM_COMPARE       1011
#define IDM_SMA20         1012
#define IDM_SMA50         1013
#define IDM_BOLLINGER     1014
#define IDM_RSI           1015
#define IDM_INSET         1016
#define IDM_THEME_SYSTEM  1020
#define IDM_THEME_LIGHT   1021
#define IDM_THEME_DARK    1022
#define IDM_MINTRAY       1023
#define IDM_ADD           1030
#define IDM_REMOVE        1031
#define IDM_MOVEUP        1032
#define IDM_MOVEDOWN      1033
#define IDM_HOLDING       1034
#define IDM_ALERTS        1035
#define IDM_EDIT          1036
#define IDM_ABOUT         1040
#define IDM_TRAY_SHOW     1050
#define IDM_TRAY_EXIT     1051
#define IDM_EXPORT_LIST   1004
#define IDM_EXPORT_CHART  1005
#define IDM_NEWS          1017
#define IDM_LIST_NEW      1060
#define IDM_LIST_RENAME   1061
#define IDM_LIST_DELETE   1062
#define IDM_LIST_BASE     1080   // + list index: switch list (menu + tabs)
#define IDM_RANGE_BASE    1100   // + range index: Ctrl+1..8
