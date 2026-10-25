// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/shared/shell.h — 用户会话与电源操作
//
// 岛台左下角的「用户」和右下角的「电源」需要这些。
//
// ⚠ 这里是**复现**开始菜单的对应功能，不是去调用开始菜单本身。
//   开始菜单是 explorer 里一个封闭的 XAML 界面，没有对外接口
//   让你"打开它的电源子菜单"。能做到的是调它背后那些系统 API。
//
//   所以实现方式是：读出当前用户名和头像 → 自己画菜单 →
//   点哪一项就调对应的 Windows API。
#pragma once

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hr {

// 当前登录用户的显示名（优先取"全名"，没有就用账户名）
std::wstring currentUserName();

// 当前用户的头像位图。拿不到返回 nullptr（比如账户没有设置图片）。
// 返回的 HBITMAP 由内部缓存持有，不要释放。
HBITMAP currentUserAvatar(int px);

// ── 电源与会话操作 ──
//
// 这些操作都会让当前进程失去对系统的控制（注销/关机/重启），
// 所以调用前务必让主程序把该还原的都还原（比如任务栏透明）。
//
// 每个函数返回是否成功发起。注意"成功发起"不等于"系统一定执行" ——
// 有未保存文档时 Windows 会拦下来弹确认框。

// 关机
bool shutdownSystem();
// 重启
bool restartSystem();
// 睡眠（需要系统支持 S3/S0 现代待机）
bool sleepSystem();
// 休眠（需要预先启用休眠文件）
bool hibernateSystem();
// 注销当前用户
bool logoffSession();
// 锁定屏幕
bool lockWorkstation();
// 断开远程会话（本地登录时不可用）
bool disconnectSession();

// 是否存在待安装的更新（决定要不要显示"更新并关机"）
bool updatesPending();

}  // namespace hr