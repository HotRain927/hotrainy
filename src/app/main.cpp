// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/main.cpp — 入口：编排标签栏 / 悬停面板 / 托盘 / 热键 / 巡检
#include "hr/shared/common.h"  // 必须最先：引入 windows.h，之后才能用 shellapi.h

#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <string>
#include <vector>

#include "hr/audio/audio.h"
#include "hr/tagbar/bar.h"
#include "hr/shared/config.h"
#include "hr/settings/dialog.h"
#include "hr/shared/hotkey.h"
#include "hr/shared/icons.h"
#include "hr/island/island.h"
#include "hr/island/islandbtn.h"
#include "hr/shared/menu.h"
#include "hr/tagbar/panel.h"
#include "hr/search/search.h"
#include "hr/settings/settings.h"
#include "hr/island/taskbar.h"
#include "hr/tb/xaml/injector.h"
#include "hr/text/theme.h"
#include "hr/app/app_context.h"
#include "hr/app/version.h"
#include "hr/app/menu_ids.h"
#include "hr/tagbar/tabs_menu_wire.h"
#include "hr/tagbar/tagbar_wire.h"
#include "hr/settings/settings_wire.h"
#include "hr/search/search_wire.h"
#include "hr/island/island_wire.h"
#include "hr/audio/audio_wire.h"

using namespace hr;

namespace {

const wchar_t* kAppName = L"hotrainy";
// ★ 版本号已抽到 hr/app/version.h（接线文件也要用，别再写死在这里）。
const UINT kTrayMsg = WM_APP + 10;
const UINT kTrayId = 1;
const UINT_PTR kAutoHideTimer = 0xC1;
const UINT_PTR kBackdropTimer = 0xC2;
const UINT_PTR kPanelPollTimer = 0xC3;
const UINT_PTR kFileDialogTimer = 0xC4;
    // 点面板外收回的检查。
    // ⚠ 单独开一个 50ms 的定时器，不跟 150ms 的那个合用 ——
    //   150ms 采不到短促的点击（按下到松开可能只有 80ms）。
    const UINT_PTR kClickOutTimer = 0xC5;

    // 点任务栏图标后的"抑制窗口"。
    //
    // ⚠ 为什么需要它：
    //   点图标会**同时**触发两条路径 ——
    //     · 任务栏发来的 WM_ACTIVATE（toggle 面板）
    //     · 我们自己的"点外检查"（看到鼠标在任务栏上就收面板）
    //   它们各干各的，互相抵消 —— 实测表现就是"点了没反应"。
    //
    //   收到 WM_ACTIVATE 时记下时刻，之后这段时间里
    //   "点外检查"一律跳过，把这次点击完整交给 toggle 处理。
    const DWORD kIconClickGraceMs = 500;

    // 最近一次"任务栏图标被点击"的时刻（GetTickCount）。
    DWORD g_iconClickTick = 0;

    // 挂起的"点外收起"。
    //
    // ⚠ 为什么不立刻收：
    //   点我们的任务栏图标时，会**同时**触发两条路径 ——
    //     · 本文件的 50ms 定时器（几十毫秒内就发现鼠标在任务栏上）
    //     · 任务栏发来的 WM_ACTIVATE（60~140ms 后才到）
    //   如果立刻收，WM_ACTIVATE 随后又把面板打开，净效果是"点了没反应"。
    //
    //   所以先挂起，等一小会儿看有没有 WM_ACTIVATE 来撤销。
    DWORD g_pendingCloseTick = 0;
    POINT g_pendingClosePt = {0, 0};

