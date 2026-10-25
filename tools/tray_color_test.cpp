// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/tray_color_test.cpp — 测试托盘文字变色是否生效
//
// 直接连共享内存通道，给 explorer 里的 DLL 发「改深色 / 改浅色」命令，
// 并截取任务栏右侧区域比较像素变化，验证颜色真的变了。
#include "hr/tb/xaml/channel.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace hr::tb::xaml;

// 测量任务栏右侧（托盘区）的平均亮度和对比度
struct TrayMetric {
    double mean;    // 平均亮度
    double dark;    // 最暗的 5% 像素平均（文字的暗部）
    double light;   // 最亮的 5% 像素平均（文字的亮部）
};

static TrayMetric measureTray() {
    TrayMetric m{ -1, -1, -1 };

    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return m;

    RECT r{};
    if (!::GetWindowRect(tray, &r)) return m;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return m;

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> px((size_t)w * h * 4);
    ::GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    // 采样右侧托盘区（最后 420 像素宽）
    std::vector<int> lum;
    const int x0 = w - 420;
    const int x1 = w - 10;
    for (int y = h / 4; y < h * 3 / 4; ++y) {
        for (int x = x0; x < x1; ++x) {
            lum.push_back(px[((size_t)y * w + x) * 4 + 2]);   // R 通道
        }
    }
    if (lum.empty()) return m;

    double sum = 0;
    for (int v : lum) sum += v;
    m.mean = sum / lum.size();

    std::vector<int> sorted = lum;
    std::sort(sorted.begin(), sorted.end());

    size_t n5 = sorted.size() / 20;
    if (n5 == 0) n5 = 1;

    double d = 0, l = 0;
    for (size_t i = 0; i < n5; ++i) d += sorted[i];
    for (size_t i = sorted.size() - n5; i < sorted.size(); ++i) l += sorted[i];
    m.dark = d / n5;
    m.light = l / n5;
    return m;
}

static void show(const char* label, const TrayMetric& m) {
    printf("  %-16s mean=%6.1f  dark5%%=%6.1f  light5%%=%6.1f\n",
           label, m.mean, m.dark, m.light);
}

int main() {
    printf("===== tray_color_test =====\n\n");

    if (!channelInit()) {
        printf("channelInit failed\n");
        return 1;
    }
    if (!channelReady()) {
        printf("channel not ready\n");
        return 1;
    }

    Sleep(600);
    TrayMetric before = measureTray();
    show("before", before);

    // 改成深色
    printf("\n>>> 发「文字改深色」\n");
    ChannelResult r1 = channelSend(ChannelCmd::SetTextDark, 0, 3000);
    printf("    ok=%d hr=0x%08X\n", (int)r1.ok, (unsigned)r1.hr);
    Sleep(1200);
    TrayMetric dark = measureTray();
    show("after dark", dark);

    // 改成浅色
    printf("\n>>> 发「文字改浅色」\n");
    ChannelResult r2 = channelSend(ChannelCmd::SetTextLight, 0, 3000);
    printf("    ok=%d hr=0x%08X\n", (int)r2.ok, (unsigned)r2.hr);
    Sleep(1200);
    TrayMetric light = measureTray();
    show("after light", light);

    printf("\n===== 判断 =====\n");
    double dMean = std::fabs(dark.mean - light.mean);
    printf("  深色与浅色的平均亮度差 = %.1f\n", dMean);
    if (dMean > 1.0) {
        printf("  ✅ 颜色确实变了\n");
    } else {
        printf("  ❌ 没变化（Foreground 可能不可写，或画刷没生效）\n");
    }

    channelShutdown();
    return 0;
}