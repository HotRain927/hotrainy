// hr/menu.h — 玻璃右键菜单（对照 ui/glass_menu.py + ui/context_menu.py）
//
// 系统原生菜单是灰白实心的，跟玻璃标签栏完全不是一个东西，所以自绘。
// 两种入口：
//   showGlassMenu      —— 只给一组行，纯展示/回调
//   showContextMenu    —— 标签/快捷方式的上下文菜单（增删改 + 设置 + 退出）
#pragma once

#include <functional>
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

struct MenuRow {
    bool sep = false;
    std::wstring glyph;
    std::wstring text;
    std::function<void()> cb;
};

// 阻塞到菜单关闭（相当于系统菜单的 TrackPopupMenu）
void showGlassMenu(HWND owner, const std::vector<MenuRow>& rows, int x, int y, bool dark);

// 上下文菜单要的回调；tabIndex / shortcutIndex 由调用方决定含义
struct ContextMenuHooks {
    std::function<void(int tab, const std::wstring& path, const std::wstring& name)> onAdd;
    std::function<void(int tab, int index)> onDelete;
    std::function<void(int tab, int index)> onEdit;
    std::function<void(int tab, int index)> onMove;
    std::function<void(int tab, int index)> onOpenLocation;
    std::function<void(int tab, const std::wstring& name)> onRenameTab;
    std::function<void(int tab)> onAddTab;
    std::function<void(int tab)> onDeleteTab;
    std::function<void(int tab)> onMoveTabLeft;
    std::function<void(int tab)> onMoveTabRight;
    std::function<void()> onOpenSettings;
    std::function<void()> onQuit;
};

// shortcutIndex < 0 表示右键点在标签上，否则是面板里的某个快捷方式
void showContextMenu(HWND owner, int tabIndex, int shortcutIndex, int x, int y, bool dark,
                     const ContextMenuHooks& hooks);

// 打开「设置」/「退出」菜单时，主程序需要暂停自动收起面板
void setContextMenuOpenCallback(std::function<void(bool)> cb);

// 请求关闭当前正在模态运行的菜单（没有菜单时什么都不做）。
//
// ⚠ 为什么需要它：showGlassMenu 会**阻塞**在菜单自己的模态循环里
//   （相当于 TrackPopupMenu）。菜单开着的时候，岛台面板可能因为
//   别的原因收起了（点外面、按 Esc、再点一次岛台图标……）——
//   面板没了、菜单还挂在屏幕上，就成了一个孤儿窗口。
//
//   所以岛台面板收起时调这个，让菜单跟着一起关。
void requestCloseGlassMenu();

}  // namespace hr
