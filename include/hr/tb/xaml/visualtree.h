// hr/tb/xaml/visualtree.h — 视觉树监听内部接口
//
// 只在 GlassTAP.dll 内部使用，不对外。
#pragma once

#include "hr/tb/xaml/interfaces.h"

#include <xamlOM.h>

namespace hr {
namespace tb {
namespace xaml {

// 挂上视觉树回调。diag 由系统在 SetSite 时交给我们。
bool attachDiagnostics(IXamlDiagnostics* diag);

// 断开并清空所有记录
void detachDiagnostics();

// 建立命令通道（消息窗口 + 轮询定时器），幂等。
//
// ⚠ 必须在视觉树挂上之后**立即**调用一次。
//
//   这个窗口原来是"用到才建"（由 scheduleApply 懒创建）——
//   于是通道能不能通，取决于有没有人调过 scheduleApply。
//   而调 scheduleApply 的条件又依赖于通道能通（比如岛台开关
//   要靠通道下发），形成死锁：透明=关时没人触发懒创建，
//   通道就一直不通，主程序所有指令超时。
//
//   所以改成由 SetSite 主动建一次，不再依赖任何业务路径。
bool ensureChannelReady();

// 对全部任务栏 / 指定任务栏应用外观
HRESULT applyToAll(GlassBrush brush, UINT color);
HRESULT applyToOne(HWND tb, GlassBrush brush, UINT color);

// 还原。tb 为 nullptr 时还原全部。
HRESULT restoreAllBars(HWND tb);

// 当前已识别的任务栏数量（诊断用）
int knownTaskbarCount();

}  // namespace xaml
}  // namespace tb
}  // namespace hr