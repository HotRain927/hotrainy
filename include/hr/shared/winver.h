// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// This file is part of hotrainy: noncommercial use only; see LICENSE.
//
// 系统版本与能力层（Win10 适配阶段 1）
// ────────────────────────────────────────────────────────────
// 为什么需要它：
//   1. 程序原先**完全没有版本检测**，默认自己是 Win11，透明完全依赖
//      "注入 explorer 修改 XAML 任务栏"这条通道 —— 而 Win10 根本没有
//      XAML 任务栏，那条通道整条不存在。
//   2. ⚠ 版本号绝不能信 GetVersionEx（受 manifest 影响，未声明 Win10
//      兼容性时一律返回 6.2/Win8）。必须用 ntdll 的 RtlGetVersion。
//   3. 能力不能散落成各处 if (build >= xxx)：集中成一张表，运行时按表
//      取第一个可用方案，失败降级到下一个，并写进日志。
#pragma once

#include <string>
#include <vector>

namespace hr {

// ── 系统版本（真实值）──────────────────────────────────
struct WinVersion {
    unsigned major = 0;          // 10
    unsigned minor = 0;          // 0
    unsigned build = 0;          // 19045 / 22621 / 26200
    unsigned ubr = 0;            // 修订号，如 3803
    std::wstring display;        // 注册表 DisplayVersion，如 "22H2" / "24H2"

    bool valid()   const { return build != 0; }
    bool isWin10() const { return build > 0 && build < 22000; }
    bool isWin11() const { return build >= 22000; }
    bool atLeast(unsigned b) const { return build >= b; }

    // "10.0.19045.3803 (22H2)"
    std::string text() const;
    // 给日志用的中文描述："Windows 10 22H2 (build 19045.3803)"
    std::string friendly() const;
};

// 进程内缓存，只探测一次
const WinVersion& winVersion();

// ── 任务栏透明：可用方案（按优先级排列）────────────────
//   Accent        SetWindowCompositionAttribute + ACRYLICBLURBEHIND
//                 ★ Win10 1809~22H2 的主路径，实测最稳，且**不需要注入**
//   Registry      HKCU\...\Explorer\Advanced\TaskbarAcrylicOpacity
//                 ★ Win11 22H2+ 的路径
//   DwmBackdrop   DWMWA_SYSTEMBACKDROP_TYPE = TRANSIENTWINDOW
//   XamlInject    注入 explorer 改 XAML 任务栏（程序原有的方式）
//   None          都不行
enum class TbMethod { XamlInject, Accent, Registry, DwmBackdrop, None };

const char* methodName(TbMethod m);

// 当前系统上"应该优先尝试"的方案表（已按优先级排序）
std::vector<TbMethod> taskbarMethods();

// 某个方案在当前系统上是否**理论上**可用（不保证一定生效，但可排除必然无效的）
bool methodSupported(TbMethod m);

// ── 安全模式 ──────────────────────────────────────────
// 命令行 --safe / -safe，或配置文件里的 safeMode=true。
// 安全模式下跳过一切注入与系统级改动，供用户自救。
bool safeMode();

// ── 初始化（在启动日志之后、创建任何窗口之前调用一次）──
//   探测版本 -> 打印版本与能力表 -> 解析 --safe
void initVersionLayer();

// 把版本与能力表写进日志（initVersionLayer 内部会调，也可单独调）
void logCapabilities();

}  // namespace hr