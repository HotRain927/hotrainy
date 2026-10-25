// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tb/xaml/injector.h — XAML 注入器（hotrainy 侧）
//
// 职责：把 GlassTAP.dll 注入 explorer，通过 COM 控制任务栏 XAML 背景。
//
// 这是唯一能实现"任务栏背景完全透明 + 图标保持清晰"的方案：
//   任务栏背景是 explorer 进程内 XAML 树里名为 "BackgroundFill" 的
//   Rectangle 的 Fill 画刷。从外部用 DWM/accent/blur/backdrop 全部无效
//   （已穷举实测），必须进到 explorer 进程里替换那个画刷。
#pragma once

#include "hr/tb/xaml/interfaces.h"

namespace hr {
namespace tb {
namespace xaml {

// 注入并取得服务接口。可重复调用（幂等）。
// explorer 重启后需要重新调用。
bool injectIntoExplorer();

// 是否已注入且服务可用
bool isInjected();

// 取服务接口（未注入返回 nullptr）。不要 Release 它。
IGlassAppearanceService* service();

// ── 高层操作 ─────────────────────────────────────────────────────────
// 把任务栏背景设为全透明（透出壁纸，图标不受影响）
bool applyTransparent();

// 还原成系统默认外观
bool restoreDefault();
bool restoreTransparencyOnly();

// 探测 explorer 里是否还有残留的 DLL 在监听控制通道。
//
// 用途：上一个实例被强杀时，它的 DLL 仍留在 explorer 里、
// 任务栏还是透明的。新实例启动时用这个判断要不要先清理。
bool hasListener();

// 岛台接管模式：开启后隐藏任务栏上的「开始」与「搜索」按钮，
// 让岛台成为这两个入口的唯一提供者。
//
// 关闭时会自动还原（把 Opacity 写回 1），不会留下缺按钮的任务栏。
bool setIslandTakeover(bool on);

// 取走"任务栏上的岛台按钮被点击"的标志。
//
// 按钮的 Tapped 回调跑在 explorer 里，它只置一个标志位；
// 主程序定时调这个函数来取。返回 true 表示这次确实点击过。
//
// ⚠ 是"取走"语义：调用一次就清零，不会重复返回 true。
bool consumeIslandTap();

// 查任务栏上岛台按钮的屏幕位置。
// 取到时填入 rect（屏幕坐标），返回是否成功。
bool islandButtonRect(RECT& out);

// 查「开始按钮原来占的那个坑」的屏幕位置和尺寸。
//
// 岛台按钮要摆在这里 —— 位置是从开始按钮**实测**出来的，
// 所以视觉上和图标组天然对齐，不需要猜任何像素。
//
// ⚠ 必须在把开始按钮 Collapsed **之前**查询。
//   隐藏之后 ActualWidth 变 0，坐标也不再更新。
//   实际用法是：开启接管时先查一次存下来，之后再隐藏。
bool startButtonSlot(RECT& out);

// 取走“任务栏布局变了”的标志（取完清零）。
//
// 图标增减时任务栏会 Add/Remove 一个 TaskListButton，
// DLL 的视觉树回调立刻能收到并置位，
// 这里取走它 —— 比轮询省得多，而且不会漏。
//
// 返回 true 表示这次确实有变化，主程序应该重新读坑位。
bool consumeLayoutChanged();

// explorer 重启后重置状态
void releaseService();

// 进程退出前调用：还原 + 卸钩子 + 释放接口
void shutdown();

}  // namespace xaml
}  // namespace tb
}  // namespace hr