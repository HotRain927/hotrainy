// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// Standalone test: does makeHIcon() actually produce a visible "H"?
//
// Context:
//   The taskbar shows the DEFAULT application icon for our window, not the H
//   we intend. The class icon (wc.hIcon/wc.hIconSm) and WM_SETICON are both
//   set, and CreateIconIndirect returns non-null handles -- yet nothing shows.
//
//   That pattern (non-null handle, blank content) points at the GDI+ drawing
//   step producing an empty bitmap.
//
// This program calls the SAME drawing code as islandbtn.cpp and saves the
// result to a PNG, so we can look at it directly instead of guessing.
//
// Build:
//   cl /nologo /O2 /MT /EHsc /Fe:test_hicon.exe test_hicon.cpp ^
//      /link /SUBSYSTEM:CONSOLE gdiplus.lib user32.lib
//
// Source is ASCII-only (the toolchain here runs under a GBK codepage,
// and non-ASCII literals in a UTF-8 file turn into invalid text suffixes).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <vector>

using namespace Gdiplus;

static ULONG_PTR g_gdiplus = 0;

// Mirrors makeHIcon() in islandbtn.cpp.
// Returns the HBITMAP (not an HICON) so we can inspect the pixels,
// plus reports whether anything was actually drawn.
static HBITMAP drawH(int size, int* outNonZeroPixels, int* outMaxAlpha) {
    HDC screen = ::GetDC(nullptr);
    if (!screen) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits,
                                       nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;

    printf("  DIBSection ok, bits=%p\n", bits);

    {
        Bitmap bmp(size, size, size * 4, PixelFormat32bppARGB, (BYTE*)bits);
        printf("  Bitmap status=%d (0=Ok)\n", (int)bmp.GetLastStatus());

        Graphics g(&bmp);
        printf("  Graphics status=%d\n", (int)g.GetLastStatus());

        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetCompositingMode(CompositingModeSourceOver);
        g.Clear(Color(0, 0, 0, 0));

        FontFamily famUI(L"Segoe UI");
        FontFamily famAlt(L"Arial");
        printf("  Segoe UI available=%d, Arial available=%d\n",
               (int)famUI.IsAvailable(), (int)famAlt.IsAvailable());
        FontFamily* fam = famUI.IsAvailable() ? &famUI : &famAlt;

        const REAL fontPx = size * 0.9f;
        printf("  fontPx=%.1f\n", (double)fontPx);

        Font font(fam, fontPx, FontStyleBold, UnitPixel);
        printf("  Font status=%d\n", (int)font.GetLastStatus());

        SolidBrush brush(Color(255, 255, 255, 255));

        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);

        RectF rc(0.0f, 0.0f, (REAL)size, (REAL)size);

        // ── 描边 ──
        //
        // 任务栏按钮读的是 exe 里烤死的图标（AUMID 机制），
        // 运行时改不了。而我们的任务栏是透明的，背后是壁纸 ——
        // 可能浅可能深，单色字形必然在某些壁纸上失效。
        //
        // 所以给 H 加一圈深色描边：
        //   深色背景 → 白 H 本身够亮
        //   浅色背景 → 深色轮廓勾出形状
        //
        // 做法：把深色字形沿 8 个方向各偏移一点画一遍，
        //       再在正中画白色字形。
        {
            SolidBrush darkBrush(Color(255, 0x10, 0x12, 0x16));
            const REAL t = (REAL)size * 0.055;   // 描边粗细，按尺寸缩放
            const REAL dx[] = {-t, 0, t, -t, t, -t, 0, t};
            const REAL dy[] = {-t, -t, -t, 0, 0, t, t, t};
            for (int k = 0; k < 8; ++k) {
                RectF rc2(dx[k], dy[k], (REAL)size, (REAL)size);
                g.DrawString(L"H", -1, &font, rc2, &fmt, &darkBrush);
            }
        }

        Status st = g.DrawString(L"H", -1, &font, rc, &fmt, &brush);
        printf("  DrawString status=%d\n", (int)st);
    }

    // Inspect the result BEFORE converting to an icon.
    const uint32_t* px = (const uint32_t*)bits;
    int nonZero = 0, maxA = 0;
    for (int i = 0; i < size * size; ++i) {
        const uint32_t a = (px[i] >> 24) & 0xFF;
        if (px[i] != 0) ++nonZero;
        if ((int)a > maxA) maxA = (int)a;
    }
    printf("  pixels: nonZero=%d/%d  maxAlpha=%d\n",
           nonZero, size * size, maxA);

    if (outNonZeroPixels) *outNonZeroPixels = nonZero;
    if (outMaxAlpha) *outMaxAlpha = maxA;
    return color;
}

static int getEncoderClsid(const WCHAR* mime, CLSID* out) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    auto* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

int wmain() {
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiplus, &gsi, nullptr) != Ok) {
        printf("GdiplusStartup FAILED\n");
        return 1;
    }
    printf("GdiplusStartup ok\n\n");

    CLSID png{};
    if (getEncoderClsid(L"image/png", &png) < 0) {
        printf("no PNG encoder\n");
        return 1;
    }

    // Test several sizes -- the taskbar uses 16, Alt+Tab uses 32.
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    for (int sz : sizes) {
        printf("=== size %d ===\n", sz);
        int nz = 0, ma = 0;
        HBITMAP bmp = drawH(sz, &nz, &ma);
        if (!bmp) {
            printf("  FAILED: no bitmap\n\n");
            continue;
        }

        // Save the DIB contents to PNG for visual inspection.
        BITMAP bm = {};
        GetObjectW(bmp, sizeof(bm), &bm);
        void* bits = bm.bmBits;

        wchar_t path[MAX_PATH];
        swprintf_s(path, L"D:\\dsh\\workspace\\hicon_%d.png", sz);

        {
            Bitmap out(sz, sz, sz * 4, PixelFormat32bppARGB, (BYTE*)bits);
            Status st = out.Save(path, &png, nullptr);
            printf("  Save -> %d (0=Ok)  %ls\n", (int)st, path);
        }

        DeleteObject(bmp);
        printf("\n");
    }

    GdiplusShutdown(g_gdiplus);
    printf("done\n");
    return 0;
}