    // 挂起多久还没等到 WM_ACTIVATE 就真的收。
    //
    // 实测 WM_ACTIVATE 延迟 60~140ms，180ms 够用且更跟手。
    // ⚠ 这个等待**只对"点在任务栏上"生效** ——
    //   点别处是立刻收的（那边不可能有激活消息）。
    const DWORD kPendingCloseMs = 180;


Config g_cfg;
BarWindow g_bar;
HoverPanel g_panel;
SettingsWindow g_settings;
SearchWindow g_search;
IslandWindow g_island;

// 任务栏上的岛台按钮（独立小窗口，挂在 Shell_TrayWnd 下）
IslandButton g_islandBtn;

// 前向声明 —— applyIslandEnabled 用得到，
// 但它们定义在文件后面。
void toggleIslandPanel(RECT btn);
void trackIslandButtonSlot(bool force = false);
void checkClickOutsideIsland();
void flushPendingClose();
extern bool g_dark;
NOTIFYICONDATAW g_nid = {};
HICON g_icon = nullptr;
ULONG_PTR g_gdiplus = 0;
HWND g_msgWnd = nullptr;

// ── 岛台联动 ────────────────────────────────────────────────────────

// 岛台启停（给设置界面调用）。
//
// 设置界面改完 islandEnabled 走这里 —— 它自己拿不到窗口实例。
// 两件事一起做：
//   1. 标签栏重排（岛台不再占标签栏位置，但配置语义上要同步）
//   2. 通知 explorer 里的 DLL 隐藏/还原任务栏的「开始」「搜索」，
//      并插入/移除我们自己的岛台按钮
// ═══════════════════════════════════════════════════════════
// 统一状态机：先重置，再按两个开关的组合分发
// ═══════════════════════════════════════════════════════════
//
// 任何一个开关被打开或关闭时，都调 applyTaskbarState()：
//
//   ① 前置重置 —— 把任务栏恢复成系统默认，
//      清掉上一次组合留下的所有修改（透明、隐藏的按钮、岛台图标）。
//   ② 读两个开关的当前值。
//   ③ 按四种组合里选中的那一个，调对应的函数。
//
// ⚠ 为什么必须先重置：这样四个状态函数**只需要"加上"自己需要的东西**，
//   不必各自判断"要不要顺手把另一个功能关掉"。
//   以前正是那种"顺手关掉"的写法漏了路径，才出现
//   "关透明把岛台也关了""开岛台把任务栏弄透明了"这类互相干扰。
//
//   代价是每次切换会先还原一次再应用，但换来了确定性：
//   给定两个开关的值，任务栏必然回到同一个样子，
//   与上一次处于什么状态无关。

// 岛台开启要做的两件事：任务栏接管 + 放上岛台按钮。
void enableIslandTakeover() {
    bool ok = tb::xaml::setIslandTakeover(true);
    logf("state: 岛台接管下发 -> %s", ok ? "成功" : "失败");

    // 在任务栏上放岛台按钮。
    //
    // ⚠ 必须在 setIslandTakeover 之后读按钮坑位 ——
    //   那个调用里才把开始按钮 Collapsed，坐标是隐藏前记下的。
    //
    //   不过现在按钮是**任务栏原生图标**：位置由任务栏自己布局，
    //   我们完全不用管，拿不到坑位也不该阻止创建。那个矩形只是
    //   窗口创建参数，图标外观来自窗口的 HICON。
    if (!g_islandBtn.alive()) {
        RECT slot{};
        if (!tb::xaml::startButtonSlot(slot)) {
            const int sw = ::GetSystemMetrics(SM_CXSCREEN);
            const int sh = ::GetSystemMetrics(SM_CYSCREEN);
            slot.left = (sw - 68) / 2;
            slot.top = sh - 72;
            slot.right = slot.left + 68;
            slot.bottom = slot.top + 72;
        }

        g_islandBtn.onClick = onIslandButtonClick;   // 实现见 src/island/island_wire.cpp（合并了原来的两份）

        g_islandBtn.setDark(g_dark);
        g_islandBtn.create(GetModuleHandleW(nullptr), slot);

        // ⚠ 创建之后立刻套用当前的「文字颜色」策略。
        //   只在设置界面改动时才设的话，启动时会一直用默认色，
        //   要手动去设置里点一下才变。
        g_islandBtn.setTextMode(g_cfg.taskbarTextColor);
        g_islandBtn.show();

        logf("islandbtn: 已显示 @ (%d,%d) %dx%d，图标颜色策略=%s",
             slot.left, slot.top, slot.right - slot.left, slot.bottom - slot.top,
             toUtf8(g_cfg.taskbarTextColor).c_str());
    }

    g_bar.setIslandEnabled(true);
    g_bar.setIslandOpen(false);
}

// ── 状态 A：透明=开, 岛台=开 ──
// 任务栏透明 + 开始/搜索隐藏 + 岛台图标
void stateTransOnIslandOn() {
    logf("state: 透明=开, 岛台=开");
    tb::xaml::applyTransparent();
    enableIslandTakeover();
}

// ── 状态 B：透明=关, 岛台=开 ──
// 任务栏不透明 + 开始/搜索隐藏 + 岛台图标
void stateTransOffIslandOn() {
    logf("state: 透明=关, 岛台=开");
    // 透明化不用管 —— 前置重置已经把它恢复成系统默认（不透明）
    enableIslandTakeover();
}

// ── 状态 C：透明=开, 岛台=关 ──
// 任务栏透明 + 开始/搜索显示（无岛台图标）
void stateTransOnIslandOff() {
    logf("state: 透明=开, 岛台=关");
    tb::xaml::applyTransparent();
    // 岛台不用管 —— 前置重置已经还原了开始/搜索按钮
}

// ── 状态 D：透明=关, 岛台=关 ──
// 任务栏不透明 + 开始/搜索显示
void stateTransOffIslandOff() {
    logf("state: 透明=关, 岛台=关");
    // 两个都关：前置重置之后就已经是目标状态，无需任何额外操作
}

// ── 前置重置 ──
//
// 恢复系统默认：透明化的背景/边框、被隐藏的开始/搜索按钮，全部还原。
// 岛台按钮窗口也收掉，后面的状态函数需要时再建。
void resetTaskbarState() {
    // ⚠ 没有 DLL 在听就别发还原指令。
    //
    //   没有 DLL 就意味着任务栏上没有任何我们留下的修改，
    //   本来也无事可做；而 restoreDefault 会等通道应答，
    //   冷启动时要白等 2 秒才超时（日志里一条"无响应"）。
    //
    //   注意区分：启动时"清理上个实例的残留"由
    //   taskbarService().ForceReset() 负责 —— 那个即使自己没注入
    //   也要发，因为残留的 DLL 可能是别的实例留下的。
    if (tb::xaml::hasListener()) {
        tb::xaml::restoreDefault();
    }

    if (g_islandBtn.alive()) {
        g_islandBtn.destroy();
        logf("islandbtn: 已销毁（前置重置）");
    }

    g_island.hide();
    g_bar.setIslandEnabled(false);
    g_bar.setIslandOpen(false);
}

// ── 统一入口 ──
//
// 任何开关变化都调这里：不需要知道"哪个变了"，它每次都先重置、
// 再读两个值、再选对应的那个状态执行。
void applyTaskbarState() {
    resetTaskbarState();                            // ① 先重置

    const bool trans  = g_cfg.taskbarTransparent;   // ② 读两个开关
    const bool island = g_cfg.islandEnabled;

    if (trans && island)       stateTransOnIslandOn();        // ③ 选一个
    else if (!trans && island) stateTransOffIslandOn();
    else if (trans && !island) stateTransOnIslandOff();
    else                       stateTransOffIslandOff();

    // ④ ★ 最后统一再套一次「文字颜色」。
    //
    //   四个状态分支都只管透明/岛台按钮，**完全不碰文字颜色** ——
    //   而"关透明再开""换壁纸"这些路径会让注入侧把它交还给系统默认，
    //   于是设置里明明是深色，任务栏右边却是浅色（用户："有时候跟设置不一致"）。
    //   放在这里收口：不管走了哪个分支，结束时文字颜色一定等于设置里的那一档。
    taskbarService().SetTextColor(g_cfg.taskbarTextColor);
    taskbarService().ApplyTextColor();
}

void applyIslandEnabled(bool on) {
    // 关掉时先收面板 —— 按钮都没了，面板留着会变成孤儿窗口
    if (!on) g_island.hide();

    g_bar.setIslandEnabled(on);
    g_bar.setIslandOpen(false);

    bool ok = tb::xaml::setIslandTakeover(on);
    logf("island: 启用=%d，任务栏接管 -> %s", (int)on, ok ? "成功" : "失败");

    // 在任务栏上放/收岛台按钮。
    //
    // 位置从开始按钮原来占的那个坑读出来。
    // 因为位置是实测的，视觉上和图标组天然对齐。
    //
    // ⚠ 必须在 setIslandTakeover 之后读 ——
    //   那个函数里才把开始按钮 Collapsed，
    //   坐标是隐藏前记下的，现在读正好。
    if (on) {
        // ⚠ 不再拿"开始按钮坑位"当门槛。
        //
        //   那是**旧方案（独立窗口）**的前提 —— 旧方案要把按钮
        //   精确摆在开始按钮原来的位置，量不到就摆不了，
        //   所以必须先查坑位。
        //
        //   现在按钮是**任务栏原生图标**：位置由任务栏自己布局，
        //   我们完全不用管。拿不到坑位也不该阻止创建。
        //
        //   实测就是这么挂的：
        //     islandbtn: 拿不到开始按钮坑位，按钮不显示
        //     HotrainyIslandBtn: 未找到        ← 窗口压根没建
        //
        //   窗口总得有个矩形，给个合理的默认值。
        //   任务栏图标的外观来自窗口的 HICON，和这个矩形无关。
        RECT slot{};
        if (!tb::xaml::startButtonSlot(slot)) {
            // 兜底：任务栏下半部分、横向居中一带
            const int sw = ::GetSystemMetrics(SM_CXSCREEN);
            const int sh = ::GetSystemMetrics(SM_CYSCREEN);
            const int w = 68, h = 72;
            slot.left = (sw - w) / 2;
            slot.top = sh - h;
            slot.right = slot.left + w;
            slot.bottom = slot.top + h;
            logf("islandbtn: 坑位拿不到，用兜底矩形 (%d,%d) %dx%d",
                 slot.left, slot.top, w, h);
        }

        {
            if (!g_islandBtn.alive()) {
                g_islandBtn.onClick = onIslandButtonClick;   // 实现见 src/island/island_wire.cpp（合并了原来的两份）
                g_islandBtn.setDark(g_dark);
                g_islandBtn.create(GetModuleHandleW(nullptr), slot);
            } else {
                g_islandBtn.moveTo(slot);
            }
            // ⚠ 创建之后立刻套用当前的「文字颜色」策略。
            //
            //   踩过的坑：只在设置界面改动时才调 applyIslandTextMode，
            //   启动时不调 —— 结果设置里明明是"深色"，
            //   按钮图标却一直是白色，要手动去设置里点一下才变。
            //
            //   这类"启动时忘了初始化"的问题很典型：
            //   回调路径都通了，但初始状态没设置。
            g_islandBtn.setTextMode(g_cfg.taskbarTextColor);

            g_islandBtn.show();
            logf("islandbtn: 已显示 @ (%d,%d) %dx%d，图标颜色策略=%s",
                 slot.left, slot.top, slot.right - slot.left, slot.bottom - slot.top,
                 toUtf8(g_cfg.taskbarTextColor).c_str());
        }
    } else {
        g_islandBtn.destroy();
        logf("islandbtn: 已销毁");
    }
}

// 岛台面板开关（给标签栏按钮用）
void toggleIslandPanel(RECT btn) {
    // ★★ 去抖：一次真实点击可能产生两次"点击"信号 ★★
    //
    //   实测（用户日志 03:11:49）：点一下任务栏上的岛台图标，
    //   我们的按钮窗口收到了**两次** WM_ACTIVATE(WA_ACTIVE)，
    //   间隔约 490ms。于是面板被 toggle 两次 —— 关了又开，
    //   用户看到的就是"面板有时候自己又冒出来了"。
    //
    //   另外还有第二条独立的点击来源：DLL 侧的 consumeIslandTap()
    //   也会报"任务栏按钮被点击"。两条路都可能报同一次点击。
    //
    //   所以在**这个统一的入口**去抖，不管信号从哪来都盖住。
    //   600ms 是实测那次重复间隔（490ms）之上留的余量 ——
    //   人不会在 600ms 内真的想开关两次。
    static DWORD lastToggle = 0;
    const DWORD now = ::GetTickCount();
    if (now - lastToggle < 600) {
        logf("island: 忽略 %lums 内的重复切换（去抖）",
             (unsigned long)(now - lastToggle));
        return;
    }
    lastToggle = now;

    g_island.toggle(btn.left, btn.bottom + px(4));
    g_bar.setIslandOpen(g_island.visible());
    logf("bar: 岛台面板 %s", g_island.visible() ? "打开" : "关闭");
}

// 文字颜色策略变化（给设置界面调用）。
//
// 岛台和任务栏托盘文字用同一套深浅判断，
// 设置里改一次两边一起变 —— 否则会出现
// "任务栏黑字、岛台白字"的割裂。
void applyIslandTextMode(const wchar_t* mode) {
    if (!mode) return;

    // ⚠ 只有**任务栏上的按钮**跟随这个设置。
    //
    //   岛台面板的文字永远是白色（面板是深色玻璃，
    //   切深色会变成黑字黑底，看不清）。
    //   这里不传给它 —— 传了也没用，面板内部会忽略。
    //
    //   任务栏托盘的文字由 taskbarService 单独处理，走另一条通道。
    g_islandBtn.setTextMode(mode);

    logw(L"island: 按钮图标颜色策略同步为 %s", mode);
}

// 轮询任务栏上的岛台按钮有没有被点。
//
// 按钮的 Tapped 回调在 explorer 里，它只置一个标志位；
// 这里定时取走 —— 取到了就开/关面板。
//
// 频率 150ms：和面板的悬停轮询一致，点击响应感觉不到延迟，
// 也不至于让通道太忙。
// 让任务栏上的岛台按钮跟随开始按钮的位置。
//
// ⚠ 为什么需要这个：
//   任务栏图标是**居中**排列的。图标一多，整组变宽、起点左移；
//   图标一少，整组变窄、起点右移。
//   只在启动时读一次坑位，图标数量一变按钮就不在正确的格子上了。
//
// 做法：定时重新查坑位，位置变了就把按钮挪过去。
//
// 频率 500ms：
//   · 图标增减是低频操作（用户点一下固定才会有变化）
//   · 每次查询要跨进程通信，再快是浪费
//   · 任务栏自己排版也要时间，500ms 足够错开
void trackIslandButtonSlot(bool force) {
    if (!g_cfg.islandEnabled) return;
    if (!g_islandBtn.alive()) return;

    // force = true 时不看缓存，强制重新读一次坑位。
    // 用在"任务栏图标增减"这种必须立刻对齐的场合。

    RECT slot{};
    if (!tb::xaml::startButtonSlot(slot)) return;

    // ⚠ 不再拿窗口矩形做比较。
    //
    //   踩过的坑：原来是 GetWindowRect 和坑位比。
    //   但窗口现在固定 2x2（为了压住缩略图预览），坑位是 68x72 ——
    //   尺寸永远不相等，于是每 500ms 都判成"变了"，
    //   日志一直刷，而且打出来的坐标还是一样的（看着很怪）。
    //
    //   改成只比坑位本身 —— 那才是我们关心的东西。
    static RECT lastSlot{};
    if (lastSlot.right != 0 &&
        lastSlot.left == slot.left && lastSlot.top == slot.top &&
        (lastSlot.right - lastSlot.left) == (slot.right - slot.left) &&
        (lastSlot.bottom - lastSlot.top) == (slot.bottom - slot.top)) {
        return;
    }
    lastSlot = slot;

    g_islandBtn.moveTo(slot);
    logf("islandbtn: 坑位变化，跟随到 (%d,%d) %dx%d",
         slot.left, slot.top, slot.right - slot.left, slot.bottom - slot.top);
}

// 点岛台面板外面的地方时，把面板收起来。
//
// ⚠ 用户的要求演变过一次，记清楚，别再搞反：
//   最早："面板不因鼠标离开而关闭，要用右上角的 × 关"
//         → 所以不能用"鼠标移开就收"
//   现在："点面板外面要自动收回"
//         → 移开不收，但**点到外面**要收
//
// 实现：
//   每帧检查鼠标左键是不是刚按下（GetAsyncKeyState 的 0x0001 位
//   表示"自上次调用以来按过"）。
//   按下了就问 WindowFromPoint：那个位置是不是我们面板的一部分？
//   不是 → 收起。
//
//   为什么不用 WM_ACTIVATE / WM_KILLFOCUS：
//     面板是 SWP_NOACTIVATE 显示的，很多时候收不到激活消息。
//     实测那条路不可靠。
//
//   为什么不装在面板自己的 WndProc 里：
//     点到面板外面时，鼠标消息根本不会送到我们窗口 ——
//     系统把它送给被点中的那个窗口了。
//     所以在主程序的定时器里全局检查。
// 把挂起的"点外收起"落实（或者丢弃）。
//
// 由定时器每次调用。逻辑见 g_pendingCloseTick 的说明。
void flushPendingClose() {
    if (!g_pendingCloseTick) return;

    // 面板已经关了 —— 挂起没意义了
    if (!g_island.visible()) {
        g_pendingCloseTick = 0;
        return;
    }

    // 还没等够，继续等 WM_ACTIVATE
    if (::GetTickCount() - g_pendingCloseTick < kPendingCloseMs) return;

    logf("island: 点外收起落实 (%d,%d)", g_pendingClosePt.x, g_pendingClosePt.y);
    g_island.hide();
    g_bar.setIslandOpen(false);
    g_pendingCloseTick = 0;
}

void checkClickOutsideIsland() {
    // 检测鼠标左键"刚被按过"。
    //
    // ★★ 为什么必须用 0x0001 锁存位 ★★
    //
    //   现象：点面板外要**按久一点**才收，轻点没反应。
    //   原因：这个检查是 50ms 的定时器，而 0x8000 位表示
    //         "**此刻**是否按下" —— 轻点只持续 30~50ms，
    //         两次采样之间就按完了，定时器根本没看见。
    //
    //   0x0001 位的语义是"自**上一次调用本函数**以来按过"，
    //   系统会把"按下"这件事锁存住，直到我们下次查询。
    //   所以再快的轻点也不会丢。
    //
    //   ⚠ 我一度放弃过这个位，理由是"轮询下不稳定" ——
    //     那个判断是错的：当时真正的问题是面板压根没打开
    //     （WM_ACTIVATE 收不到），跟这个位无关。
    //
    //   ⚠ 副作用：这个位会被**任何**查询 VK_LBUTTON 的代码清掉。
    //     所以本进程里只允许这一处查它。目前确实只有这一处。
    //
    //   保险：锁存位和上升沿两个都看，任一命中就算点击。
    const SHORT st = ::GetAsyncKeyState(VK_LBUTTON);
    const bool latched = (st & 0x0001) != 0;    // 自上次查询以来按过

    const bool down = (st & 0x8000) != 0;       // 此刻正按着
    static bool prevDown = false;
    const bool justPressed = down && !prevDown; // 上升沿（兜底）
    prevDown = down;

    // ★★ 状态更新必须在守卫之前 ★★
    //
    //   踩过的坑：原来写成"先判断抑制窗口、再更新 prevDown"，
    //   提前 return 让 prevDown 永远陈旧；
    //   抑制窗口一过，只要按键还按着，justPressed 立刻为 true，
    //   又触发一次"点外收起"。
    //   所以 prevDown 的更新在上一行就已经做了。

    if (!latched && !justPressed) return;

    if (!g_island.visible()) return;

    // ⚠ 刚点过任务栏图标 → 这次点击由 WM_ACTIVATE 那条路处理，
    //   这里必须让路。
    //
    //   不让路的话两条路会互相抵消：
    //     点外检查先收 → WM_ACTIVATE 后到又开 → 用户觉得"点了没反应"。
    //
    //   ⚠ 这个判断放在状态更新**之后**是关键 ——
    //     放前面会让 prevDown 永远陈旧。
    if (::GetTickCount() - g_iconClickTick < kIconClickGraceMs) {
        return;
    }

    POINT pt{};
    ::GetCursorPos(&pt);

    // 点到哪儿了？
    HWND hit = ::WindowFromPoint(pt);

    // 面板自己、或者面板的子窗口（Edit 等）—— 都算"在里面"
    auto isOurs = [&](HWND h) {
        if (!h) return false;
        HWND root = ::GetAncestor(h, GA_ROOT);
        // 面板
        if (root == g_island.hwnd()) return true;
        // 我们自己的其它窗口（菜单、对话框、搜索窗）也算 ——
        // 点这些是正常交互，不该收面板
        if (root == g_bar.hwnd()) return true;
        if (root == g_msgWnd) return true;
        // 任务栏上的岛台按钮。
        //
        // ⚠ 用 g_islandBtn.hwnd() 拿真实句柄，**不要按类名 FindWindow**。
        //   踩过的坑：类名从 HotrainyIslandBtn 改成 HotrainyIslandBtnV2
        //   之后，按旧类名查永远返回 NULL，
        //   于是"点在按钮上"被误判成"点在外部"。
        if (g_islandBtn.alive()) {
            HWND btn = g_islandBtn.hwnd();
            if (root == btn || hit == btn) return true;
        }
        // 右键菜单 / 用户菜单 / 电源菜单 / 搜索窗 / 对话框 ——
        // 同进程的窗口一律算"我们自己"，点它们不该收面板。
        //
        // ⚠⚠ 这里原来是个写了很久的 bug：拿 GetWindowThreadProcessId
        //     的**返回值**（线程 ID）去和 GetCurrentProcessId()（进程 ID）
        //     比较 —— 这俩根本不是一回事，比较几乎永远为假。
        //
        //     于是"点电源菜单"被判成"点到面板外"：
        //     面板立刻收起，而收起又会把模态菜单一起关掉，
        //     用户看到的就是"电源键打不开了"。
        //
        //     正确写法是用 out 参数拿进程 ID 再比。
        if (root) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(root, &pid);
            if (pid == ::GetCurrentProcessId()) return true;
        }
        return false;
    };

