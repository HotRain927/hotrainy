// hr/island/island.h — 岛台面板
//
// 从标签栏左侧的「岛台」按钮唤出的独立面板。和标签栏的悬停面板不同：
//
//   · 不因鼠标离开而关闭 —— 只在点右上角 × 或再点岛台按钮时关
//   · 可拖边框改大小，尺寸记进配置，下次打开还是这么大
//   · 顶部一条搜索框（复用标签栏的搜索引擎）
//   · 左侧分类栏：「全部」（用户右键加的）+ 每个标签一项
//   · 中间大图标网格
//   · 左下角用户菜单（锁定 / 注销 / 切换账户）
//   · 右下角电源菜单（睡眠 / 关机 / 重启 / 注销）
//   · 右键网格空白处 → 添加快捷方式到「全部」
//
// 渲染复用标签面板那套玻璃管线（Surface + Backdrop），所以外观一致：
// 抓屏 → 模糊 → 色调渐变 → 圆角 → 描边。
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

#include "hr/shared/config.h"
#include "hr/glass/glass.h"
#include "hr/text/island_theme.h"
// ⚠ 面板内嵌的搜索框要复刻标签栏搜索窗那套（真 EDIT + 同一套搜索引擎），
//   所以需要 SearchHit / queryHits。search.h 不反向包含 island.h，无循环。
#include "hr/search/search.h"

namespace hr {

// 内嵌搜索框的子类过程（定义在 island.cpp）。
//
// ⚠ 必须在**命名空间作用域**再声明一次：
//   光靠类里的 friend 声明，普通查找找不到它（friend 只对 ADL 可见），
//   island.cpp 里取函数地址会报"未声明的标识符"。
LRESULT CALLBACK islandSearchEditProc(HWND, UINT, WPARAM, LPARAM);


class IslandWindow {
public:
    // 要打开某个快捷方式（复用主程序那套启动逻辑）
    std::function<void(int)> onLaunchShortcut;
    // 要打开某个文件路径（分类里的快捷方式用 targetPath）
    std::function<void(const std::wstring&)> onLaunchPath;
    // 配置变了（分类增删、快捷方式增删）→ 外层负责保存
    std::function<void()> onConfigDirty;

    bool create(HINSTANCE hInst, Config* cfg);
    void destroy();

    // 从标签栏的岛台按钮唤出。x,y 是锚点（一般是按钮的屏幕坐标）。
    void show(int anchorX, int anchorY);
    void hide();
    void toggle(int anchorX, int anchorY);
    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

    void setDark(bool dark);
    void notifyConfigChanged();  // 标签/快捷方式变了 → 重绘

    // 外观参数（玻璃开关 / 壁纸等）变了 → 重抓背景 + 重烘焙底色。
    //
    // ⚠ 和 notifyConfigChanged 分开：那个只管分类/布局，
    //   这个是"设置里的外观项变了"，必须重抓屏幕背景才看得出变化
    //   —— 否则岛台的透明/模糊不会实时更新（用户报过这个）。
    void refreshAppearance();
    // 透明实时更新（对照标签面板 HoverPanel::pollTick / refreshBackdropLive）：
    // 前台窗口一变就重抓一次背景，别一直显示"打开那一刻"的背景。
    void pollTick();
    void refreshBackdropLive();
    // 拖动/缩放**过程中**的实时重抓（面板可见，靠 WDA_EXCLUDEFROMCAPTURE 排除自己）
    void refreshBackdropWhileDragging();
    void reloadIcons();          // 图标缓存失效（比如新加了快捷方式）

    // 设置文字颜色策略，跟随设置里的「文字颜色」。
    // 传 L"auto" / L"dark" / L"light"。
    void setTextMode(const std::wstring& mode);

    // 强制指定深浅（"自动"模式下由主程序采样壁纸后调这个）。
    void setDarkText(bool dark);

private:
    // 按当前策略算出该用深色还是浅色文字，并更新 textMain_ / textDim_。
    void resolveTextColors();
    // 采样面板背后那块壁纸的平均亮度
    double sampleBackdropLuminance();

    // 可写版 —— 拖动换位要直接改容器内容。
    //
    // 为什么不用 currentShortcuts() 再 const_cast：
    //   容器可能是 cfg_->islandShortcuts，也可能是某个 Tab 的 shortcuts，
    //   显式区分更安全。
    std::vector<Shortcut>* mutableCurrentShortcuts();

