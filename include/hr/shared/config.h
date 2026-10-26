// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/shared/config.h — 与 Python 版 config.json 完全兼容（同一份文件、同一套键名）
#pragma once

#include <string>
#include <vector>

namespace hr {

struct Shortcut {
    std::string id;
    std::wstring name;
    std::wstring targetPath;
    std::wstring iconPath;
    std::wstring args;
    std::wstring createdAt;
};

struct Tab {
    std::string id;
    std::wstring name;
    int order = 0;
    std::wstring wallpaper;
    std::vector<Shortcut> shortcuts;
};

struct Config {
    int version = 1;
    int displayMonitor = 0;
    std::string position = "top";           // top / bottom
    bool audioBar = false;
    // 音频条尺寸（像素）与波动方向、拖动位置
    int audioBarW = 0;                      // 横向长度（0 = 自动：原屏幕比例）
    int audioBarH = 0;                      // 纵向长度（0 = 自动：屏高 2/3）
    int audioBarX = -1;                     // 拖动后的位置（-1 = 默认）
    int audioBarY = -1;
    bool hoverMode = true;                  // true=悬停展开 false=点击展开
    std::string themeColor = "默认";
    bool glassMode = true;
    bool wallpaperMode = false;
    bool autoHide = true;
    int shortcutFontSize = 10;              // 7~20

    bool searchEnabled = true;
    std::string searchHotkey = "Ctrl+Alt+Space";
    bool searchFollow = true;
    std::vector<std::wstring> searchDirs;
    int searchW = 560;
    int searchH = 420;
    bool searchSizeCustom = false;
    int searchX = -1;
    int searchY = -1;
    bool searchPosCustom = false;
    std::string searchScope = "all";        // 固定 all（控件已删，载入时强制，见 config.cpp）
    bool searchDialogAuto = true;

    // ── 任务栏（实现见 src/taskbar.cpp + src/tb/xaml/）──
    // 开 = 任务栏透明，关 = 跟随系统。与标签栏完全解耦。
    bool taskbarTransparent = false;
    // 透明强度。越界由 TaskbarService::SetAlpha 夹到 [100, 245]。
    // 180 = 实测平衡点（背景够透、图标保留 76%）。
    int taskbarAlpha = 180;

    // 托盘文字颜色。
    //
    // 任务栏透明后背景变成壁纸。若壁纸是浅色，系统默认的白色文字
    // （时间、日期、输入法、WLAN、音量等）会看不清。
    //
    //   "auto"  按任务栏背后的壁纸亮度自动决定深浅（默认）
    //   "dark"  强制深色（适合浅色壁纸）
    //   "light" 强制浅色（适合深色壁纸）
    std::wstring taskbarTextColor = L"auto";

    // ── 岛台（Island）──
    //
    // 一个从标签栏左侧按钮唤出的独立面板：顶部搜索、按分类展示大图标、
    // 左下角用户菜单、右下角电源菜单。
    //
    // 开启后任务栏上的「开始」和「搜索」会被隐藏（由岛台接管入口）。
    bool islandEnabled = false;

    // 面板尺寸。用户拖边框改过之后记在这里，下次打开还是这个大小。
    int islandW = 720;
    int islandH = 520;

    // 面板是否记住过位置（同样由拖动决定；-1 表示还没定过）
    int islandX = -1;
    int islandY = -1;

    // 岛台面板的背景不透明度（0.05 ~ 1.0）。
    //
    // ⚠ 和标签面板的透明度是**两套独立**的配置。
    //   岛台是常驻的大面板，用户通常希望它比标签面板更透一点，
    //   好让壁纸透出来；但也不能太透，否则文字读不清。
    //   默认 0.72 是这两者之间的折中。
    double islandOpacity = 0.72;
    // ★ 全局面板透明度（0-100，所有面板共用一个）。作用在各面板提交分层窗口时的整体 alpha 上：
    //   100 = 完全可见，0 = 完全看不见。面板底色本来就是纯透明（不铺色底），
    //   只保留一圈极细边框，所以这个值只影响整块面板的浓淡。
    int panelOpacity = 100;

    // 「全部」分类里的快捷方式 —— 用户通过面板右键自己加的。
    // 与 Tab::shortcuts 结构相同，显示时用大图标网格。
    std::vector<Shortcut> islandShortcuts;

    // 岛台面板当前选中的分类（"__all__" 或某个 tab.id）。
    // 记住它，下次打开还停在原来的分类。
    std::string islandCategory = "__all__";

    std::vector<Tab> tabs;
};

// 对应 main.py:488 `_glass_appearance_on()`。
// 玻璃开关与壁纸开关在**数据层**互斥（开一个关另一个，见 main.py:552/681），
// 但在**外观层**是「或」：只要任一开着，标签栏就画完整的液态玻璃。
// Python 原文注释：
//   「玻璃状态：全部玻璃，没有任何壁纸；
//     壁纸状态：标签栏和玻璃状态长得一模一样，只有快捷方式面板换成壁纸。
//     所以只要任意一个开着，就按玻璃外观绘制。」
inline bool glassAppearanceOn(const Config& cfg) {
    return cfg.glassMode || cfg.wallpaperMode;
}

// 对应 hover_panel.py:152 `_use_wallpaper`：真要用壁纸当底
// = 壁纸模式开着 且 该标签自己设了壁纸。
inline bool useWallpaperFor(const Config& cfg, int tabIndex) {
    if (!cfg.wallpaperMode || tabIndex < 0 || tabIndex >= (int)cfg.tabs.size()) return false;
    return !cfg.tabs[tabIndex].wallpaper.empty();
}

// 对应 hover_panel.py:158 `_glass_like`：走「悬浮圆角卡片」外观
// = 玻璃模式 或（壁纸模式且该标签有壁纸）。
inline bool glassLikePanel(const Config& cfg, int tabIndex) {
    return cfg.glassMode || useWallpaperFor(cfg, tabIndex);
}

extern const int kFontMin;
extern const int kFontMax;

bool loadConfig(Config& cfg, std::vector<std::string>& warnings);
// 写回同一份 config.json：只覆盖已知键，未知键原样保留
bool saveConfig(const Config& cfg);

Tab* findTab(Config& cfg, int index);

}  // namespace hr