    if (isOurs(hit)) {
        // 打一条日志（限流），便于排查"点了没反应"。
        //
        // ⚠ 这里以前是静默 return，导致实测时完全不知道
        //   检查有没有跑、判断成了什么。
        static DWORD lastLog = 0;
        const DWORD now = ::GetTickCount();
        if (now - lastLog > 2000) {
            lastLog = now;
            wchar_t cls[64] = L"?";
            if (hit) ::GetClassNameW(::GetAncestor(hit, GA_ROOT), cls, 64);
            logf("island: 点在我们自己的窗口上 (%d,%d) class=%ls，不收",
                 pt.x, pt.y, cls);
        }
        return;
    }

    // ⚠ 点到了外面。但还要排一种情况：
    //   用户点的是**岛台按钮**想关面板 —— 那个按钮在任务栏上，
    //   属于别的进程（explorer 的子窗口其实是我们的进程，
    //   但 WindowFromPoint 可能返回 Shell_TrayWnd）。
    //
    //   判据：如果点击位置落在岛台按钮的矩形里，就交给按钮的
    //   onClick 处理（它自己会 toggle），这里不要动。
    if (g_islandBtn.alive()) {
        RECT br{};
        if (::GetWindowRect(g_islandBtn.hwnd(), &br) &&
            pt.x >= br.left && pt.x < br.right &&
            pt.y >= br.top && pt.y < br.bottom) {
            return;
        }
    }

