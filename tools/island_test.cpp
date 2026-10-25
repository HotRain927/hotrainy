// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/island_test.cpp — 验证任务栏「开始」「搜索」按钮能否隐藏
//
// 不启动主程序，直接连共享内存通道给 explorer 里的 DLL 发命令，
// 然后截屏比对任务栏左端的像素变化。
#include "hr/tb/xaml/channel.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace hr::tb::xaml;

// 量任务栏左端一条的亮度统计
struct RegionStat {
    double mean = -1;
    double dark = -1;
    double light = -1;
    int nonBg = 0;      // 与中位数差异大的像素数（≈内容量）
};

static RegionStat measure(HWND tray, int x0, int x1) {
    RegionStat s;
    RECT r{};
    if (!tray || !::GetWindowRect(tray, &r)) return s;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return s;
    if (x1 > w) x1 = w;

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

    std::vector<int> lum;
    for (int y = 0; y < h; ++y) {
        for (int x = x0; x < x1; ++x) {
            lum.push_back(px[((size_t)y * w + x) * 4 + 2]);
        }
    }
    if (lum.empty()) return s;

    double sum = 0;
    for (int v : lum) sum += v;
    s.mean = sum / lum.size();

    std::vector<int> sorted = lum;
    std::sort(sorted.begin(), sorted.end());
    size_t n5 = sorted.size() / 20;
    if (!n5) n5 = 1;
    double d = 0, l = 0;
    for (size_t i = 0; i < n5; ++i) d += sorted[i];
    for (size_t i = sorted.size() - n5; i < sorted.size(); ++i) l += sorted[i];
    s.dark = d / n5;
    s.light = l / n5;

    // 内容量：偏离中位数 ±25 的像素占比
    int med = sorted[sorted.size() / 2];
    for (int v : lum) if (std::abs(v - med) > 25) ++s.nonBg;
    return s;
}

static void show(const char* label, const RegionStat& s, int total) {
    printf("  %-12s mean=%6.1f  dark=%6.1f  light=%6.1f  内容=%d/%d (%.1f%%)\n",
           label, s.mean, s.dark, s.light,
           s.nonBg, total, total ? 100.0 * s.nonBg / total : 0.0);
}

int main() {
    printf("===== island_test：任务栏开始/搜索能否隐藏 =====\n\n");

    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) { printf("找不到任务栏\n"); return 1; }

    RECT r{};
    ::GetWindowRect(tray, &r);
    printf("任务栏 %dx%d @ (%d,%d)\n\n", r.right - r.left, r.bottom - r.top, r.left, r.top);

    if (!channelInit() || !channelReady()) {
        printf("通道未就绪\n");
        return 1;
    }

    // 任务栏左端 0~500 覆盖「开始 + 搜索 + 任务视图」
    const int X0 = 0, X1 = 500;
    const int total = (X1 - X0) * (r.bottom - r.top);

    Sleep(600);
    RegionStat before = measure(tray, X0, X1);
    show("隐藏前", before, total);

    printf("\n>>> 发「岛台接管 = 开」\n");
    ChannelResult r1 = channelSend(ChannelCmd::SetIslandTakeover, 1, 8000);
    printf("    ok=%d hr=0x%08X\n", (int)r1.ok, (unsigned)r1.hr);
    Sleep(2500);

    RegionStat after = measure(tray, X0, X1);
    show("隐藏后", after, total);

    printf("\n>>> 发「岛台接管 = 关」（验证还原）\n");
    ChannelResult r2 = channelSend(ChannelCmd::SetIslandTakeover, 0, 8000);
    printf("    ok=%d hr=0x%08X\n", (int)r2.ok, (unsigned)r2.hr);
    Sleep(2500);

    RegionStat back = measure(tray, X0, X1);
    show("还原后", back, total);

    printf("\n===== 判断 =====\n");
    double d1 = std::fabs(after.nonBg - before.nonBg);
    double d2 = std::fabs(back.nonBg - before.nonBg);
    printf("  隐藏前后内容量差 = %.0f px\n", d1);
    printf("  还原后与原始差   = %.0f px\n", d2);

    if (d1 > 500) {
        printf("  ✅ 隐藏生效（任务栏左端内容明显减少）\n");
    } else {
        printf("  ❌ 看不出变化\n");
    }
    if (d2 < d1 * 0.5) {
        printf("  ✅ 还原正常\n");
    } else {
        printf("  ⚠ 还原后与原始仍有差异\n");
    }

    channelShutdown();
    return 0;
}