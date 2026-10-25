// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// Decisive test: what icon does our island-button window actually hand out?
//
// makeHIcon() is proven correct (see hicon_32.png -- a clean white H).
// So the question is whether the window exposes that icon to the taskbar.
//
// The taskbar obtains a button's icon by sending WM_GETICON with:
//   ICON_SMALL2 (2)  -- preferred, DPI-appropriate size
//   ICON_SMALL  (0)  -- classic 16x16
//   ICON_BIG    (1)  -- 32x32
// and falls back to the window CLASS icon (GCLP_HICONSM / GCLP_HICON)
// when the window returns NULL.
//
// This tool asks the live window for each of those, dumps whatever comes
// back to a PNG, and reports the class-icon fallbacks. That tells us
// exactly which step is failing.
//
// Build:
//   cl /nologo /O2 /MT /EHsc /Fe:probe_win_icon.exe probe_win_icon.cpp ^
//      /link /SUBSYSTEM:CONSOLE gdiplus.lib user32.lib gdi32.lib
//
// ASCII-only source on purpose (GBK codepage toolchain).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <vector>

using namespace Gdiplus;

static ULONG_PTR g_gdiplus = 0;

// Render an HICON into a PNG so we can see it. Icons carry two bitmaps
// (color + mask); GetIconInfo gives us the color bitmap to convert.
static bool saveIconPng(HICON icon, const wchar_t* path) {
    if (!icon) return false;

    ICONINFO ii = {};
    if (!::GetIconInfo(icon, &ii)) {
        printf("    GetIconInfo failed %lu\n", GetLastError());
        return false;
    }

    BITMAP bm = {};
    if (!ii.hbmColor || !::GetObjectW(ii.hbmColor, sizeof(bm), &bm)) {
        printf("    no color bitmap\n");
        if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
        if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
        return false;
    }

    const int w = bm.bmWidth, h = bm.bmHeight;
    printf("    color bitmap %dx%d, %d bpp\n", w, h, bm.bmBitsPixel);

    // Pull the pixels out as 32bpp top-down.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> buf((size_t)w * h);
    HDC screen = ::GetDC(nullptr);
    const int got = ::GetDIBits(screen, ii.hbmColor, 0, (UINT)h, buf.data(),
                                &bi, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screen);

    ::DeleteObject(ii.hbmColor);
    if (ii.hbmMask) ::DeleteObject(ii.hbmMask);

    if (got != h) {
        printf("    GetDIBits got %d of %d\n", got, h);
        return false;
    }

    // Report alpha so we can tell "blank" from "drawn".
    int nonZero = 0, maxA = 0;
    bool allOpaque = true;
    for (uint32_t c : buf) {
        if (c != 0) ++nonZero;
        const int a = (int)((c >> 24) & 0xFF);
        if (a > maxA) maxA = a;
        if (a != 0 && a != 255) allOpaque = false;
    }
    printf("    pixels: nonZero=%d/%d maxAlpha=%d%s\n",
           nonZero, w * h, maxA,
           (maxA == 0) ? "   <-- BLANK (all alpha 0)" : "");

    // If every alpha is 0 (a legacy non-alpha icon), force opaque so the
    // PNG is viewable rather than fully transparent.
    if (maxA == 0) {
        for (uint32_t& c : buf) c |= 0xFF000000u;
    }

    CLSID png{};
    {
        UINT num = 0, size = 0;
        GetImageEncodersSize(&num, &size);
        if (size == 0) return false;
        std::vector<BYTE> b(size);
        auto* codecs = (ImageCodecInfo*)b.data();
        GetImageEncoders(num, size, codecs);
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                png = codecs[i].Clsid;
                break;
            }
        }
    }

    Bitmap out(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)buf.data());
    const Status st = out.Save(path, &png, nullptr);
    printf("    save -> %d  %ls\n", (int)st, path);
    return st == Ok;
}

int wmain() {
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplus, &gsi, nullptr);

    // Find the island button window. Accept either the old or the renamed
    // class, so this tool keeps working across the rename experiment.
    const wchar_t* classes[] = {L"HotrainyIslandBtnV2", L"HotrainyIslandBtn"};
    HWND hwnd = nullptr;
    for (const wchar_t* cls : classes) {
        hwnd = ::FindWindowW(cls, nullptr);
        if (hwnd) {
            printf("found window: class=%ls hwnd=%p\n", cls, (void*)hwnd);
            break;
        }
    }
    if (!hwnd) {
        printf("island button window NOT FOUND\n");
        printf("(is hotrainy running?)\n");
        GdiplusShutdown(g_gdiplus);
        return 1;
    }

    // Ask the window for each icon flavour the taskbar may request.
    struct { int kind; const wchar_t* name; const wchar_t* file; } asks[] = {
        {2 /*ICON_SMALL2*/, L"ICON_SMALL2", L"D:\\dsh\\workspace\\win_small2.png"},
        {0 /*ICON_SMALL */, L"ICON_SMALL ", L"D:\\dsh\\workspace\\win_small.png"},
        {1 /*ICON_BIG   */, L"ICON_BIG   ", L"D:\\dsh\\workspace\\win_big.png"},
    };

    for (auto& a : asks) {
        printf("\nWM_GETICON %ls (%d):\n", a.name, a.kind);
        HICON ic = (HICON)::SendMessageW(hwnd, WM_GETICON, (WPARAM)a.kind, 0);
        printf("  returned %p\n", (void*)ic);
        if (ic) {
            saveIconPng(ic, a.file);
        } else {
            printf("    (NULL -- taskbar would fall back to the class icon)\n");
        }
    }

    printf("\nclass icon fallbacks:\n");
    HICON csm = (HICON)::GetClassLongPtrW(hwnd, GCLP_HICONSM);
    HICON cbg = (HICON)::GetClassLongPtrW(hwnd, GCLP_HICON);
    printf("  GCLP_HICONSM = %p\n", (void*)csm);
    printf("  GCLP_HICON   = %p\n", (void*)cbg);
    if (csm) saveIconPng(csm, L"D:\\dsh\\workspace\\class_small.png");
    if (cbg) saveIconPng(cbg, L"D:\\dsh\\workspace\\class_big.png");

    printf("\ndone\n");
    GdiplusShutdown(g_gdiplus);
    return 0;
}