    // ★ 分两种情况，别一律挂起 —— 那会白白多等 180ms。
    //
    //   ① 点在**任务栏**上：可能是点我们的岛台图标，
    //      而任务栏的 WM_ACTIVATE 要 60~140ms 后才到。
    //      所以先挂起，给激活一个机会来撤销
    //      （否则它随后又把面板打开，用户觉得"点了没反应"）。
    //
    //   ② 点在**别处**（桌面、其它应用、空白）：立刻收。
    //      那些地方不可能引出任何激活消息，等待纯属浪费 ——
    //      这也是用户感觉"收回慢"的主因。
    wchar_t cls[64] = L"?";
    if (hit) ::GetClassNameW(::GetAncestor(hit, GA_ROOT), cls, 64);

    const bool onTaskbar = (wcscmp(cls, L"Shell_TrayWnd") == 0 ||
                            wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0);

    if (!onTaskbar) {
        // 立刻收，零延迟
        logf("island: 点到面板外 (%d,%d) class=%ls，立刻收回", pt.x, pt.y, cls);
        g_island.hide();
        g_bar.setIslandOpen(false);
        g_pendingCloseTick = 0;
        return;
    }

    g_pendingCloseTick = ::GetTickCount();
    g_pendingClosePt = pt;
    logf("island: 点到面板外 (%d,%d) class=%ls，挂起待定", pt.x, pt.y, cls);
}

