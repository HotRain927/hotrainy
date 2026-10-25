// hr/tagbar/bar.h — 顶部标签栏（对照 ui/tab_bar.py）
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

namespace hr {

// 岛台启停的联动入口。
//
// 设置界面只知道自己改了个布尔值，但"启用岛台"要同时做两件事：
//   1. 让标签栏画出岛台按钮、并重排图标区
//   2. 通知 explorer 里的 DLL 隐藏/还原任务栏的「开始」「搜索」
//
// 这两件事都需要只存在于 main.cpp 的窗口实例，settings 拿不到。
// 所以由 main 在启动时把自己的实现挂上来，settings 通过这个函数调用。
//
// 指针为空时（比如单元测试环境）静默忽略。
using IslandEnableHook = void (*)(bool on);
void setIslandEnableHook(IslandEnableHook hook);
void islandSetEnabled(bool on);

// 统一状态机入口：读两个开关的当前值，分发到对应的处理函数。
//
// ⚠ 为什么不用 islandSetEnabled + SetTransparent 分别调：
//   分别调会有交叉影响（关透明化时连带关了岛台，等等）。
//   统一入口每次都重新读两个值，选最终状态，不会遗漏。
using IslandApplyStateHook = void (*)();
void setIslandApplyStateHook(IslandApplyStateHook hook);
void islandApplyState();

// 文字颜色策略变化的联动入口。
//
// 设置里改了「文字颜色」之后，除了任务栏托盘文字要跟着变，
// 岛台面板的文字也要跟着变 —— 否则会出现
// "任务栏用黑字、岛台用白字"的割裂观感。
//
// 同样由 main 挂实现：settings 拿不到岛台窗口实例。
using IslandTextHook = void (*)(const wchar_t* mode);
void setIslandTextHook(IslandTextHook hook);
void islandSetTextMode(const std::wstring& mode);

class BarWindow {
public:
    // 右侧固定图标
    enum IconKind { ICON_SNIP = 0, ICON_RECYCLE, ICON_COMPUTER, ICON_SETTINGS, ICON_COUNT };

    std::function<void(int, RECT)> onHoverTab;         // 悬停（35ms 停留后）
    std::function<void()> onLeave;                     // 移出标签栏
    std::function<void(int, bool, POINT)> onTabClick;  // (索引, 是否右键, 屏幕坐标)
    std::function<void(int)> onIconClick;              // 右侧图标
    // 岛台按钮被点击（左键）。参数是按钮的屏幕矩形，
    // 主程序用它当锚点决定面板弹在哪。
    std::function<void(RECT)> onIslandClick;

    bool create(HINSTANCE hInst, Config* cfg);
    void destroy();

    HWND hwnd() const { return hwnd_; }
    int height() const { return height_; }
    int tabCount() const { return (int)tabRects_.size(); }
    RECT tabRect(int idx) const;
    int tabAt(int x) const;
    int iconAt(int x) const;
    // 岛台按钮是否被点中（x 是客户区坐标）
    bool islandAt(int x) const;
    // 标签条（真正放标签的那一段）的右边界，右侧小图标区不算在内
    int tabAreaRight() const { return iconsStart_; }

    // 岛台按钮的屏幕矩形（面板定位用；未启用时返回空矩形）
    RECT islandButtonRect() const;

    // 岛台是否启用 —— 决定要不要画那个按钮、要不要给它留位置
    void setIslandEnabled(bool on);
    bool islandEnabled() const { return islandEnabled_; }
    // 岛台面板是否开着 —— 按钮要画成"按下"的样子
    void setIslandOpen(bool open);

    void setActive(int idx);
    int active() const { return active_; }
    void setDark(bool dark);
    bool dark() const { return dark_; }
    void setPanelOpen(bool open);
    void setMenuOpen(bool open);
    bool visible() const { return visible_; }

    void onConfigChanged();     // 标签增删/改名 → 重排
    void refreshBackdrop(bool force);
    void tickBackdrop();        // 700ms：巡检背后画面
    void autoHideTick();        // 150ms：自动隐藏
    void redraw();

    // 深浅色调在运行中变了（整屏平均亮度跨过阈值）→ 通知外层同步面板/搜索窗
    std::function<void(bool)> onDarkChanged;

    // ★ 鼠标不在标签栏窗口内就清掉悬停态（面板开着也清）。
    //   由 main.cpp 的 50ms 轮询调用 —— autoHideTick() 那个定时器实测没在跑，
    //   所以清理不能挂在它上面。
    void clearHoverIfMouseAway();
    void clearHover();           // 供外部（面板收起时）清掉残留的悬停态

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void relayout();
    void recomputeIconArea();    // 图标区起点（含岛台按钮那一格）
    void rebuildBase();          // 模糊底 + 渐变（缓存）
    void slideTo(int targetY);
    void place(int y);
    void showBar();
    void hideBar();

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    Config* cfg_ = nullptr;

    Surface base_;               // 缓存：模糊后的玻璃底
    Surface frame_;              // 本帧：底 + 胶囊 + 文字
    Backdrop backdrop_;
    uint32_t surfaceAvg_ = 0xFF18181C;
    uint32_t textMain_ = 0xFFF5F5F7;
    uint32_t textDim_ = 0xFFC2C4CC;

    std::vector<RECT> tabRects_;
    int active_ = -1;
    int hover_ = -1;
    int iconHover_ = -1;
    int width_ = 0;
    int height_ = 0;
    int tabW_ = 0;
    int iconW_ = 0;
    int iconsStart_ = 0;

    // ── 岛台按钮 ──
    //
    // 位置：图标区**最左边**（比截图/回收站/此电脑/设置更靠左）。
    // 它不占用 ICON_COUNT 那一组的下标 —— 那组是"系统工具"，
    // 岛台是另一类东西，混在一起会让 iconAt() 的语义变乱。
    // 所以单独记一个矩形，命中测试也单独做。
    bool islandEnabled_ = false;
    bool islandOpen_ = false;
    RECT islandRect_{};          // 客户区坐标

    bool dark_ = true;   // 永远深色玻璃（用户定调），不再随背景翻浅
    bool visible_ = true;
    bool panelOpen_ = false;
    bool menuOpen_ = false;
    int barY_ = 0;
    int slidingTo_ = 0;
    int hoverPending_ = -1;
    bool tracking_ = false;
    UINT_PTR hoverTimer_ = 0;
};

}  // namespace hr
