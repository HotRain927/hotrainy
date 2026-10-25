// Feasibility test: will the taskbar create a NATIVE icon button for our window?
//
// Idea (from the user):
//   Instead of injecting elements into the taskbar's XAML tree,
//   make the taskbar create an icon for us -- just like any
//   third-party application does.
//
//   Then the icon is a native TaskListButton:
//     - layout managed by the taskbar itself
//     - follows reordering automatically
//     - can never lag behind or overlap
//
// Conditions for the taskbar to show an icon (empirical):
//   - top-level window (not WS_CHILD)
//   - has WS_EX_APPWINDOW, OR
//   - no owner, visible, has a title
//
// This test only creates a window so we can see whether the
// taskbar shows its icon.
//
// Usage:
//   taskbar_icon_test.exe          (default: WS_EX_APPWINDOW)
//   taskbar_icon_test.exe /tool    (WS_EX_TOOLWINDOW -- control case)
//
// NOTE: source is deliberately ASCII-only. The MSVC toolchain here
// runs under a GBK codepage and non-ASCII literals in a UTF-8 file
// get mangled into invalid text suffixes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

static const wchar_t* kWndClass = L"HotrainyIconTest";
static HWND g_hwnd = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
    bool useApp = true;
    bool useTool = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"/tool") == 0) { useTool = true; useApp = false; }
        if (wcscmp(argv[i], L"/app") == 0) { useApp = true; useTool = false; }
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));   // IDI_APPLICATION
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        printf("RegisterClassEx failed %lu\n", GetLastError());
        return 1;
    }

    DWORD ex = 0;
    if (useApp) ex |= WS_EX_APPWINDOW;
    if (useTool) ex |= WS_EX_TOOLWINDOW;

    g_hwnd = CreateWindowExW(
        ex, kWndClass, L"Hotrainy Icon Test",
        WS_OVERLAPPEDWINDOW,
        100, 100, 400, 200,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_hwnd) {
        printf("CreateWindowEx failed %lu\n", GetLastError());
        return 1;
    }

    HICON big = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    ::SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    ::SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)big);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    printf("window created, exstyle=0x%08X\n", (unsigned)ex);
    printf("  WS_EX_APPWINDOW  = %s\n", useApp ? "yes" : "no");
    printf("  WS_EX_TOOLWINDOW = %s\n", useTool ? "yes" : "no");
    printf("\nLook at the taskbar: does an icon appear?\n");
    printf("auto-exit in 15 seconds...\n");

    SetTimer(g_hwnd, 1, 15000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == 1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(g_hwnd);
    return 0;
}