// ── 图标预热 ────────────────────────────────────────────────────────
//
// ⚠ 为什么需要它（实测数据）：
//
//   首次展开悬停面板时，每个图标要
//     SHGetFileInfoW + DrawIconEx + 逐像素转换
//   实测 10~15ms 一个。Tools 标签 13 个快捷方式 →
//     "panel: 展开 ... 绘制=187ms 合计=203ms"
//   用户的感觉就是"标签面板出现有点慢"。
//
//   图标本身是有缓存的（icons.cpp 的 g_cache），所以只要提前解码一次，
//   之后的展开就是瞬间（实测第二次展开只有 47ms，且其中大部分是文字排版）。
//
// 做法：
//   借 150ms 的轮询定时器**分片**预热 —— 每拍只做一个路径。
//   一个路径要解两个尺寸（悬停面板用 48px、岛台用 px(48)），
//   合计约 25ms，占一拍的 1/6，不会让人感觉到卡。
//
//   为什么不一次性做完：那会在启动瞬间卡半秒。
//   为什么不新开线程：SHGetFileInfo / 图标提取要 COM 与 shell，
//   多线程得自己初始化 COM、还要给 g_cache 加锁，得不偿失。
void prewarmIconsStep() {
    static std::vector<std::wstring> todo;
    static size_t idx = 0;
    static bool built = false;

    if (!built) {
        built = true;
        auto add = [&](const Shortcut& s) {
            todo.push_back(s.iconPath.empty() ? s.targetPath : s.iconPath);
        };
        for (const Tab& t : g_cfg.tabs) {
            for (const Shortcut& s : t.shortcuts) add(s);
        }
        for (const Shortcut& s : g_cfg.islandShortcuts) add(s);
        logf("icons: 预热清单 %d 个（悬停面板 48px / 岛台 %dpx）",
             (int)todo.size(), px(48));
    }

    if (idx >= todo.size()) return;

    iconFor(todo[idx], 48);        // 悬停面板用（见 panel.cpp 的 max(48, iconSize_)）
    iconFor(todo[idx], px(48));    // 岛台网格用
    ++idx;

    if (idx >= todo.size()) logf("icons: 预热完成，共 %d 个", (int)todo.size());
}

void pollIslandButton() {
    if (!g_cfg.islandEnabled) return;

    // ★★ 已停用 DLL 侧的"岛台按钮被点击"探测 ★★
    //
    //   那条路的原理是：往任务栏的 XAML 树里插一个 Button 并挂 Tapped
    //   （见 visualtree.cpp 的 probeInjectElement）。
    //
    //   而"插进任务栏 XAML 树"是**早期就确认走不通的废弃方案** ——
    //   元素插进去了但不参与布局，停在 -10000 的未布局坐标上，不渲染。
    //
    //   问题在于：按钮虽然不显示，**Tapped 事件却会凭空触发**。
    //   实测（用户机器上的日志）：没有任何人碰鼠标，
    //   它照样报"取到任务栏按钮点击 → 任务栏按钮被点击"，
    //   于是面板自己弹出来 —— 这就是用户反复说的
    //   "岛台面板有时候会自启动"。
    //
    //   现在岛台按钮是任务栏上的**原生图标窗口**，
    //   点击由它自己的 WM_ACTIVATE 上报（见 enableIslandTakeover
    //   里挂的 onClick）。那条路实测可靠，不再需要 DLL 帮忙探测。
    //
    //   所以这里刻意什么都不做 —— 保留函数是为了不动调用结构。
    static int tickLog = 0;
    if (tickLog < 3) {
        ++tickLog;
        logf("island: 轮询任务栏按钮（第 %d 次，DLL 探测已停用）", tickLog);
    }
}
bool g_dark = true;
bool g_searchHotkeyOk = false;
HWND g_dialogSeen = nullptr;       // 当前看到的文件对话框 hwnd
HWND g_dialogDismissed = nullptr;  // 用户手动关掉过的那个，别再弹

HICON makeTrayIcon(int size) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) return nullptr;

    uint32_t* px = (uint32_t*)bits;
    int pad = size / 8;
    int r = size / 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int idx = y * size + x;
            bool inside = x >= pad && x < size - pad && y >= pad && y < size - pad;
            if (inside) {
                int cx = (x < pad + r) ? pad + r : (x >= size - pad - r ? size - pad - r - 1 : x);
                int cy = (y < pad + r) ? pad + r : (y >= size - pad - r ? size - pad - r - 1 : y);
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) inside = false;
            }
            if (!inside) {
                px[idx] = 0;
                continue;
            }
            int t = (y - pad) * 100 / std::max(1, size - 2 * pad);
            int v = 235 - t * 55;
            px[idx] = (255u << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)(v + 10);
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

void trayCreate(HWND owner) {
    g_icon = makeTrayIcon(32);
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = owner;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMsg;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"hotrainy — 屏幕顶部标签栏");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    logf("tray: 已创建");
}

void trayDestroy() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
}

void showPanelForTab(int idx, RECT bounds, bool force = false) {
    // ★ 点击路径带 force=true，不受这个护栏限制
    // ★ 互斥要彻底：点击模式下**不管面板开没开**，
    //   悬停都不允许把面板调出来/切过去（只有点击带 force）。
    //   原来多了一个 !g_panel.visible()：面板一旦打开，
    //   悬停其他标签就会把面板切过去 —— 看起来就是“悬停和点击不互斥”。
    if (!force && !g_cfg.hoverMode) return;
    RECT wr = {};
    GetWindowRect(g_bar.hwnd(), &wr);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    g_panel.showForTab(idx, wr.left + bounds.left, g_bar.height(), bounds.right - bounds.left,
                       screenH);
    g_bar.setPanelOpen(true);
    // 悬停不改「激活标签」—— 对照 tab_bar.py：active_index 只在点击时变，
    // hover_index 是另一层淡胶囊，鼠标一离开就自己消失。
    // 以前这里顺手 setActive，鼠标走了胶囊还留在那个标签上，非自动隐藏时特别明显。
}

