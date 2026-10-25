// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// Enumerate every top-level window that would get a taskbar button.
//
// Why: hiding our island-button window did NOT make the icon disappear,
// so the icon we have been staring at may not belong to us at all.
//
// The taskbar shows a button for a window when it is:
//   - a top-level window (no WS_CHILD)
//   - visible
//   - NOT a tool window (WS_EX_TOOLWINDOW suppresses it)
//   - either has WS_EX_APPWINDOW, or is unowned and has a title
//
// This lists each such window with its class, title, process, ex-style,
// owner and whether it hands out an icon -- so we can identify exactly
// which window produced the icon on the taskbar.
//
// Build:
//   cl /nologo /O2 /MT /EHsc /Fe:list_taskbar_wins.exe list_taskbar_wins.cpp ^
//      /link /SUBSYSTEM:CONSOLE user32.lib
//
// ASCII-only source (GBK codepage toolchain).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

struct Ctx { int shown; };

static const char* exFlags(DWORD ex) {
    static char buf[256];
    buf[0] = 0;
    if (ex & WS_EX_APPWINDOW)    strcat_s(buf, "[APPWINDOW]");
    if (ex & WS_EX_TOOLWINDOW)   strcat_s(buf, "[TOOLWINDOW]");
    if (ex & WS_EX_NOACTIVATE)   strcat_s(buf, "[NOACTIVATE]");
    if (ex & WS_EX_LAYERED)      strcat_s(buf, "[LAYERED]");
    if (ex & WS_EX_TOPMOST)      strcat_s(buf, "[TOPMOST]");
    return buf;
}

static BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    auto* ctx = (Ctx*)lp;

    // Only what the taskbar would consider.
    if (::GetWindow(h, GW_OWNER) != nullptr) return TRUE;   // owned -> no button
    if (::GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) return TRUE;
    if (!::IsWindowVisible(h)) return TRUE;

    const DWORD ex = (DWORD)::GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;                 // tool -> no button

    wchar_t cls[256] = {}, title[512] = {};
    ::GetClassNameW(h, cls, 256);
    ::GetWindowTextW(h, title, 512);

    // Unowned windows need a title to get a button (unless APPWINDOW).
    if (!(ex & WS_EX_APPWINDOW) && title[0] == 0) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);

    // Process name
    wchar_t pname[MAX_PATH] = L"?";
    HANDLE ph = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (ph) {
        DWORD n = MAX_PATH;
        ::QueryFullProcessImageNameW(ph, 0, pname, &n);
        ::CloseHandle(ph);
        wchar_t* slash = wcsrchr(pname, L'\\');
        if (slash) memmove(pname, slash + 1, (wcslen(slash) + 1) * sizeof(wchar_t));
    }

    RECT r{};
    ::GetWindowRect(h, &r);

    // Which icon would the taskbar use?
    HICON ic = (HICON)::SendMessageW(h, WM_GETICON, 2 /*ICON_SMALL2*/, 0);
    if (!ic) ic = (HICON)::SendMessageW(h, WM_GETICON, 0 /*ICON_SMALL*/, 0);
    if (!ic) ic = (HICON)::GetClassLongPtrW(h, GCLP_HICONSM);

    ++ctx->shown;
    printf("%2d) hwnd=%p\n", ctx->shown, (void*)h);
    printf("    class=%ls\n", cls);
    printf("    title=%ls\n", title[0] ? title : L"(none)");
    printf("    proc =%ls (pid %lu)\n", pname, (unsigned long)pid);
    printf("    rect =(%d,%d) %dx%d\n", r.left, r.top,
           r.right - r.left, r.bottom - r.top);
    printf("    ex   =0x%08X %s\n", (unsigned)ex, exFlags(ex));
    printf("    icon =%p%s\n", (void*)ic, ic ? "" : "   <-- NO ICON");
    printf("\n");
    return TRUE;
}

int wmain() {
    printf("windows that would get a taskbar button:\n\n");
    Ctx ctx{0};
    ::EnumWindows(enumProc, (LPARAM)&ctx);
    printf("total: %d\n", ctx.shown);
    return 0;
}