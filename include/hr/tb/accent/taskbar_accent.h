// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// This file is part of hotrainy: noncommercial use only; see LICENSE.
//
// Win10 任务栏透明（accents 亚克力）—— Win10 适配阶段 2
// ────────────────────────────────────────────────────────────
// 为什么单独一条路：
//   程序原有的透明是"注入 explorer、改 Win11 的 XAML 任务栏"，而 Win10
//   根本没有 XAML 任务栏。Win10 的正确做法是对 Shell_TrayWnd 调
//   SetWindowCompositionAttribute + ACCENT_ENABLE_ACRYLICBLURBEHIND
//   —— 按实测矩阵这是"最稳定的一代，几乎无坑"，而且**完全不需要注入**。
//
// 两个必须记住的坑：
//   ① GradientColor 是 0xAABBGGRR（ABGR），不是 ARGB —— 颜色写反会变蓝/变红
//   ② ACRYLIC 下 alpha=0 会退化成完全透明且丢掉模糊；最小要留 1
#pragma once

namespace hr::tb::accent {

// 当前系统是否应该走这条路：
//   只有 build < 22621（Win10 全系 + Win11 21H2）才谈得上；
//   Win11 22H2 起 accent 对任务栏大面积失效，要走注册表/背板方案。
//   安全模式下恒为 false。
bool supported();

// 应用亚克力。opacityPct：0-100，越大越不透明（内部会夹到 [1, 255]）。
// 幂等：重复调用只是再次下发同一个 accent。
bool apply(int opacityPct);

// 还原：把 accent 置为 ACCENT_DISABLED。
// ⚠ SetWindowCompositionAttribute 读不到窗口原来的 accent 状态，
//   所以"还原"只能关掉 —— 这是社区通行的做法，也是我们能做的最干净的事。
bool restore();

bool isApplied();

// 把当前状态写进日志（apply/restore 内部也会写关键行）
void logStatus();

}  // namespace hr::tb::accent