void hidePanel() {
    g_panel.hide();
    g_bar.setPanelOpen(false);
}

// 搜索热键：设置里改了立即重注册，失败返回 false（设置窗口会显示「已被占用」）
// ★ registerSearchHotkey() 已搬到 src/search/search_wire.cpp。

// 设置窗口改完一项后调用：把变化推给各部件
bool applySetting(const std::string& key) {
    // ★ 这里现在只是**派发**：每个模块只管自己那几个键
    //   （handled = 这个键是不是我的；返回值 = 生效成功与否）。
    //   加设置项时只改**归属模块**的 xxx_applySetting，main.cpp 不用动 ——
    //   归属见 docs/代码结构.md 第七节那张表。
    bool handled = false;
    bool ok = true;
    if (!handled) ok = tagbar_applySetting(key, handled);
    if (!handled) ok = island_applySetting(key, handled);
    if (!handled) ok = search_applySetting(key, handled);
    if (!handled) ok = audio_applySetting(key, handled);
    return ok;
}

// ★ openSettings() 已搬到 src/settings/settings_wire.cpp（声明见 settings_wire.h）。

// ── 右键菜单的增删改（对照 main.py 的 CRUD 一段） ──

LRESULT CALLBACK msgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kTrayMsg:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) trayMenu(hwnd);
            return 0;
        case WM_HOTKEY:
            logf("WM_HOTKEY id=%u", (unsigned)wp);
            if (wp == kHotkeyHideBar) {
                // Ctrl+Alt+H：强制显隐
                POINT p = {};
                GetCursorPos(&p);
                if (g_bar.visible()) {
                    SetCursorPos(p.x, GetSystemMetrics(SM_CYSCREEN) - 1);
                } else {
                    SetCursorPos(p.x, 0);
                }
            } else if (wp == kHotkeySearch) {
                hidePanel();
                g_search.toggle();
            }
            return 0;
        case WM_TIMER:
            if (wp == kAutoHideTimer) {
                g_bar.autoHideTick();
                return 0;
            }
            if (wp == kBackdropTimer) {
                g_bar.tickBackdrop();
                return 0;
            }
            if (wp == kClickOutTimer) {
                // 点面板外面就收起。
                //
                // 50ms 一次：比 150ms 更能采到短促点击。
                // 这个检查只调两个 API，代价可忽略。
                checkClickOutsideIsland();
                flushPendingClose();

                // ★ 任务栏图标增减 → 立刻让岛台按钮跟上。
                //
                // 放在这个 50ms 的定时器里而不是 150ms 那个：
                //   图标重排是瞬间完成的，我们越早跟上越好。
                //   50ms 的延迟用户完全看不到重叠。
                //
                // 顺带说明为什么不用更高的频率（比如 16ms）：
                //   每次查询都是一次跨进程通信，16ms 一次代价不划算。
                //   50ms 是延迟和开销的折中 —— 实测够用。
                if (g_cfg.islandEnabled && tb::xaml::consumeLayoutChanged()) {
                    logf("islandbtn: 收到布局变化通知，立刻重定位");
                    trackIslandButtonSlot(true);
                }
                return 0;
            }
            if (wp == kPanelPollTimer) {
                if (g_panel.visible()) {
                    RECT wr = {};
                    GetWindowRect(g_bar.hwnd(), &wr);
                    // 右侧小图标区不算「还在标签条上」：鼠标挪到图标上就该收面板。
                    // 不收的话，从最右标签往右划到图标上，面板会一直挂在下面。
                    wr.right = wr.left + g_bar.tabAreaRight();
                    g_panel.pollTick(wr);
                // 岛台面板也要跟着前台窗口实时重抓背景（透明实时更新）
                g_island.pollTick();
                // ★ 标签栏：鼠标离开就清悬停态（胶囊残留问题的正解）
                g_bar.clearHoverIfMouseAway();
                }
                // 顺便看看任务栏上的岛台按钮被点了没有。
                // 合用一个定时器：频率一样，没必要多开一个。
                pollIslandButton();

                // 图标预热：每拍一个（见 prewarmIconsStep 的说明）。
                // 放在这里是因为 150ms 一拍的节奏正好够"分片"，
                // 不会像 40ms 那个定时器一样密集到能感觉出卡。
                prewarmIconsStep();

                // 跟踪岛台按钮的位置。
                //
                // ⚠ 这个定时器是 150ms，但跟踪只要 500ms 一次 ——
                //   跨进程查询没必要那么勤，而且图标增减本来就是低频操作。
                //   所以在这里自己节流。
                {
                    static DWORD lastTrack = 0;
                    const DWORD now = ::GetTickCount();
                    if (now - lastTrack >= 500) {
                        lastTrack = now;
                        trackIslandButtonSlot();
                    }
                }

                return 0;
            }
            if (wp == kFileDialogTimer) {
                // 文件对话框一出现就把搜索框贴上去（对照 main.py:_watch_file_dialog）
                if (g_cfg.searchEnabled && g_cfg.searchDialogAuto) {
                    RECT rc = {};
                    HWND dlg = nullptr;
                    if (fileDialogBounds(rc, dlg)) {
                        if (dlg != g_dialogSeen) {
                            g_dialogSeen = dlg;
                            g_dialogDismissed = nullptr;
                        }
                        if (dlg != g_dialogDismissed) {
                            if (!g_search.visible()) {
                                hidePanel();
                                g_search.show(false);
                            } else if (g_search.attached() != dlg && !g_search.userDetached()) {
                                g_search.attachTo(dlg, true);
                            }
                        }
                    } else {
                        g_dialogSeen = nullptr;
                        g_dialogDismissed = nullptr;
                    }
                }
                return 0;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}



}  // namespace