    // 拖动换位：把 from 位置的项移到 to 位置。
    //
    // 顺序就是容器里的实际次序 —— 没有单独的 order 字段，
    // 这样"配置里怎么存"和"面板上怎么显示"永远一致。
    void moveShortcut(int from, int to);

    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    // ── 布局与绘制 ──
    void layout();
    void rebuildBase();

    // ★ 外阴影留白（逻辑像素，走 px() 换算）——与悬停面板同一套：
    //   窗口向外扩这么多，内容坐标**一像素不动**；
    //   只有"跟系统打交道"的地方要减掉它（提交、鼠标坐标、窗口矩形、输入框摆放）。
    int shadowInset() const;
    void redraw();
    void refreshBackdrop();

    // ── 分类 ──
    struct Category {
        std::string id;         // "__all__" 或 tab.id
        std::wstring name;      // 显示名
        bool isAll = false;
        int shortcutCount = 0;
    };
    void buildCategories();
    int categoryAt(int x, int y) const;
    const std::vector<Shortcut>* currentShortcuts() const;

    // ── 网格（大图标）──
    void computeGrid();
    int cellAt(int x, int y) const;      // 返回网格下标，-1 表示没命中
    // ── 面板内嵌的真搜索框 ────────────────────────────────────────
    //
    // 用户要求："把（标签栏搜索窗）原本的那个搜索框复制一份，固定在岛台上"。
    //
    // ⚠ 必须是真正的 EDIT 控件，不能自绘 —— 这是整件事的关键：
    //     自绘框只能靠 WM_CHAR 收字符，而中文输入法会**截走按键**
    //     （字母进组合态、回车被上屏），窗口一个字符都收不到；
    //     之前为了能用只能把输入法摘掉，代价是搜不了中文。
    //     真 EDIT 有输入法组合窗口，拼音/五笔都正常。
    //
    // ⚠ 而且它必须是**独立顶层弹窗**，不能是子控件：
    //     本面板用 UpdateLayeredWindow 上屏，这种窗口不渲染子控件
    //     （文字整块消失）。标签栏搜索窗也是这么做的，这里照抄。
    HWND searchEdit_ = nullptr;          // 内嵌的真输入框（独立弹窗）
    WNDPROC searchEditOldProc_ = nullptr;
    HFONT searchFont_ = nullptr;
    // 搜索框底色：从画好的那一帧里取的像素（和提亮条零色差）。
    // ⚠ 必须有它 —— EDIT 完全没有底色时，**输入法组合串**会用系统默认的
    //   白色当背景，打字时后面就出现一块白底（用户要求去掉）。
    HBRUSH searchBgBrush_ = nullptr;
    uint32_t searchBgCol_ = 0xFF202024;
    bool searchTyping_ = false;          // 有输入内容 → 面板显示结果列表
    std::vector<SearchHit> results_;     // 查询结果（和搜索窗同一套引擎）
    int resultSel_ = 0;
    int resultScroll_ = 0;
    int resultRowsVis_ = 1;
    RECT resultArea_{};                  // 结果列表区域（客户区坐标）
    int resultRowH_ = 0;

    void createSearchEdit(HINSTANCE hInst);
    void layoutSearchEdit();             // 把搜索框铺到面板顶部那条
    void showSearchEdit(bool show);
    void focusSearchEdit();
    void runIslandSearch();              // 防抖后真正查一次
    void clearIslandSearch();
    void updateSearchBgColor(int x, int y);
    int  iconSkipForSample() const;
    void drawResults();                  // 搜索态：在面板里画结果列表
    int  resultRowAt(int x, int y) const;
    void moveResultSel(int delta);
    void openResult(int index, bool reveal);
    friend LRESULT CALLBACK islandSearchEditProc(HWND, UINT, WPARAM, LPARAM);

    void drawGridCell(int index, int cx, int cy, int cw, int ch, bool hover);

    // ── 顶部搜索框 ──

    // ── 左下用户 / 右下电源 ──
    void showUserMenu();
    void showPowerMenu();

    // ── 右键菜单 ──
    void showContextMenu(int screenX, int screenY, int gridIndex);
    void addShortcutDialog();

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    Config* cfg_ = nullptr;

    Surface base_;      // 缓存的玻璃底（内容大小）
    Surface frame_;     // 本帧：底 + 内容（内容大小）
    Surface shadow_;    // 画布大小：只含外圈影子
    Surface outer_;     // 画布大小：shadow_ + frame_ → 提交给窗口
    Backdrop backdrop_;
    bool baseValid_ = false;
    // 上次重抓背景时的前台窗口 —— 用来判断"背景要不要重抓"（透明实时更新）。
    // 对照标签面板的 HoverPanel::fgHwnd_。
    HWND fgHwnd_ = nullptr;
    bool dragLive_ = false;      // 拖动/缩放中（此时开着实时的背景重抓）

