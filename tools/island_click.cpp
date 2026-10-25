// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/island_click.cpp — 模拟点击标签栏的岛台按钮，并截图
//
// 用于自动化验证：找到标签栏窗口，算出岛台按钮的位置，
// 发 WM_LBUTTONDOWN，然后截屏。
#include <windows.h>
#include <gdiplus.h>

#include <cstdio>

#pragma comment(lib, "gdiplus.lib")

static ULONG_PTR g_token = 0;

static bool savePng(HBITMAP bmp, const wchar_t* path) {
    Gdiplus::Bitmap image(bmp, nullptr);
    CLSID clsid{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &clsid);
    return image.Save(path, &clsid, nullptr) == Gdiplus::Ok;
}

static void captureRect(const RECT& r, const wchar_t* path) {
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    ::SelectObject(mem, old);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    if (savePng(bmp, path)) wprintf(L"已保存 %s (%dx%d)\n", path, w, h);
    ::DeleteObject(bmp);
}

int wmain(int argc, wchar_t** argv) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_token, &input, nullptr);

    // 标签栏窗口（我们的，不是系统的）
    HWND bar = ::FindWindowW(L"HotrainyBar", nullptr);
    if (!bar) {
        wprintf(L"找不到标签栏窗口 HotrainyBar\n");
        return 1;
    }

    RECT br{};
    ::GetWindowRect(bar, &br);
    wprintf(L"标签栏 %dx%d @ (%d,%d)\n",
            br.right - br.left, br.bottom - br.top, br.left, br.top);

    // 岛台按钮：图标区最左一格。
    // 图标区起点 = 宽 - (4+1)*iconW，iconW 是按 DPI 缩放的 24px。
    //
    // 这里不去猜 DPI：直接扫标签栏右端，
    // 找一个"看起来像按钮"的位置。简化做法是点图标区左起第一格 ——
    // 主程序日志里会打 "图标区从 N 起"，这里按常见 DPI 试两个值。
    const int width = br.right - br.left;
    const int height = br.bottom - br.top;

    // 先按 150% DPI（iconW=36）算，再按 100%（iconW=24）
    int candidates[2] = {0, 0};
    candidates[0] = br.left + (width - 5 * 36) + 18;   // 150%
    candidates[1] = br.left + (width - 5 * 24) + 12;   // 100%

    for (int i = 0; i < 2; ++i) {
        const int x = candidates[i];
        const int y = br.top + height / 2;
        wprintf(L"尝试点击 (%d,%d)\n", x, y);

        // 转成客户区坐标再发消息
        POINT pt = {x, y};
        ::ScreenToClient(bar, &pt);
        ::SendMessageW(bar, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
        ::SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        ::SendMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
        ::Sleep(700);

        RECT ir{};
        HWND isl = ::FindWindowW(L"HotrainyIsland", nullptr);
        if (isl && ::IsWindowVisible(isl)) {
            ::GetWindowRect(isl, &ir);
            wprintf(L"岛台已显示 %dx%d @ (%d,%d)\n",
                    ir.right - ir.left, ir.bottom - ir.top, ir.left, ir.top);
            // 往外扩一点，把阴影和边框一起截进来
            RECT cap = {ir.left - 20, ir.top - 20, ir.right + 20, ir.bottom + 20};
            captureRect(cap, L"D:\\dsh\\workspace\\island_open.png");
            Gdiplus::GdiplusShutdown(g_token);
            return 0;
        }
    }

    wprintf(L"两次尝试都没打开岛台\n");
    // 还是截一张标签栏看看按钮在哪
    captureRect(br, L"D:\\dsh\\workspace\\bar_now.png");
    Gdiplus::GdiplusShutdown(g_token);
    return 2;
}