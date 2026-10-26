// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/tagbar/tagbar_wire.cpp — 标签栏 + 它的悬停面板的接线。
//
// 结构重构第 3 片：原 main.cpp 里那 8 处 g_bar.onXxx / g_panel.onXxx 搬到这里
// （文档 docs/代码结构.md 第七节的表里就是这一行："tagbar | 8 | 0 | 待建 tagbar_wire.cpp"）。
//
// 依赖的 main 内部量走 app_context 的薄访问器：
//   bar() / panel() / search() / cfg() / dark() / hidePanelWire() / showPanelForTabWire()
//   toggleIslandPanelWire()
#include "hr/tagbar/tagbar_wire.h"

#include <shellapi.h>   // ShellExecuteW / AllowSetForegroundWindow
#include <windows.h>

#include "hr/app/app_context.h"
#include "hr/settings/dialog.h"          // setDialogDark()
#include "hr/settings/settings_wire.h"   // openSettings()
#include "hr/shared/config.h"
#include "hr/shared/menu.h"              // showContextMenu()
#include <string>

#include "hr/tagbar/tabs_menu_wire.h"    // makeHooks()

namespace hr {

// 标签栏右侧那四个小图标：截图 / 回收站 / 计算机 / 设置。
// （原来在 main.cpp 里；它只依赖模块头，整体搬过来。）
void launchRightIcon(int idx) {
    switch (idx) {
        case BarWindow::ICON_SNIP:
            ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
            ShellExecuteW(nullptr, L"open", L"snippingtool.exe", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case BarWindow::ICON_RECYCLE:
            ::AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, L"open", L"explorer.exe",
                          L"::{645FF040-5081-101B-9F08-00AA002F954E}", nullptr, SW_SHOWNORMAL);
            break;
        case BarWindow::ICON_COMPUTER:
            ::AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, L"open", L"explorer.exe",
                          L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", nullptr, SW_SHOWNORMAL);
            break;
        case BarWindow::ICON_SETTINGS:
            openSettings();
            break;
        default:
            break;
    }
}

void tagbarWire() {
    bar().onDarkChanged = [](bool d) {
        dark() = d;
        panel().setDark(true);   // 面板恒定深色玻璃（文字已是固定"白字黑边"，不跟设置变）
        search().setDark(d);
        setDialogDark(d);
    };

    bar().onHoverTab = [](int idx, RECT bounds) { showPanelForTabWire(idx, bounds); };

    bar().onLeave = []() {
        if (!panel().visible()) {
            // 面板本来就没开，离开标签栏也不该让标签栏一直挂着
            bar().setPanelOpen(false);
            return;
        }
        // 面板开着时一般交给 150ms 轮询判断（鼠标可能正往面板移动），
        // 但要是挪到了右侧小图标上，那就是明确不要面板了 —— 立刻收，别等轮询
        POINT p = {};
        GetCursorPos(&p);
        RECT wr = {};
        GetWindowRect(bar().hwnd(), &wr);
        if (p.y >= wr.top && p.y < wr.bottom && p.x >= wr.left + bar().tabAreaRight()) {
            hidePanelWire();
            bar().clearHover();
        }
    };

    bar().onTabClick = [](int idx, bool right, POINT pt) {
        if (right) {
            showContextMenu(bar().hwnd(), idx, -1, pt.x, pt.y, dark(), makeHooks());
            return;
        }
        if (idx != bar().active()) {
            bar().setActive(idx);
            showPanelForTabWire(idx, bar().tabRect(idx), /*force=*/true);
        } else if (!cfg().hoverMode) {
            if (panel().visible()) {
                hidePanelWire();
            } else {
                showPanelForTabWire(idx, bar().tabRect(idx), /*force=*/true);
            }
        }
    };

    bar().onIconClick = [](int idx) { launchRightIcon(idx); };

    // 点岛台按钮 → 开/关岛台面板。
    //
    // 面板不因鼠标离开而关（用户明确要求），所以这里只管切换，
    // 不做"移开就收"的判断。
    bar().onIslandClick = [](RECT btn) { toggleIslandPanelWire(btn); };

    // 面板自己收起时（150ms 轮询发现鼠标走远了）必须回报给标签栏，
    // 否则 panelOpen_ 一直是 true -> 标签栏永远不收、标签一直亮着。
    // 同时清掉标签栏的悬停态，避免面板没了胶囊还挂在标签上。
    panel().onLeavePanel = []() {
        bar().setPanelOpen(false);
        bar().clearHover();
    };

    panel().onShortcutRightClick = [](int shortcutIndex, POINT pt) {
        int tab = panel().currentTab();
        if (tab < 0) tab = bar().active();
        showContextMenu(panel().hwnd(), tab, shortcutIndex, pt.x, pt.y, dark(), makeHooks());
    };
}

// 标签栏/悬停面板相关的设置键生效。
//
// 透明度、玻璃/壁纸模式、快捷方式字号、壁纸 —— 都只影响"标签栏 + 悬停面板"这一摊
// （岛台只是跟着刷新外观，所以这里也一并调它的 refreshAppearance()）。
bool tagbar_applySetting(const std::string& key, bool& handled) {
    handled = true;
    if (key == "hover_mode") {
        hidePanelWire();
        bar().setActive(-1);
    } else if (key == "panel_opacity") {
        // 全局面板透明度变了：各面板按新 alpha 重新出帧
        hidePanelWire();
        bar().refreshBackdrop(false);
        bar().onConfigChanged();
        panel().onConfigChanged();
        island().refreshAppearance();
        search().refreshAppearance();   // ★ 搜索窗也要跟着滑块走
    } else if (key == "glass_mode" || key == "wallpaper_mode") {
        hidePanelWire();
        bar().refreshBackdrop(true);
        bar().onConfigChanged();
        panel().onConfigChanged();
        // ★ 岛台面板也得跟着变 —— 原来只通知了标签栏和标签面板，
        //   结果关掉玻璃后岛台还是旧样子（用户说的"透明不实时更新"）。
        island().refreshAppearance();
        search().refreshAppearance();   // ★ 搜索窗同理
    } else if (key == "shortcut_font_size") {
        panel().setFontSize(cfg().shortcutFontSize);
        panel().onConfigChanged();
    } else if (key == "wallpaper") {
        hidePanelWire();
        panel().onConfigChanged();
        island().refreshAppearance();
    } else {
        handled = false;
    }
    return true;
}

}  // namespace hr
