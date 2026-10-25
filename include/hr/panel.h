// hr/panel.h — 悬停展开面板（对照 ui/hover_panel.py）
#pragma once

#include <functional>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "hr/config.h"
#include "hr/glass.h"

namespace hr {

class HoverPanel {
public:
    // (快捷方式索引, 屏幕坐标)
    std::function<void(int, POINT)> onShortcutRightClick;
    std::function<void()> onLeavePanel;

    bool create(HINSTANCE hInst, Config* cfg);
    void destroy();

    // 在标签正下方展开
    void showForTab(int tabIndex, int x, int y, int width, int screenH);
    void hide();
    bool visible() const { return visible_; }
    bool mouseInside() const;
    HWND hwnd() const { return hwnd_; }

    void setDark(bool dark);
    void setFontSize(int size);
    void onConfigChanged();   // 当前标签内容变了 → 重绘
    bool currentTabValid() const { return tabIndex_ >= 0; }
    int currentTab() const { return tabIndex_; }

    // 显示期间 150ms 轮询：背景换了重抓；鼠标离开且不在标签栏则收起
    void pollTick(const RECT& barRect);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void layout();
    void rebuildBase();
    void redraw();
    void refreshBackdropLive();
    bool isOwnWindow(HWND h) const;
    int rowAt(int y) const;
    void setScroll(int row);
    void buildScrollbar();
    void launch(int rowIndex);

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    Config* cfg_ = nullptr;

    Surface base_;
    Surface frame_;
    Backdrop backdrop_;
    bool baseValid_ = false;
    std::wstring wallKey_;   // 上次烘焙用的壁纸路径（参与 hover_panel.py:296 的复用 key）

    // 面板开着的时候，只有前台窗口换了才重抓背景。
    // （Python 版 _check_background_changed 就是这么做的；每隔 150ms 无条件
    //   hide->抓屏->show 一次会让面板肉眼可见地频闪）
    HWND fgHwnd_ = nullptr;


    int tabIndex_ = -1;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool visible_ = false;
    bool dark_ = true;
    int fontSize_ = 10;
    int rowH_ = 0;
    int iconSize_ = 0;
    int top_ = 0;
    int rowX1_ = 0, rowX2_ = 0;
    int iconX_ = 0;
    int nameX_ = 0;
    int hoverRow_ = -1;

    // 是否已登记 TrackMouseEvent(TME_LEAVE)。
    // ⚠ 必须是成员：原来在 WM_MOUSEMOVE 里用 `static bool tracked`，
    //   只第一次登记，之后收不到 WM_MOUSELEAVE（见 panel.cpp 的说明）。
    //   hide() 时复位，下次进入重新登记。
    bool tracking_ = false;

    int scrollRow_ = 0;
    int visibleRows_ = 0;
    int maxScroll_ = 0;

    // 滚动条几何
    bool sbVisible_ = false;
    int sbX1_ = 0, sbX2_ = 0;
    int sbTrackY1_ = 0, sbTrackY2_ = 0;
    int sbThumbY1_ = 0, sbThumbY2_ = 0;
    int sbSpan_ = 1;
    int sbThumbH_ = 0;
    bool dragging_ = false;
    int dragStartY_ = 0;
    int dragStartRow_ = 0;

    uint32_t textMain_ = 0xFFF5F5F7;
    uint32_t textDim_ = 0xFFC2C4CC;
    uint32_t rowHoverColor_ = 0x20FFFFFF;
    uint32_t avg_ = 0xFF18181C;
};

}  // namespace hr
