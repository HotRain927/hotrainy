// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/island_shot.cpp — 岛台接管前后截图对比
//
// 每次调用：截任务栏左端一条，存成 PNG。
// 配合外部命令开关岛台，人工对比两张图即可。
#include <windows.h>
#include <gdiplus.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "gdiplus.lib")

static ULONG_PTR g_gdiToken = 0;

static bool savePng(HBITMAP bmp, const wchar_t* path) {
    Gdiplus::Bitmap image(bmp, nullptr);
    CLSID clsid{};
    // PNG 编码器的 CLSID
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &clsid);
    return image.Save(path, &clsid, nullptr) == Gdiplus::Ok;
}

int wmain(int argc, wchar_t** argv) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiToken, &input, nullptr);

    const wchar_t* outPath = (argc > 1) ? argv[1] : L"island_shot.png";
    const int capW = (argc > 2) ? _wtoi(argv[2]) : 600;

    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) { wprintf(L"找不到任务栏\n"); return 1; }

    RECT r{};
    ::GetWindowRect(tray, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    const int cw = (capW > w) ? w : capW;
    wprintf(L"任务栏 %dx%d @ (%d,%d)，截左端 %d px\n", w, h, r.left, r.top, cw);

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, cw, h);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, cw, h, screen, r.left, r.top, SRCCOPY);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    if (savePng(bmp, outPath)) {
        wprintf(L"已保存 %s\n", outPath);
    } else {
        wprintf(L"保存失败\n");
    }
    ::DeleteObject(bmp);

    Gdiplus::GdiplusShutdown(g_gdiToken);
    return 0;
}