int main() {
    logInit();
    logf("===== hotrainy(C++) 启动 =====");

    // ⚠ 单实例保护 —— 必须放在最前面，早于任何窗口创建。
    //
    //   不做的后果（实测踩过）：
    //     连点几次 exe 就起来好几个实例，每个都建了一套
    //     标签栏/岛台/搜索窗口，全都叠在一起。
    //     点标签栏右侧图标时，命中测试落在"上面那个"实例上，
    //     而它可能是后启的、状态不全的那个 ——
    //     表现出来就是"有的图标点不开""点了没反应"，
    //     而且关掉一个还有别的在跑，怎么都退不干净。
    //
    //   用命名互斥体判断。名字带 Local\ 前缀：
    //   同一用户会话内唯一，多用户登录时互不干扰。
    HANDLE single = ::CreateMutexW(nullptr, TRUE, L"Local\\hotrainy_single_instance_v1");
    if (single && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有一个在跑了。把它叫到前面来，然后自己退出。
        logf("启动: 已有实例在运行，本次退出");

        // 尝试激活已有实例的标签栏，让用户看到"程序还活着"
        if (HWND bar = ::FindWindowW(L"HotrainyBar", nullptr)) {
            ::ShowWindow(bar, SW_SHOW);
        }
        ::CloseHandle(single);
        return 0;
    }

    enablePerMonitorDpi();
    dpiScale();

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCo)) logf("CoInitializeEx 失败 0x%08lX", (unsigned long)hrCo);

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g_gdiplus, &gsi, nullptr);

    std::vector<std::string> warns;
    bool ok = loadConfig(g_cfg, warns);
    for (const auto& w : warns) logf("config: %s", w.c_str());
    logf("config: %s，%d 个标签，字号 %d", ok ? "读到了" : "用默认", (int)g_cfg.tabs.size(),
         g_cfg.shortcutFontSize);

    // ★ 不再强制关闭音频条：用户要求"下次启动时所有东西都保持上次设置的状态"，
    //   所以 audioBar 的开关状态照配置读（原来这里无条件置 false，是照 main.py:210
    //   的老行为 —— 那会导致"开了音频条，下次启动又没了"，与用户要求矛盾）。

    // 深浅固定为深色（用户要求）：不判整屏/脚下，避免奶白玻璃发白

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = msgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HotrainyMsg";
    RegisterClassExW(&wc);
    g_msgWnd = CreateWindowExW(0, L"HotrainyMsg", kAppName, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!g_msgWnd) {
        logf("CreateWindow 失败 %lu", (unsigned long)GetLastError());
        return 1;
    }

    // ★ 全界面文字统一"白字黑边"（用户口径）：在创建任何窗口之前设一次。
    //   之后所有 drawText 都忽略调用方给的 RGB、只保留明暗层次，并在外面描一圈黑边 ——
    //   浅色壁纸和深色窗口上都能读清。
    Surface::setGlobalTextStyle(0xC0000000u, true);

    if (!g_bar.create(GetModuleHandleW(nullptr), &g_cfg)) return 1;
    // 永远深色玻璃（用户定调）：不判深浅、不随背景翻色，亮度压制在玻璃管线里做
    g_dark = true;
    // ★ 「文字颜色」设置只作用于**任务栏右侧那些系统文字**（走注入侧的 setTextMode），
    //   不再驱动各面板的主题深浅 —— 面板/岛台/设置/标签栏的文字是固定的"白字黑边"，
    //   跟这个设置无关（用户："其他的别让他随着改了"）。
    const bool panelDark = true;
    g_bar.setDark(panelDark);
    if (!g_panel.create(GetModuleHandleW(nullptr), &g_cfg)) return 1;
    // ★ 这一行原来写的是 setDark(taskbarTextColor == L"light")，会**覆盖**上面
    //   刚设好的 setDark(true) —— 于是「文字颜色」又把面板的深浅带跑了
    //   （用户 taskbarTextColor = "dark" → 面板变浅色 → 雾走浅色那支发白，
    //     "标签面板跟设置/岛台颜色不一样"）。
    //   那个设置的作用范围现在只剩任务栏右侧的系统文字（见上面 1441 行的说明）。
    g_panel.setDark(panelDark);   // 与标签栏 / 岛台 / 设置统一深色玻璃
    g_panel.setFontSize(g_cfg.shortcutFontSize);
    if (!g_search.create(GetModuleHandleW(nullptr), &g_cfg, g_dark)) return 1;
    searchWire();   // 搜索模块的接线（已搬到 src/search/search_wire.cpp）
    tagbarWire();   // 标签栏 + 悬停面板的接线（见 src/tagbar/tagbar_wire.h）
    // ★ 音频条初次使用：按当前屏幕算一个合适大小并写进配置，
    //   之后用户就在这个基础上加减（不再有"0 = 自动"这种夹死减号的状态）。
    // ★ 默认尺寸：横向丝带 —— 宽约 1/3 屏宽，高约其 1/8（用户要求"大概是横屏的
    //   三分之一"）。原来的"屏宽/6 × 屏高-8"是给竖条用的，换成横向丝带后
    //   高度会离谱。尺寸仍可在设置里用「音频条横向/纵向长度」调整。
    // ★ 默认尺寸/位置 = 用户当前在用的那一套（1000x392 @ 783,1175）。
    //   注意：只在这几个值为"未设置"（W/H<=0、X/Y<0）时才套用，
    //   用户自己调过的一律以配置为准 —— 绝不再覆盖用户数据。
    if (g_cfg.audioBarW <= 0) g_cfg.audioBarW = 1000;
    if (g_cfg.audioBarH <= 0) g_cfg.audioBarH = 392;
    if (g_cfg.audioBarX < 0) g_cfg.audioBarX = 783;
    if (g_cfg.audioBarY < 0) g_cfg.audioBarY = 1175;
    saveConfig(g_cfg);
    initAudioBar(GetModuleHandleW(nullptr));
    audioWire();    // 音频条的接线（见 src/audio/audio_wire.cpp）
    setAudioBarGeometry(g_cfg.audioBarW, g_cfg.audioBarH);
    setAudioBarPos(g_cfg.audioBarX, g_cfg.audioBarY);
    // ★ 拖动结束后把位置写回配置 —— 这个 hook 一直有接口却没接上，
    //   所以"下次启动停在上次的位置"从来没生效过（用户明确要求要记住）。
    setAudioBarMovedHook([](int x, int y) {
        g_cfg.audioBarX = x;
        g_cfg.audioBarY = y;
        saveConfig(g_cfg);
    });

    // ── 岛台 ──
    if (!g_island.create(GetModuleHandleW(nullptr), &g_cfg)) return 1;
    g_island.setDark(panelDark);     // 与标签面板同一套文字颜色
    // 文字颜色策略跟随设置里那一项
    g_island.setTextMode(g_cfg.taskbarTextColor);

    // 岛台的搜索：面板里内嵌了从标签栏搜索窗**搬过来**的真 EDIT 输入框，
    // 结果直接在面板里列（用的是同一套引擎 queryHits）。
    // 这里不需要任何接线 —— 见 island.cpp 的 createSearchEdit。

    // 启动分类里的快捷方式（网格点击）。
    // 走和标签面板同一条启动路径，参数/工作目录处理才一致。
    islandWire();   // 岛台的回调（见 src/island/island_wire.cpp）

    // 岛台改了配置（分类切换、加快捷方式）→ 存盘
    islandWire();   // 岛台模块的接线（已搬到 src/island/island_wire.cpp）


    // 点岛台按钮 → 开/关岛台面板。
    //
    // 面板不因鼠标离开而关（用户明确要求），所以这里只管切换，
    // 不做"移开就收"的判断。

    // 设置界面改「岛台」开关时回调这里
    setIslandEnableHook(&applyIslandEnabled);
    setIslandApplyStateHook(&applyTaskbarState);
    // 改「文字颜色」时回调这里（岛台文字跟着任务栏一起变）
    setIslandTextHook(&applyIslandTextMode);

    // 面板自己收起时（150ms 轮询发现鼠标走远了）必须回报给标签栏，
    // 否则 panelOpen_ 一直是 true -> 标签栏永远不收、标签一直亮着。
    // 同时清掉标签栏的悬停态，避免面板没了胶囊还挂在标签上。

    // 面板里右键某个快捷方式 → 同一套菜单，只是带上快捷方式下标

    // 菜单打开期间不许自动收起面板
    setContextMenuOpenCallback([](bool open) {
        g_bar.setMenuOpen(open);
        if (open && g_panel.visible()) g_bar.setPanelOpen(true);
    });
    setDialogDark(g_dark);

    trayCreate(g_msgWnd);
    bool hk = RegisterHotKey(g_msgWnd, kHotkeyHideBar, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H') != FALSE;
    logf("hotkey Ctrl+Alt+H: %s", hk ? "已注册" : "注册失败（可能被占用）");
    registerSearchHotkey();

    // 任务栏透明。
    //
    // 实现见 src/tb/xaml/：注入 explorer，把任务栏背景元素的
    // Opacity 设为 0，背景透出壁纸而图标不受影响。
    //
    // 顺序：
    //   1. ForceReset —— 上次若被强杀，任务栏可能还留着透明，
    //      在应用本次设置之前先复位，保证从干净状态开始
    //   2. SetTransparent —— 写入本次的期望状态
    //   3. Start —— 若期望是开启，此时真正执行注入
    taskbarService().ForceReset();
    taskbarService().SetTextColor(g_cfg.taskbarTextColor);
    taskbarService().Start();

    // ★★ 统一状态机：一个入口，四种状态 ★★
    //
    //   用户的要求："无论开还是关其中一个开关，都会去识别，
    //   而不是只有开的时候"。
    //
    //   applyTaskbarState() 读两个开关的当前配置值，
    //   按四种组合分发到对应的处理函数：
    //     · 透明=开, 岛台=开   → 透明化 + 隐藏按钮
    //     · 透明=关, 岛台=开   → 还原透明化 + 隐藏按钮
    //     · 透明=开, 岛台=关   → 透明化 + 还原按钮
    //     · 透明=关, 岛台=关   → 还原透明化 + 还原按钮
    //
    //   每个状态函数都是幂等的 —— 重复调用无副作用。
    //   ForceReset 已经清理了上一次残留，这里直接设到目标状态。
    //
    //   ⚠ 替代了原来分散的 SetTransparent + applyIslandEnabled。
    //     原来那套的问题是：各自只管"开"的情况，
    //     "关"的情况靠 ForceReset 兜底 —— 但 ForceReset 只在启动时跑一次，
    //     运行时切换就漏了。
    applyTaskbarState();
    logf("taskbar: 已启动，透明=%d 岛台=%d",
         (int)g_cfg.taskbarTransparent, (int)g_cfg.islandEnabled);

    SetTimer(g_msgWnd, kAutoHideTimer, 40, nullptr);
    // ⚠ 40ms 而不是原来的 120ms。
    //
    //   这个定时器只做一件事：g_bar.autoHideTick() ——
    //   一次 GetCursorPos 加上必要时显隐，代价可以忽略。
    //
    //   而 120ms 的采样间隔意味着：鼠标移到顶边后，最坏要等 120ms
    //   标签栏才出来，离开后同样要等 120ms 才收 ——
    //   用户的感觉就是"移动到上面卡一下""收回慢"。
    //   40ms 是肉眼跟手的门槛。
    SetTimer(g_msgWnd, kBackdropTimer, 700, nullptr);
    SetTimer(g_msgWnd, kPanelPollTimer, 50, nullptr);
    SetTimer(g_msgWnd, kFileDialogTimer, 600, nullptr);
    SetTimer(g_msgWnd, kClickOutTimer, 30, nullptr);   // 点面板外收回（30ms 更跟手）  // main.py:960 after(600, _watch_file_dialog)

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    logf("退出");
    // 先停任务栏引擎：它会在退出前把任务栏恢复成系统默认，
    // 不能给用户留一个半透明的残骸
    //
    // ⚠ 岛台接管也要还原 —— 不还原的话退出后任务栏会缺「开始」和「搜索」
    //   两个入口，用户只能重启 explorer。这个顺序无所谓，
    //   只要在 shutdown 之前做完即可（shutdown 会卸钩子）。
    if (g_cfg.islandEnabled) {
        tb::xaml::setIslandTakeover(false);
        logf("island: 任务栏接管已撤销");
    }
    g_island.destroy();
    taskbarService().Stop();
    g_settings.close();
    if (hk) UnregisterHotKey(g_msgWnd, kHotkeyHideBar);
    if (searchHotkeyOk()) UnregisterHotKey(g_msgWnd, kHotkeySearch);
    g_search.destroy();
    destroyAudioBar();
    g_panel.destroy();
    g_bar.destroy();
    trayDestroy();
    saveConfig(g_cfg);
    Gdiplus::GdiplusShutdown(g_gdiplus);
    if (SUCCEEDED(hrCo)) CoUninitialize();

    // 释放单实例互斥体 —— 不释放的话下次启动会被误判成"已有实例"。
    if (single) {
        ::ReleaseMutex(single);
        ::CloseHandle(single);
    }

    logf("===== 已退出 =====");
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  应用上下文访问器（见 include/hr/app/app_context.h）
//
//  这些对象原本定义在上面的匿名命名空间里（内部链接），各模块的接线文件拿不到。
//  这里只提供"返回引用"的薄包装，不改变任何行为；新模块在此加一行即可。
// ═══════════════════════════════════════════════════════════════════════════
namespace hr {
Config&         cfg()        { return g_cfg; }
BarWindow&      bar()        { return g_bar; }
HoverPanel&     panel()      { return g_panel; }
IslandWindow&   island()     { return g_island; }
IslandButton&   islandBtn()  { return g_islandBtn; }
SearchWindow&   search()     { return g_search; }
SettingsWindow& settings()   { return g_settings; }
HWND            msgWnd()     { return g_msgWnd; }
HWND&           dialogDismissed() { return g_dialogDismissed; }
DWORD&          iconClickTick()   { return g_iconClickTick; }
DWORD&          pendingCloseTick() { return g_pendingCloseTick; }
void            toggleIslandPanelWire(RECT btn) { ::toggleIslandPanel(btn); }
bool            applySettingKey(const std::string& key) { return ::applySetting(key); }
ContextMenuHooks makeHooksWire() { return hr::makeHooks(); }
bool& dark() { return ::g_dark; }
bool& searchHotkeyOk() { return ::g_searchHotkeyOk; }
void hidePanelWire() { ::hidePanel(); }
void showPanelForTabWire(int idx, RECT bounds, bool force) { ::showPanelForTab(idx, bounds, force); }
}  // namespace hr
