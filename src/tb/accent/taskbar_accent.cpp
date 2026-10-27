// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// This file is part of hotrainy: noncommercial use only; see LICENSE.
//
// Win10 任务栏透明（accents 亚克力）实现。见头文件里的说明。

#include "hr/tb/accent/taskbar_accent.h"

#include <windows.h>

#include <vector>

#include "hr/shared/common.h"
#include "hr/shared/winver.h"

namespace hr::tb::accent {
namespace {

// ── 未公开但长期稳定的 API ─────────────────────────────
enum ACCENT_STATE : DWORD {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5,
};

struct ACCENT_POLICY {
    DWORD AccentState;
    DWORD AccentFlags;      // 2 = 绘制整个窗口；0 = 只画边框
    DWORD GradientColor;    // ★ 0xAABBGGRR（ABGR！不是 ARGB）
    DWORD AnimationId;
};

enum WINDOWCOMPOSITIONATTRIB : DWORD {
    WCA_ACCENT_POLICY = 19,
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attribute;
    PVOID Data;
    SIZE_T SizeOfData;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

SetWindowCompositionAttributeFn resolveFn() {
    static SetWindowCompositionAttributeFn fn = [] {
        HMODULE u = ::GetModuleHandleW(L"user32.dll");
        if (!u) u = ::LoadLibraryW(L"user32.dll");
        return u ? (SetWindowCompositionAttributeFn)::GetProcAddress(
                       u, "SetWindowCompositionAttribute")
                 : nullptr;
    }();
    return fn;
}

// 收集所有任务栏窗口：主任务栏 + 每个副屏一个
std::vector<HWND> taskbarWindows() {
    std::vector<HWND> out;
    if (HWND t = ::FindWindowW(L"Shell_TrayWnd", nullptr)) out.push_back(t);
    int guard = 0;
    HWND prev = nullptr;
    while (guard++ < 16) {
        HWND h = ::FindWindowExW(nullptr, prev, L"Shell_SecondaryTrayWnd",
                                 nullptr);
        if (!h) break;
        out.push_back(h);
        prev = h;
    }
    return out;
}

bool g_applied = false;

// 0xAABBGGRR：先算 ABGR，再交给 API
DWORD makeColor(int alpha, int r, int g, int b) {
    if (alpha < 1) alpha = 1;        // ★ alpha=0 会让亚克力退化成全透明且丢模糊
    if (alpha > 255) alpha = 255;
    return ((DWORD)alpha << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
}

bool sendAccent(HWND hwnd, ACCENT_STATE state, DWORD color) {
    auto fn = resolveFn();
    if (!fn) return false;
    ACCENT_POLICY p{};
    p.AccentState = state;
    p.AccentFlags = (state == ACCENT_DISABLED) ? 0 : 2;
    p.GradientColor = color;
    p.AnimationId = 0;
    WINDOWCOMPOSITIONATTRIBDATA d{};
    d.Attribute = WCA_ACCENT_POLICY;
    d.Data = &p;
    d.SizeOfData = sizeof(p);
    return fn(hwnd, &d) != FALSE;
}

}  // namespace

bool supported() {
    if (hr::safeMode()) return false;             // 安全模式：不做任何系统级改动
    const auto& v = hr::winVersion();
    // Win10 全系 + Win11 21H2 可用；22H2（22621）起 accent 对任务栏大面积失效
    return v.valid() && v.build > 0 && v.build < 22621;
}

bool apply(int opacityPct) {
    if (!supported()) {
        logf("accent: 当前系统不走 accent 这条路（build=%u, 安全模式=%d）",
             hr::winVersion().build, (int)hr::safeMode());
        return false;
    }
    if (opacityPct < 0) opacityPct = 0;
    if (opacityPct > 100) opacityPct = 100;
    // 透明度滑块 -> 亚克力 alpha：留一点底（最低 0x20），免得完全看不出
    int alpha = 0x20 + (205 * opacityPct) / 100;   // 0x20..0xED
    // 深色玻璃：AABBGGRR，R=0x22 G=0x22 B=0x24（略微偏蓝的深灰，跟系统深色任务栏接近）
    DWORD color = makeColor(alpha, 0x22, 0x22, 0x24);

    auto wins = taskbarWindows();
    if (wins.empty()) {
        logf("accent: 找不到任务栏窗口（Shell_TrayWnd）");
        return false;
    }
    int ok = 0;
    for (HWND h : wins) {
        if (sendAccent(h, ACCENT_ENABLE_ACRYLICBLURBEHIND, color)) {
            ++ok;
        } else {
            logf("accent: 下发失败 hwnd=%p err=%lu", (void*)h,
                 (unsigned long)::GetLastError());
        }
    }
    g_applied = (ok > 0);
    logf("accent: 应用亚克力 %d%% -> alpha=0x%02X color=0x%08X，成功 %d/%d 个任务栏",
         opacityPct, (int)((color >> 24) & 0xFF), color, ok, (int)wins.size());
    return g_applied;
}

bool restore() {
    auto wins = taskbarWindows();
    if (wins.empty()) return false;
    int ok = 0;
    for (HWND h : wins) {
        if (sendAccent(h, ACCENT_DISABLED, 0)) ++ok;
    }
    g_applied = false;
    logf("accent: 还原（ACCENT_DISABLED）成功 %d/%d 个任务栏", ok,
         (int)wins.size());
    return ok > 0;
}

bool isApplied() { return g_applied; }

void logStatus() {
    logf("accent: supported=%d applied=%d build=%u",
         (int)supported(), (int)g_applied, hr::winVersion().build);
}

}  // namespace hr::tb::accent