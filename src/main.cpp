// StockTool entry point.
#include "app.h"

#include <commctrl.h>

#include <memory>

// Visual-styles (Common Controls v6) manifest.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE /*hPrev*/, PWSTR /*cmdLine*/, int nCmdShow) {
    const BOOL dpiOk = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    (void)dpiOk;  // best effort: falls back to system DPI on older builds

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    const BOOL iccOk = InitCommonControlsEx(&icc);
    (void)iccOk;

    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiIn, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"GDI+ initialisation failed.", L"StockTool", MB_ICONERROR);
        return 1;
    }

    int rc = 0;
    {
        // App owns several MB of fixed buffers; keep it off the stack.
        auto app = std::make_unique<st::App>();
        std::wstring err;
        if (!app->Create(hInst, nCmdShow, err)) {
            MessageBoxW(nullptr, err.c_str(), L"StockTool", MB_ICONERROR);
            rc = 1;
        } else {
            rc = app->Run();
        }
    }

    Gdiplus::GdiplusShutdown(gdiToken);
    return rc;
}
