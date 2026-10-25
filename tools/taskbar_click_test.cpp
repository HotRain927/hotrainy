// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// Step 2 verification: can we intercept clicks on a NATIVE taskbar icon?
//
// The previous test proved the taskbar creates a native icon button for
// a window with WS_EX_APPWINDOW. Native layout management is exactly
// what we want (no polling, no overlap).
//
// Open questions this test answers:
//   1. Does clicking the native icon activate our window?
//   2. Can we suppress that activation and instead run our own action?
//   3. Which messages arrive (WM_ACTIVATE? WM_SETFOCUS? none?)
//
// Strategy to try:
//   - WS_EX_NOACTIVATE keeps the window from stealing focus,
//     but the taskbar may still send activation messages.
//   - ShowWindow with SW_SHOWNOACTIVATE.
//   - Handle WM_ACTIVATE / WM_SETFOCUS / WM_NCACTIVATE and log them.
//
// Source is ASCII-only on purpose (toolchain runs under a GBK codepage).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

static const wchar_t* kWndClass = L"HotrainyClickTest";
static HWND g_hwnd = nullptr;
static int g_clickCount = 0;
static DWORD g_start = 0;

static void logmsg(const char* what, WPARAM w, LPARAM l) {
    DWORD ms = GetTickCount() - g_start;
    printf("[%6lu ms] %-18s w=%llu l=%lld\n",
           (unsigned long)ms, what,
           (unsigned long long)w, (long long)l);
    fflush(stdout);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ACTIVATE:
            logmsg("WM_ACTIVATE", w, l);
            return 0;

        case WM_NCACTIVATE:
            logmsg("WM_NCACTIVATE", w, l);
            return TRUE;

        case WM_SETFOCUS:
            logmsg("WM_SETFOCUS", w, l);
            return 0;

        case WM_MOUSEACTIVATE:
            logmsg("WM_MOUSEACTIVATE", w, l);
            return MA_NOACTIVATE;

        case WM_LBUTTONUP:
            ++g_clickCount;
            printf("  >>> click #%d detected on our window\n", g_clickCount);
            fflush(stdout);
            return 0;

        // The taskbar uses this to decide whether to show a preview
        // and to send us activation. Logging it tells us the icon
        // is being interacted with.
        case WM_GETMINMAXINFO:
            return DefWindowProcW(h, m, w, l);

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int wmain() {
    g_start = GetTickCount();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIconSm = wc.hIcon;

    if (!RegisterClassExW(&wc)) {
        printf("RegisterClassEx failed %lu\n", GetLastError());
        return 1;
    }

    // Key question: does WS_EX_NOACTIVATE stop the taskbar from
    // creating an icon? (It should NOT -- NOACTIVATE is unrelated
    // to appwindow-ness.)
    g_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
        kWndClass, L"Hotrainy Click Test",
        WS_POPUP,                       // popup + appwindow = no titlebar, still an icon
        100, 100, 300, 200,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_hwnd) {
        printf("CreateWindowEx failed %lu\n", GetLastError());
        return 1;
    }

    HICON ic = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    ::SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)ic);
    ::SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ic);

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    printf("styles: WS_EX_APPWINDOW | WS_EX_NOACTIVATE\n");
    printf("NOTE: dropped TOOLWINDOW -- with APPWINDOW present it hides the icon.\n");
    printf("\nWatch the console while you click the taskbar icon.\n");
    printf("auto-exit in 30 seconds...\n\n");
    fflush(stdout);

    SetTimer(g_hwnd, 1, 30000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == 1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(g_hwnd);
    return 0;
}