    bool visible_ = false;
    bool dark_ = true;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;

    // 拖动改大小
    bool resizing_ = false;
    int resizeEdge_ = 0;        // HTLEFT / HTRIGHT / HTBOTTOM / HTBOTTOMRIGHT ...
    POINT resizeStart_{};
    int resizeStartW_ = 0, resizeStartH_ = 0;
    bool moving_ = false;
    POINT moveStart_{};
    int moveStartX_ = 0, moveStartY_ = 0;

    // 布局几何
    int searchH_ = 0;           // 顶部搜索条高度
    int sideW_ = 0;             // 左侧分类栏宽度
    int gridX_ = 0, gridY_ = 0;
    int gridW_ = 0, gridH_ = 0;
    int cellW_ = 0, cellH_ = 0;
    int cols_ = 1, rows_ = 0;
    int scrollRow_ = 0;
    int maxScrollRow_ = 0;
    int iconSize_ = 0;

    // 底部两个按钮的热区
    RECT userBtn_{};
    RECT powerBtn_{};




    // 分类
    std::vector<Category> categories_;
    int hotCategory_ = -1;

    // 网格悬停
    int hotCell_ = -1;
    int pressedCell_ = -1;

    // ── 长按拖动换位 ──
    //
    // 交互设计：按住某个图标不放（超过 kDragHoldMs）进入拖动状态，
    // 拖动过程中鼠标下面的格子高亮，松手就换到那个位置。
    //
    // ⚠ 为什么要"长按"而不是直接拖：
    //   单击是"启动快捷方式"，直接拖会和单击冲突 ——
    //   手抖一下就会把图标挪走而不是打开程序。
    //   加个几百毫秒的延迟，单击和拖动就分得开了。
    int dragCell_ = -1;         // 正在拖的格子（-1 = 没在拖）
    bool dragArmed_ = false;    // 已按下但还没到长按时间
    DWORD dragStartTick_ = 0;   // 按下时刻，用来算有没有到长按时间
    POINT dragStartPt_ = {};    // 按下位置，用来算有没有移动
    int dragTarget_ = -1;       // 当前拖到哪个格子（松手会落在这）

    // ── 拖动过程中的实时跟随 ──
    //
    // 用户要求"能看到过程"：被拖的图标跟着鼠标走，
    // 其它图标实时让位。所以每帧都要知道鼠标在哪。
    POINT dragCurPt_ = {};      // 鼠标当前位置（客户区坐标）

    // 让位的动画进度（0~1）。
    //
    // ⚠ 为什么要缓动而不是直接跳：
    //   直接跳的话，鼠标一越过边界，后面的图标"啪"地整体挪一格，
    //   看起来很硬。用 0→1 的缓动插值，位移有过渡，像真的被挤开。
    //
    //   每次 dragTarget_ 变化就重置为 0，然后靠定时器推进。
    double dragShiftT_ = 1.0;
    int dragShiftFrom_ = -1;    // 上一帧的目标格，用来判断要不要重启动画

    // 让位动画的起始时刻。
    //
    // ⚠ 进度按**真实经过时间**算，不是每帧加固定值。
    //   帧驱动（dragShiftT_ += 0.18/帧）在 WM_TIMER 抖动时会忽快忽慢 ——
    //   系统忙的时候一帧可能是 30ms 而不是 16ms，
    //   同一个动画时快时慢，看起来就是"卡"。
    //   时间驱动则不管掉不掉帧，时长都稳定，才有"丝滑"的观感。
    DWORD dragShiftStart_ = 0;

    // 拖动跟手动画用的定时器（60fps 左右）
    static const UINT_PTR kDragAnimTimerId = 0x72;

    // 文字颜色策略 —— 跟随设置里的「文字颜色」。
    //
    // 和任务栏托盘文字用同一套判断：
    //   auto  按面板背后壁纸的亮度自动决定深浅
    //   dark  强制深色
    //   light 强制浅色
    //
    // 这样岛台和任务栏的文字观感一致，
    // 不会出现"任务栏用黑字、岛台用白字"的割裂。
    std::wstring textMode_ = L"auto";
    bool useDarkText_ = false;   // 根据 textMode_ 和实际亮度算出来的结果

    uint32_t textMain_ = 0xFFF5F5F7;
    uint32_t textDim_ = 0xFFC2C4CC;
};

}  // namespace hr