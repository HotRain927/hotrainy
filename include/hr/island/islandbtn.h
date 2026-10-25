// hr/island/islandbtn.h — 任务栏上的岛台按钮（一个小窗口）
//
// ═══════════════════════════════════════════════════════════════════════
// 【为什么是独立窗口，而不是插进任务栏的 XAML 树】
//
// 一开始的想法是往 explorer 的任务栏 XAML 树里插一个 Button 控件，
// 这样它就和开始按钮、应用图标同级，天生就是"图标组的一员"。
//
// 实测走不通：
//   · 开始按钮的父容器是 Microsoft.UI.Xaml.Controls.ItemsRepeater
//   · ItemsRepeater 用 ElementFactory 管理子元素，
//     外部直接 Children().InsertAt 进去的元素不在它的布局范围内
//   · 证据：插入后尺寸算得出来（40x32），但坐标是 (-10000,-10016) ——
//     XAML 的"未参与布局"哨兵值，元素被丢到屏幕外，看不见也点不到
//
//   · 要真插进去只能替换 ElementFactory 或 ItemsSource，
//     那是改任务栏模板，风险极高（很容易让任务栏崩）
//
// 【改成什么做法】
//
//   把开始按钮 Collapsed 掉（它的位置就腾出来了），
//   然后读它**原来占的那个坑**的屏幕坐标（实测 (982,1528) 68x72），
//   在那个位置上放我们自己的小窗口。
//
//   位置是从开始按钮实测的 → 视觉上和图标组严丝合缝，
//   不需要猜任何像素，也不用改用户的任何系统设置。
//
//   窗口用 SetParent 挂到 Shell_TrayWnd 下 →
//   随任务栏一起隐藏、置顶、移动。
// ═══════════════════════════════════════════════════════════════════════
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace hr {

class IslandButton {
public:
    // 点击回调。主程序在这里开/关岛台面板。
    std::function<void()> onClick;

    // 创建（不显示）。slot 是开始按钮原来占的那个坑，屏幕坐标。
    //
    // 窗口会挂到任务栏（Shell_TrayWnd）下面，
    // 所以 slot 会被换算成相对任务栏客户区的坐标。
    bool create(HINSTANCE hInst, const RECT& slot);

    // 移动到新位置（任务栏移动、分辨率变化、或坑位重新量出来时调）
    void moveTo(const RECT& slot);

    // 显示 / 隐藏
    void show();
    void hide();
    void destroy();

    bool alive() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

    // 岛台面板开着时按钮要高亮一下，让用户知道点过了
    void setActive(bool on);

    // 跟随文字颜色策略（和面板、托盘文字同一套）
    void setTextMode(const std::wstring& mode);

    // 强制指定深浅（"自动"模式下由主程序采样后调这个）。
    void setDarkText(bool dark) { darkText_ = dark; redraw(); }

    void setDark(bool dark) { dark_ = dark; redraw(); }

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);
    void redraw();

    // 实际绘制（提成独立方法，供 WM_PAINT 和 redraw 共用）。
    // 分层窗口是“推”模式，不能只靠 InvalidateRect。
    void paintNow();

    // 生成 H 的 HICON 并设给窗口。
    //
    // 任务栏显示的是**窗口图标**（WM_SETICON 设的那个），
    // 不是我们画的内容 —— 因为现在是原生图标模式，
    // 图标由任务栏绘制。
    void applyIcon();

    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    bool dark_ = true;
    bool active_ = false;
    bool hot_ = false;          // 鼠标悬停

    // 图标用深色还是浅色。
    // 跟随设置里的「文字颜色」策略，
    // 和托盘文字、面板文字同一套判断。
    bool darkText_ = false;
    bool tracking_ = false;     // 已注册 TrackMouseEvent

    RECT slot_{};               // 屏幕坐标下的坑位（保留原值，moveTo 用）
};

}  // namespace hr