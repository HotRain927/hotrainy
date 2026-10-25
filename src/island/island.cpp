// hr/island.cpp — 岛台面板实现
//
// 设计要点见 island.h。这里补充几个实现上的关键决定：
//
// 【为什么是独立窗口而不是嵌在标签栏里】
//   面板要能拖边框改大小、要能持有键盘焦点（搜索框输入），
//   而且不因鼠标离开而关。标签栏是个 24px 高的条，塞不下这些。
//
// 【为什么不用 WS_EX_NOACTIVATE】
//   标签栏和悬停面板都用 NOACTIVATE（不抢焦点），但岛台有搜索框，
//   必须能接收键盘输入和文本光标。所以这里是普通可激活窗口。
//   代价：点它会抢走前台焦点，这是搜索功能必须付的。
//
// 【玻璃渲染】
//   复用 Surface::buildPanel —— 和标签面板同一套：
//   抓屏 → 1/4 缩放模糊 → 竖向色调渐变 → 圆角 → 描边。
//   所以视觉上和标签面板完全一致。
#include "hr/island/island.h"

#include "hr/shared/common.h"
#include "hr/shared/icons.h"
#include "hr/shared/menu.h"
#include "hr/search/search.h"      // queryHits / SearchHit（岛台内嵌搜索框复用同一套引擎）
#include "hr/shared/shell.h"
#include "hr/text/theme.h"

#include <shellapi.h>       // ShellExecuteW
#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM
#include <objbase.h>        // CoInitialize 相关
#include <dwmapi.h>         // DwmFlush（重抓背景前等 DWM 合成一帧）
#include <imm.h>            // ImmSetCompositionWindow（把输入法组合窗口挪开）

#pragma comment(lib, "shell32.lib")

#include <algorithm>
#include <cstdio>

namespace hr {

namespace {

const wchar_t* kIslandClass = L"HotrainyIsland";

// ★ 外阴影留白（逻辑像素）。IslandWindow::shadowInset() 和 contentPt() 共用这一个值 ——
//   两处必须一致，否则鼠标坐标会整体偏掉。
//   窗口 = 内容 + 四周这么多留白；内容坐标不变，只有跟系统打交道的地方要减掉它。
constexpr int kIslandShadowLogical = 12;
inline int islandShadowPx() {
    return px(kIslandShadowLogical);
}

// 面板尺寸下限 —— 再小连搜索框和分类栏都排不下
const int kMinW = 420;
const int kMinH = 320;

// 拖边框的感应厚度
// 拖边框的感应厚度。
//
// ⚠ 从 6 提到 10：
//   6 物理像素在 150% DPI 下只有 4 逻辑像素，鼠标很难精准压上去。
//   实测用户反馈"调大小的方式不好"就是这个原因 —— 抓不住边框。
const int kResizeBorder = 10;

// 尺寸/位置落盘的延迟。
//
// 拖动过程中 WM_SIZE 会连续触发，每次都写配置会把磁盘刷爆。
// 用一个一次性定时器，等尺寸稳定下来再存。
const UINT_PTR kSaveTimerId = 0x15L;

// 长按多久算"拖动"而不是"单击"。
//
// 400ms 是权衡：太短会把单击误判成拖动，
// 太长用户会觉得"怎么拖不动"。
// 另外还允许"按住移动超过 6px"提前触发，见 WM_MOUSEMOVE。
const DWORD kDragHoldMs = 400;

// 长按检测的定时器。
//
// ⚠ 为什么必须用定时器而不是在 WM_MOUSEMOVE 里判断：
//   用户按住不动时，系统**不发鼠标移动消息** ——
//   只在移动里判断的话，静止按住永远不会进拖动状态。
//   实测就是这么"没反应"的。
//
// 200ms 检查一次：比 400ms 的门槛细一倍，用户感觉不到延迟。
const UINT_PTR kDragTimerId = 0x71;

// 让位动画的时长（毫秒）。
//
// 手机桌面拖动时，被挤开的图标大约用 200ms 滑到位 ——
// 太短（<120ms）显得生硬像瞬移，太长（>300ms）会觉得拖泥带水、
// 跟手性变差。200ms 配 easeOutCubic 是接近 iOS/Android 主屏的手感。
const int kDragAnimTauMs = 65;

// 展开动画用的定时器（15ms ≈ 60fps）与时长
const UINT_PTR kGrowTimerId = 0x73;
const int kGrowMs = 190;   // 让位缓动的指数时间常数（约 3τ 收敛，够快又不生硬）

// 分类「全部」的保留 id
const char* kAllCategory = "__all__";

}  // namespace

// ── 创建 / 销毁 ─────────────────────────────────────────────────────

bool IslandWindow::create(HINSTANCE hInst, Config* cfg) {
    hinst_ = hInst;
    cfg_ = cfg;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &IslandWindow::proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kIslandClass;
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        // 1410 = 类已存在。另一个实例先注册了，不算失败。
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            logf("island: RegisterClass 失败 %lu", (unsigned long)err);
            return false;
        }
    }

    // ⚠ 这里**不用** WS_EX_NOACTIVATE —— 搜索框需要键盘焦点。
    //   WS_EX_TOOLWINDOW 让它不出现在 Alt+Tab 里。
    //
    // ⚠⚠ 必须带 WS_THICKFRAME。
    //
    //   这是"窗口可拖边框改大小"的标志。没有它的话，
    //   即使 WM_NCHITTEST 正确返回了 HTBOTTOMRIGHT，
    //   系统也会直接忽略 —— 根本不进拖动循环，
    //   WM_SIZING / WM_EXITSIZEMOVE 一个都不会来。
    //
    //   （实测：不加时拖右下角完全没反应，尺寸一点不变。）
    //
    //   加了之后系统会画一圈非客户区边框，但分层窗口
    //   （WS_EX_LAYERED + UpdateLayeredWindow）下那圈边框不渲染，
    //   所以外观不受影响。
    //
    // \u26a0 \u8981 WS_EX_TOPMOST \u2014\u2014 \u7528\u6237\u660e\u786e\u8981\u6c42\u201c\u70b9\u5f00\u9762\u677f\u4e00\u5b9a\u8981\u663e\u793a\u5728\u6700\u4e0a\u65b9\u201d\u3002
    //
    //   \u6211\u4e00\u5ea6\u628a\u5b83\u53bb\u6389\u4e86\uff0c\u56e0\u4e3a\u7528\u6237\u62b1\u6028\u201c\u70b9\u522b\u7684\u8f6f\u4ef6\u65f6\u5b83\u8fd8\u538b\u5728\u6700\u4e0a\u5c42\u201d\u3002
    //   \u4f46\u90a3\u53e5\u8bdd\u7684\u91cd\u70b9\u662f**\u8981\u7f6e\u9876**\uff0c\u800c\u201c\u6321\u7740\u522b\u4eba\u201d\u8be5\u9760
    //   \u201c\u70b9\u9762\u677f\u5916\u90e8\u81ea\u52a8\u6536\u56de\u201d\u89e3\u51b3 \u2014\u2014 \u4e0d\u662f\u9760\u53d6\u6d88\u7f6e\u9876\u3002
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                            kIslandClass, L"hotrainy-island",
                            WS_POPUP | WS_THICKFRAME,
                            0, 0, 1, 1, nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        logf("island: CreateWindowEx 失败 %lu", (unsigned long)GetLastError());
        return false;
    }

    w_ = cfg_->islandW;
    h_ = cfg_->islandH;
    if (w_ < kMinW) w_ = kMinW;
    if (h_ < kMinH) h_ = kMinH;

    buildCategories();
    layout();
    createSearchEdit(hInst);     // 面板内嵌的真搜索框（见该函数的说明）
    logf("island: 已创建 %dx%d", w_, h_);
    return true;
}

void IslandWindow::destroy() {
    // 输入框是 hwnd_ 的 owned 窗口，DestroyWindow(hwnd_) 会连带销毁它；
    // 但字体句柄是我们自己建的，要自己删（不然又是个 HFONT 泄漏）。
    if (searchEdit_) {
        searchEditOldProc_ = nullptr;
        ::DestroyWindow(searchEdit_);
        searchEdit_ = nullptr;
    }
    if (searchFont_) {
        ::DeleteObject(searchFont_);
        searchFont_ = nullptr;
    }
    if (searchBgBrush_) {
        ::DeleteObject(searchBgBrush_);
        searchBgBrush_ = nullptr;
    }
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

// ── 显示 / 隐藏 ─────────────────────────────────────────────────────

void IslandWindow::show(int anchorX, int anchorY) {
    // ★ 打开时把拖动状态清干净（见长按定时器里的说明：残留的 dragArmed_
    //   会让面板一打开就"自动"抓起某个快捷方式）。
    dragArmed_ = false;
    dragCell_ = -1;
    dragTarget_ = -1;
    pressedCell_ = -1;
    // 展开动画：如果正播着，取消掉（别让它继续改窗口位置）
    growActive_ = false;
    if (hwnd_) {
        ::KillTimer(hwnd_, kDragTimerId);
        ::KillTimer(hwnd_, kDragAnimTimerId);
        ::KillTimer(hwnd_, kGrowTimerId);
    }
    if (!hwnd_ || !cfg_) return;

    w_ = cfg_->islandW;
    h_ = cfg_->islandH;
    if (w_ < kMinW) w_ = kMinW;
    if (h_ < kMinH) h_ = kMinH;

    // 位置。
    //
    // 优先用记住的位置（用户拖过之后存下来的）。
    // 没拖过就贴着锚点展开 —— 锚点通常是任务栏上的岛台按钮。
    if (cfg_->islandX >= 0 && cfg_->islandY >= 0) {
        x_ = cfg_->islandX;
        y_ = cfg_->islandY;
    } else {
        // 锚点是任务栏上的岛台按钮，一般在任务栏最左端。
        //
        // 面板要"从按钮上长出来"：
        //   · 水平：左边缘和按钮对齐
        //   · 垂直：按任务栏在屏幕上还是下决定往上弹还是往下弹
        //
        // ⚠ 锚点 y 传的是**按钮底边**（任务栏坐标），
        //   直接用会跑到屏幕外，所以要先判断任务栏在哪半屏。
        x_ = anchorX;

        RECT work{};
        if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            const int screenMid = (work.top + work.bottom) / 2;
            if (anchorY > screenMid) {
                // 任务栏在屏幕下半部（常见：底部任务栏）→ 面板往上展开，
                // 底边贴着任务栏顶边
                y_ = work.bottom - h_ - px(8);
            } else {
                // 任务栏在顶部 → 往下展开
                y_ = work.top + px(8);
            }
        } else {
            y_ = anchorY;
        }
    }

    // 整体夹进工作区（不然拖到屏幕外就找不回来了）
    RECT work{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        if (x_ + w_ > work.right) x_ = work.right - w_;
        if (y_ + h_ > work.bottom) y_ = work.bottom - h_;
        if (x_ < work.left) x_ = work.left;
        if (y_ < work.top) y_ = work.top;
    }

    // ★ 「从图标位置长出来」：先记下最终窗口矩形，再把起始矩形放在图标那一带。
    //   动画本身在 redraw 的提交处做（那里才知道 ULW 的 dst/size 该怎么插值）。
    {
        const int s0 = shadowInset();
        growTo_ = {x_ - s0, y_ - s0, x_ + w_ + s0, y_ + h_ + s0};
        const int gw = std::max(px(72), (int)((growTo_.right - growTo_.left) * 2 / 5));
        const int gh = std::max(px(72), (int)((growTo_.bottom - growTo_.top) * 2 / 5));
        growFrom_.left = anchorX - gw / 2;
        growFrom_.top = anchorY - gh / 2;
        growFrom_.right = growFrom_.left + gw;
        growFrom_.bottom = growFrom_.top + gh;
        growActive_ = true;
        growStart_ = ::GetTickCount();
        if (!hwnd_) {
            growActive_ = false;
        } else {
            ::SetTimer(hwnd_, kGrowTimerId, 15, nullptr);
        }
    }

    buildCategories();
    layout();

    // 算文字深浅 —— 必须在抓背景之前定下来，
    // 因为采样位置和面板位置有关。
    resolveTextColors();

    // 抓一块干净背景再烘焙玻璃。
    // 抓之前先把窗口藏起来，否则会抓到自己（上一次显示留下的像素）。
    ShowWindow(hwnd_, SW_HIDE);
    RECT rc = {x_, y_, x_ + w_, y_ + h_};
    Backdrop bd;
    if (Surface::capture(rc, 0, bd)) {
        backdrop_ = bd;
        baseValid_ = false;
    }

    redraw();

    // ⚠ 打开时抬到最前（一次），但**不激活**。
    //
    //   踩过的坑：
    //     1. 窗口原来带 WS_EX_TOPMOST，用户点别的软件时
    //        面板还压在上面挡着 —— 已经去掉那个样式。
    //     2. 原来是 SetForegroundWindow，会把前台窗口抢过来。
    //        用户正在别的程序里打字，点一下岛台按钮焦点就飞了。
    //        面板是"菜单式"的东西，不该抢焦点
    //        （内部搜索框需要焦点时，用户会主动点进去）。
    //
    //   SWP_NOACTIVATE 就是"抬到最前但别激活我"。
    ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
    layoutSearchEdit();
    showSearchEdit(true);
    logf("island: 显示 @ (%d,%d) %dx%d", x_, y_, w_, h_);
}

void IslandWindow::hide() {
    // ★ 关闭时同样清干净：否则下次打开会带着上一次的 armed 状态。
    dragArmed_ = false;
    dragCell_ = -1;
    dragTarget_ = -1;
    pressedCell_ = -1;
    if (hwnd_) {
        ::KillTimer(hwnd_, kDragTimerId);
        ::KillTimer(hwnd_, kDragAnimTimerId);
    }
    if (!hwnd_) return;

    // ★ 面板收起时，把正在打开的菜单（电源/右键）一起关掉。
    //
    //   菜单是**模态**的（showGlassMenu 阻塞在它自己的消息循环里），
    //   面板收起不会自动带走它 —— 屏幕上会留一个没有归属的小窗口。
    //   用户的要求就是"打开电源时出现的那个小窗口要跟面板一起关闭"。
    requestCloseGlassMenu();

    // ⚠ 隐藏前把当前尺寸/位置存下来。
    //
    //   拖动结束时的保存靠 400ms 定时器，如果用户拖完立刻按 × 或点别处，
    //   定时器还没到就被隐藏了 —— 尺寸就丢了。
    //   这里兜一道，保证"拖完就关"也不会丢。
    //
    // ★★ 但**只有显示过的面板**才保存。★★
    //
    //   踩过的坑：面板窗口在 create() 时是一个占位矩形
    //   （kMinW x kMinH = 420x320 @ 0,0），此时根本谈不上用户尺寸。
    //   而启动流程里会走一次 hide()（前置重置/关闭岛台）——
    //   于是那个占位矩形被当成"用户尺寸"写进了配置，
    //   把用户真正调过的 1305x791 覆盖成了 420x320 @ (0,0)。
    //   后果：下次打开面板又小又挤到左上角。
    //
    //   visible_ 只在 show() 到 hide() 之间为真，用它当门槛正好。
    if (visible_) {
        RECT wr{};
        if (::GetWindowRect(hwnd_, &wr)) {
            // ★ 窗口矩形 = 内容 + 四周外阴影留白 → 存进配置的必须是**内容**几何，
            //   否则每次隐藏都按"窗口尺寸"记一遍，下次打开就长大一圈。
            const int shd = islandShadowPx();
            const int nw = (wr.right - wr.left) - shd * 2;
            const int nh = (wr.bottom - wr.top) - shd * 2;
            const int nxp = wr.left + shd, nyp = wr.top + shd;
            if (nw > 0 && nh > 0 && (nw != w_ || nh != h_ || nxp != x_ || nyp != y_)) {
                w_ = nw;
                h_ = nh;
                x_ = nxp;
                y_ = nyp;
                if (cfg_) {
                    cfg_->islandW = w_;
                    cfg_->islandH = h_;
                    cfg_->islandX = x_;
                    cfg_->islandY = y_;
                }
                if (onConfigDirty) onConfigDirty();
                logf("island: 隐藏前保存尺寸 %dx%d @ (%d,%d)", w_, h_, x_, y_);
            }
        }
    }

    // ⚠ 顺序要紧：先把 visible_ 置 false，再隐藏窗口。
    //
    //   踩过的坑：原来是先 ShowWindow(SW_HIDE) 再 visible_ = false。
    //   而隐藏窗口会触发 WM_WINDOWPOSCHANGED → 那里会调 layoutSearchEdit()，
    //   它看到 visible_ 还是 true，就用 SWP_SHOWWINDOW 把**输入框又显示回来** ——
    //   表现就是"面板收回了，那条背景（输入框）还留在屏幕上"。
    showSearchEdit(false);      // 收起输入框并清空（下次打开是干净的）
    visible_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    logf("island: 隐藏");
}

void IslandWindow::toggle(int anchorX, int anchorY) {
    if (visible_) hide();
    else show(anchorX, anchorY);
}

void IslandWindow::setDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    baseValid_ = false;
    if (visible_) redraw();
}

void IslandWindow::notifyConfigChanged() {
    buildCategories();
    layout();
    if (visible_) {
        baseValid_ = false;
        redraw();
    }
}

// 外观参数（玻璃开关 / 壁纸 / 任务栏透明等）变了 → 重抓背景、重烘焙底色。
//
// ⚠ 为什么必须单独开一个入口：
//   设置里改「玻璃外观」时，主程序会通知标签栏和标签面板
//   （g_bar.onConfigChanged / g_panel.onConfigChanged），
//   但**从来没有通知岛台面板** —— 于是标签面板立刻就变了，
//   岛台还是旧样子。用户的原话就是"岛台面板的透明不实时更新"。
void IslandWindow::refreshAppearance() {
    if (!hwnd_ || !cfg_) return;

    // ★ 照设置面板的做法：**不重抓背景，只重烘焙**。
    //
    //   背景只在打开时抓一次（见 show()），
    //   之后改玻璃/模式/主题都只重新烘焙已有底图：
    //   不藏窗口、不重抓，所以既不会“像被重新打开”也不会把透明弄丢。
    baseValid_ = false;
    if (visible_) {
        redraw();
        layoutSearchEdit();     // 外观/尺寸变了，内嵌输入框跟着重摆
    }
    logf("island: 外观参数已刷新（重抓背景）");
}

// 只重抓背景（外观参数不变）—— 和标签面板 HoverPanel::refreshBackdropLive 同一套做法。
//
// ⚠ 必须先把窗口藏起来再抓：
//     面板自己就在要抓的区域里，不藏就会把"自己那张玻璃脸"拍进去。
//   ⚠ 藏之前 DWM 要先合成一帧（DwmFlush），否则抓到的还是上一帧带自己的画面。
//   ⚠ 输入框是独立顶层窗口，它由 WM_SHOWWINDOW 跟着一起藏/显
//     （见那条消息的说明）—— 所以抓到的背景里不会有它。
void IslandWindow::refreshBackdropLive() {
    if (!hwnd_ || !visible_) return;

    RECT rc = {x_, y_, x_ + w_, y_ + h_};
    // ★ 这里**绝对不能** ShowWindow(SW_HIDE) 再抓。
    //
    //   那一藏一显就是用户报的"拖动放下的瞬间面板被关掉又打开一下"；
    //   而且 backdrop_ 现在**没有任何渲染代码读它**（面板不再抓背景做玻璃），
    //   这趟搬运纯属多余。
    //
    //   不带 CAPTUREBLT 时分层窗口会被排除 —— 直接抓就是面板背后的画面，
    //   不需要躲开自己。
    Backdrop bd;
    const bool ok = Surface::capture(rc, 0, bd, /*includeLayered=*/false);

    // 背景没变就不重建 —— 否则每 150ms 白重建一次（标签面板也这么做）
    if (ok && bd.sig != backdrop_.sig) {
        backdrop_ = bd;
        baseValid_ = false;
        redraw();
        logf("island: 背景变了 -> 重抓并重烘焙");
    }
}

// 拖动/缩放过程中的实时重抓。
//
// ⚠ 和 refreshBackdropLive 的区别：
//   那个是"藏起来→抓→显示"（适合非拖动场景）；
//   拖动时窗口正被系统拖着，藏起来会把拖拽搞乱，
//   所以改成"一边显示一边抓"——靠 WM_ENTERSIZEMOVE 打的
//   WDA_EXCLUDEFROMCAPTURE 把面板和输入框排除在抓屏之外。
void IslandWindow::refreshBackdropWhileDragging() {
    if (!hwnd_ || !visible_) return;

    RECT wr{};
    if (!::GetWindowRect(hwnd_, &wr)) return;

    // 只抓**面板当前这一块**，而且**不带 CAPTUREBLT**：
    //   不带它时分层窗口会被排除，而面板和输入框都是分层窗口 ——
    //   所以抓到的是"面板背后"的真实背景，抓完直接当底重烘焙。
    //
    // ⚠ 千万别在这里藏窗口（会中止拖动），也别抓整屏（太重会堵住拖动）。
    // ★ 抓的是**内容**那一块（窗口多出的一圈留白不该算进来，
    //   否则玻璃底的取样范围会偏出去一圈）。
    const int shd = islandShadowPx();
    RECT rc = {wr.left + shd, wr.top + shd, wr.right - shd, wr.bottom - shd};
    Backdrop bd;
    if (Surface::capture(rc, 0, bd, /*includeLayered=*/false)) {
        backdrop_ = bd;
        baseValid_ = false;
    }

    // 记账：拖动中位置一直在变，位置要跟上（窗口矩形 → 内容位置）
    x_ = wr.left + shd;
    y_ = wr.top + shd;
    redraw();
    layoutSearchEdit();     // 输入框也要跟着面板走
}

// 周期性检查（由主程序 150ms 的定时器调用）：
// 前台窗口换了就重抓背景 —— 这就是"实时更新"。
void IslandWindow::pollTick() {
    if (!visible_ || !cfg_) return;

    // 只有"玻璃外观"才需要跟着前台重抓：
    //   壁纸底是固定的图，重抓没意义；实色底更没意义。
    if (!glassAppearanceOn(*cfg_)) return;

    // ★ 先关掉：这条路径靠“藏窗口→抓屏→显示”重抓背景，
    //   拖动后前台会变，于是触发一次 ——
    //   用户看到的就是“窗口重新出现的感觉”。
    //   在把“不藏窗口也能重抓”做出来之前，先不做这件事。
    return;

    HWND fg = ::GetForegroundWindow();
    if (!fg || fg == fgHwnd_) return;
    fgHwnd_ = fg;

    // 前台变成自己（点面板、点内嵌输入框都会）——不用重抓
    if (fg == hwnd_ || fg == searchEdit_) return;

    refreshBackdropLive();
}

void IslandWindow::reloadIcons() {
    if (visible_) redraw();
}

void IslandWindow::setDarkText(bool dark) {
    // ⚠ 面板文字永远白色，所以这个接口**不再影响面板**。
    //
    //   留着是为了接口稳定（主程序还在调），但实际什么都不做。
    //   真正跟随设置的是任务栏上的岛台按钮 —— 那是另一个类。
    (void)dark;
}

void IslandWindow::setTextMode(const std::wstring& mode) {
    if (mode != L"dark" && mode != L"light") return;   // 「自动」已删除
    if (textMode_ == mode) return;
    textMode_ = mode;
    if (visible_) {
        resolveTextColors();
        redraw();
    }
}

// 采样面板背后那块壁纸的平均亮度。
//
// 和任务栏那边（visualtree.cpp 的 shouldUseDarkText）用同一套算法：
// 感知亮度 = 0.2126R + 0.7152G + 0.0722B，阈值 140。
//
// 这样岛台和任务栏的文字深浅判断一致，
// 不会出现"任务栏用黑字、岛台用白字"的割裂。
double IslandWindow::sampleBackdropLuminance() {
    if (w_ <= 0 || h_ <= 0) return 128.0;

    // 采样面板正下方那条壁纸（避开面板自己）
    const int sampleW = std::min(w_ - 40, 500);
    const int sampleH = 40;
    if (sampleW <= 0) return 128.0;

    // 面板在屏幕上时，采它下边缘外面一点
    int sx = x_ + 20;
    int sy = y_ + h_ + 10;

    // 如果面板已经贴着屏幕底部，就采它上面
    RECT work{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        if (sy + sampleH > work.bottom) sy = y_ - sampleH - 10;
        if (sy < work.top) sy = work.top + 10;
        if (sx + sampleW > work.right) sx = work.right - sampleW - 10;
        if (sx < work.left) sx = work.left + 10;
    }

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, sampleW, sampleH);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, sampleW, sampleH, screen, sx, sy, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sampleW;
    bi.bmiHeader.biHeight = -sampleH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> px((size_t)sampleW * sampleH * 4);
    ::GetDIBits(mem, bmp, 0, sampleH, px.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    double sum = 0;
    const int n = sampleW * sampleH;
    for (int i = 0; i < n; ++i) {
        const double b = px[(size_t)i * 4 + 0];
        const double g = px[(size_t)i * 4 + 1];
        const double r = px[(size_t)i * 4 + 2];
        sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
    }
    return n ? sum / n : 128.0;
}

void IslandWindow::resolveTextColors() {
    // ⚠ 岛台面板的文字**永远是白色**，不跟随任何设置。
    //
    //   这里改过三次，记一下最终结论，避免以后又改回去：
    //
    //   1. "跟随壁纸亮度自动切深浅" —— 错。
    //      面板是一块深色玻璃，跟着背后的壁纸切会出现
    //      "深色玻璃上写黑字"，根本看不清。
    //
    //   2. "跟随设置里的文字颜色" —— 也错。
    //      那个设置是给**任务栏托盘文字**用的。
    //      托盘贴在壁纸上（壁纸可能是浅色），所以要能切深色；
    //      但面板自己就是深色玻璃，切深色没有意义。
    //
    //   3. 固定白色 —— 对。面板底色永远是深色玻璃，
    //      白字在它上面恒定可读，不受壁纸影响。
    //
    //   用户明确要求过这一点。
    // 文字颜色跟随全局设置（不再是硬编码白字）：见 handle() 里按 textMode_ 取色
    useDarkText_ = false;

    textMain_ = kIslandTextMainLight;
    textDim_  = kIslandTextDimLight;

    logf("island: 面板文字用白色（固定，不跟随设置）");
}

// ── 分类 ────────────────────────────────────────────────────────────

void IslandWindow::buildCategories() {
    categories_.clear();
    if (!cfg_) return;

    // 「全部」放最前 —— 这是用户自己右键加的快捷方式
    Category all;
    all.id = kAllCategory;
    all.name = L"全部";
    all.isAll = true;
    all.shortcutCount = (int)cfg_->islandShortcuts.size();
    categories_.push_back(all);

    // 其余分类 = 每个标签一项，内容是该标签已有的快捷方式
    for (const Tab& t : cfg_->tabs) {
        Category c;
        c.id = t.id;
        c.name = t.name;
        c.isAll = false;
        c.shortcutCount = (int)t.shortcuts.size();
        categories_.push_back(c);
    }

    // 配置里记的分类如果已经不存在了（标签被删），回落到「全部」
    bool found = false;
    for (const Category& c : categories_) {
        if (c.id == cfg_->islandCategory) { found = true; break; }
    }
    if (!found) cfg_->islandCategory = kAllCategory;
}

const std::vector<Shortcut>* IslandWindow::currentShortcuts() const {
    if (!cfg_) return nullptr;
    if (cfg_->islandCategory == kAllCategory) return &cfg_->islandShortcuts;
    for (const Tab& t : cfg_->tabs) {
        if (t.id == cfg_->islandCategory) return &t.shortcuts;
    }
    return &cfg_->islandShortcuts;
}

std::vector<Shortcut>* IslandWindow::mutableCurrentShortcuts() {
    if (!cfg_) return nullptr;
    if (cfg_->islandCategory == kAllCategory) return &cfg_->islandShortcuts;
    for (Tab& t : cfg_->tabs) {
        if (t.id == cfg_->islandCategory) return &t.shortcuts;
    }
    return &cfg_->islandShortcuts;
}

void IslandWindow::moveShortcut(int from, int to) {
    auto* items = mutableCurrentShortcuts();
    if (!items) return;

    const int n = (int)items->size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    // 取出这一项，插到新位置。
    //
    // ⚠ 用"取出再插入"而不是 std::swap。
    //   互换的话，把第 1 个拖到第 5 个，结果第 5 个跑到第 1 个的位置 ——
    //   而用户预期是"其他的顺次往前挪一格"。
    Shortcut s = (*items)[from];
    items->erase(items->begin() + from);
    items->insert(items->begin() + to, s);

    logf("island: 快捷方式换位 %d -> %d（共 %d 项）", from, to, n);

    if (onConfigDirty) onConfigDirty();
    redraw();
}

int IslandWindow::categoryAt(int x, int y) const {
    if (x < 0 || x >= sideW_) return -1;
    const int itemH = px(34);
    const int top = searchH_ + px(8);
    if (y < top) return -1;
    int idx = (y - top) / itemH;
    if (idx < 0 || idx >= (int)categories_.size()) return -1;
    return idx;
}

// ── 布局 ────────────────────────────────────────────────────────────

void IslandWindow::layout() {
    searchH_ = px(52);          // 顶部搜索条
    sideW_ = px(132);           // 左侧分类栏
    iconSize_ = px(48);         // 大图标
    cellW_ = px(96);
    cellH_ = px(96);

    const int bottomBar = px(48);   // 底部用户/电源条

    gridX_ = sideW_;
    gridY_ = searchH_;
    gridW_ = w_ - sideW_;
    gridH_ = h_ - searchH_ - bottomBar;
    if (gridW_ < cellW_) gridW_ = cellW_;
    if (gridH_ < cellH_) gridH_ = cellH_;

    cols_ = std::max(1, gridW_ / cellW_);
    rows_ = std::max(1, gridH_ / cellH_);

    // 搜索结果列表的区域 = 搜索框下面、底部按钮上面的整块。
    // 搜索态时分类栏和网格都不画，整块让给结果列表。
    resultRowH_ = px(46);
    resultArea_.left = px(12);
    resultArea_.top = searchH_ + px(6);
    resultArea_.right = w_ - px(12);
    // ⚠ 下沿要再抬高 px(24)：底部那行操作提示（↑↓ 选择 Enter 打开…）
    //   原来和最后一条结果叠在一起，看着很脏。
    resultArea_.bottom = h_ - bottomBar - px(24);
    if (resultArea_.bottom <= resultArea_.top) resultArea_.bottom = resultArea_.top + resultRowH_;

    // ⚠ 顶部不再有"搜索框矩形"这种成员了 ——
    //   搜索框整个就是从标签栏搜索窗搬过来的那个 EDIT 窗口，
    //   位置由 layoutSearchEdit() 按 w_/searchH_ 直接算，
    //   面板自己不画、也不记这块区域（用户要求"之前的完全不要"）。

    // 底部两个按钮：左下用户，右下电源
    const int btnW = px(120);
    const int btnH = px(32);
    const int pad = px(12);
    userBtn_.left = pad;
    userBtn_.top = h_ - btnH - pad;
    userBtn_.right = pad + btnW;
    userBtn_.bottom = h_ - pad;

    powerBtn_.right = w_ - pad;
    powerBtn_.top = h_ - btnH - pad;
    powerBtn_.left = w_ - pad - btnW;
    powerBtn_.bottom = h_ - pad;
}

void IslandWindow::computeGrid() {
    const std::vector<Shortcut>* items = currentShortcuts();
    const int total = items ? (int)items->size() : 0;
    const int perPage = cols_ * rows_;
    maxScrollRow_ = 0;
    if (perPage > 0 && total > perPage) {
        maxScrollRow_ = (total + perPage - 1) / perPage - 1;
    }
    if (scrollRow_ > maxScrollRow_) scrollRow_ = maxScrollRow_;
    if (scrollRow_ < 0) scrollRow_ = 0;
}

int IslandWindow::cellAt(int x, int y) const {
    if (x < gridX_ || y < gridY_ || x >= gridX_ + gridW_ || y >= gridY_ + gridH_) return -1;
    const int localX = x - gridX_;
    const int localY = y - gridY_;
    const int col = localX / cellW_;
    const int row = localY / cellH_;
    if (col < 0 || col >= cols_ || row < 0 || row >= rows_) return -1;

    const int perPage = cols_ * rows_;
    const int idx = scrollRow_ * perPage + row * cols_ + col;

    const std::vector<Shortcut>* items = currentShortcuts();
    if (!items || idx < 0 || idx >= (int)items->size()) return -1;
    return idx;
}

// ── 绘制 ────────────────────────────────────────────────────────────

void IslandWindow::rebuildBase() {
    if (w_ <= 0 || h_ <= 0) return;
    base_.create(w_, h_);
    const int radius = px(14);
    // ★ 全局面板透明度：与标签面板共用同一个百分比（见 glass.cpp buildFlatPanel）。
    const int op = cfg_ ? cfg_->panelOpacity : 0;
    base_.buildFlatPanel(op, radius, dark_);   // 0 = 纯透明，边框始终保留
    base_.cutRoundCorners(radius);

    // ★ 外阴影：与悬停面板同一套 —— 画布比内容大出四周留白，
    //   影子只画在内容矩形之外，**内容坐标一像素不动**。
    const int s = shadowInset();
    shadow_.create(w_ + s * 2, h_ + s * 2);
    shadow_.clear();
    RECT content = {s, s, s + w_, s + h_};
    shadow_.applyOuterShadow(content, radius, dark_, px(2));

    baseValid_ = true;
}

int IslandWindow::shadowInset() const {
    return islandShadowPx();
}

// ★ 让位动画：每帧把每格"当前显示位置"朝它该去的位置推进一步。
//
//   指数缓动 + 时间驱动（dt 用 GetTickCount 算）：不管 WM_TIMER 稳不稳、
//   掉不掉帧，收敛速度都一样，所以看起来是顺滑的滑动而不是"一顿一顿"。
//   目标格中途变化也**不需要重置** —— 换个方向继续滑就是了。
void IslandWindow::stepCellAnim() {
    if (dragCell_ < 0 || dragPageStart_ < 0 || cellAnimX_.empty()) return;

    const DWORD now = ::GetTickCount();
    DWORD dt = animLastTick_ ? (now - animLastTick_) : 16;
    animLastTick_ = now;
    if (dt > 100) dt = 100;                     // 卡顿/切后台回来别一步到位
    const double k = 1.0 - std::exp(-(double)dt / (double)kDragAnimTauMs);

    for (int i = 0; i < (int)cellAnimX_.size(); ++i) {
        // 被拖的那格跟着鼠标走，不参与让位
        if (i == dragCell_ - dragPageStart_) continue;
        float tx = 0.f, ty = 0.f;
        dragFinalPos(dragPageStart_ + i, tx, ty);
        cellAnimX_[i] += (tx - cellAnimX_[i]) * (float)k;
        cellAnimY_[i] += (ty - cellAnimY_[i]) * (float)k;
    }
}

// 第 origIdx 项在"松手后的最终布局"里落在哪个格子（返回左上角，网格坐标）。
//
// 映射与 moveShortcut 的 erase(from)+insert(to) 完全一致 ——
// 所以动画的终点就是松手后的落点，松手那一刻不会"跳一下"。
void IslandWindow::dragFinalPos(int origIdx, float& tx, float& ty) const {
    const int cols = cols_ > 0 ? cols_ : 1;
    int fin = origIdx;
    const int dFrom = dragCell_, dTo = dragTarget_;
    if (dFrom >= 0 && dTo >= 0) {
        if (origIdx == dFrom) fin = dTo;                                        // 被拖的去目标位
        else if (dFrom < dTo && origIdx > dFrom && origIdx <= dTo) fin = origIdx - 1;
        else if (dFrom > dTo && origIdx >= dTo && origIdx < dFrom) fin = origIdx + 1;
    }
    // fin 是全局下标 → 减掉页起始才是页内行列
    const int slot = fin - (dragPageStart_ >= 0 ? dragPageStart_ : 0);
    tx = (float)(gridX_ + (slot % cols) * cellW_);
    ty = (float)(gridY_ + (slot / cols) * cellH_);
}

void IslandWindow::drawGridCell(int index, int cx, int cy, int cw, int ch, bool hover) {
    // ★ 文字和图标都用**原始 alpha**，不再按面板透明度折算。
    //
    //   原来这里乘 sqrt(islandOpacity)，理由写在下面（"别像印在实心板上"）。
    //   但那个前提是"面板被额外降了 alpha"。现在面板参数已与标签面板
    //   对齐（不额外降 alpha），再折算就变成"岛台的文字比标签面板淡"，
    //   而且快捷方式图标也会发灰 —— 用户明确要求图标不要透明。
    //
    //   所以保持 fade() 这个函数形状（调用点不用改），系数固定为 1.0。
    const double kTxtAlpha = 1.0;
    auto fade = [&](uint32_t c) -> uint32_t {
        const uint32_t a = (c >> 24) & 0xFF;
        uint32_t na = (uint32_t)std::lround(a * kTxtAlpha);
        if (na > 255) na = 255;
        return (na << 24) | (c & 0x00FFFFFFu);
    };

    const std::vector<Shortcut>* items = currentShortcuts();
    if (!items || index < 0 || index >= (int)items->size()) return;
    const Shortcut& s = items->at(index);

    if (hover) {
        // 悬停：画一层圆角高亮
        frame_.fillRoundRect(cx + px(4), cy + px(4), cw - px(8), ch - px(8),
                             px(8), dark_ ? 0x30FFFFFF : 0x28000000);
    }

    // 大图标居中偏上
    const int iconX = cx + (cw - iconSize_) / 2;
    const int iconY = cy + px(10);

    // 图标来源：有自定义 iconPath 就用它，否则用目标路径让 shell 提取。
    // 两条路都交给 icons.cpp 的缓存，重复绘制不会反复解码。
    std::wstring iconSrc = s.iconPath.empty() ? s.targetPath : s.iconPath;
    HBITMAP bmp = iconFor(iconSrc, iconSize_);
    if (bmp) {
        // ★ 图标用完全不透明绘制。
        //
        //   原来这里是 drawBitmapFaded(bmp, ..., sqrt(islandOpacity))，
        //   图标跟着面板一起变淡 —— 微信那种自带底色方块的图标
        //   看起来就是一块发灰的贴纸。
        //   用户明确要求"岛台的快捷方式不要透明"，所以改成实心绘制。
        frame_.drawBitmap(bmp, iconX, iconY, iconSize_);
    } else {
        // 拿不到图标就画一个圆角占位块，避免格子空着看不出边界
        frame_.fillRoundRect(iconX, iconY, iconSize_, iconSize_, px(10),
                             dark_ ? 0x22FFFFFF : 0x18000000);
    }

    // 名称居中在图标下方，超出格宽就截断
    const int nameTop = iconY + iconSize_ + px(6);
    const int avail = cw - px(8);
    std::wstring label = Surface::fitText(s.name.c_str(), (float)pt(9), avail);
    RECT tr = {cx + px(4), nameTop, cx + cw - px(4), nameTop + px(18)};
    frame_.drawText(label.c_str(), tr, (float)pt(9), fade(textMain_), 1, false);
}

// ══ 面板内嵌的真搜索框 ══════════════════════════════════════════════
//
// 用户要求："把（标签栏搜索窗里）原本的那个搜索框复制一份，固定在岛台上"。
//
// ⚠ 为什么必须是真的 EDIT 控件（这一段是这次改动的全部要害）：
//     自绘框只能靠 WM_CHAR 收字符，而中文输入法会**截走按键** ——
//     字母进组合态、回车被上屏，窗口一个字符都收不到。
//     之前为了让自绘框能收字母，只能 ImmAssociateContext 把输入法摘掉，
//     代价是**搜不了中文**（这个限制反复被用户碰到）。
//     真 EDIT 有输入法组合窗口，拼音/五笔都正常。
//
// ⚠ 为什么必须是"独立弹窗"而不是子控件：
//     面板用 UpdateLayeredWindow 上屏，这种窗口**不渲染子控件** ——
//     子控件会整块消失（连文字都不剩）。
//     标签栏那个搜索窗（search.cpp 的 edit_）就是同样的方案，这里照抄。
//
// 视觉：输入框**没有底**（WM_CTLCOLOREDIT 返回 NULL_BRUSH + 透明背景模式），
//       面板的玻璃底直接透上来，EDIT 只负责在上面画文字。
//       （用户明确要求"把那个底给删了"，所以不要再加底色。）

const UINT_PTR kIslandSearchTimerId = 0xC6;   // 输入防抖
const UINT_PTR kDragLiveTimerId = 0xC7;       // 拖动中的实时重抓（80ms）

void IslandWindow::createSearchEdit(HINSTANCE hInst) {
    if (searchEdit_ || !hwnd_) return;

    // ⚠ 必须是 WS_EX_LAYERED：
    //   拖动中我们要用"不带 CAPTUREBLT"的抓屏排除自己和输入框（分层窗口会被排除），
    //   输入框不是分层的话就会被拍进背景里，玻璃上会烙着搜索框的样子。
    //   分层 + LWA_ALPHA(255) 的显示效果和不分层完全一样。
    searchEdit_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, L"EDIT", L"",
                                    WS_POPUP | ES_AUTOHSCROLL | ES_LEFT,
                                    0, 0, 10, 10, hwnd_, nullptr, hInst, nullptr);
    if (!searchEdit_) {
        logf("island: 搜索输入框创建失败 err=%lu", (unsigned long)::GetLastError());
        return;
    }
    // 子类过程要靠 GWLP_USERDATA 找到 this —— 必须在挂钩之前设好
    ::SetWindowLongPtrW(searchEdit_, GWLP_USERDATA, (LONG_PTR)this);
    // 分层只是为了让抓屏能排除它，alpha 必须给满 —— 否则搜索框就变半透明了
    ::SetLayeredWindowAttributes(searchEdit_, 0, 255, LWA_ALPHA);

    searchFont_ = ::CreateFontW(-pt(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    if (searchFont_) ::SendMessageW(searchEdit_, WM_SETFONT, (WPARAM)searchFont_, TRUE);

    // ⚠ 不设占位提示（cue banner / 自绘文字都不设）。
    //   标签栏那个搜索窗本来就没有占位，忠实复制它；
    //   而且在透明底 EDIT 上 cue banner 本来也不渲染。

    searchEditOldProc_ = (WNDPROC)::SetWindowLongPtrW(searchEdit_, GWLP_WNDPROC,
                                                      (LONG_PTR)islandSearchEditProc);
    logf("island: 真搜索框已创建（EDIT，输入法可用）");
}

// 取一点像素当输入框底色（见 redraw 里的调用）。
//
// ⚠ 位置要避开放大镜字形：取在它右边一点。
int IslandWindow::iconSkipForSample() const {
    return px(12) + px(20) + px(10);   // 图标左边距 + 图标宽 + 一点余量
}

void IslandWindow::updateSearchBgColor(int x, int y) {
    const auto& pxv = frame_.pixels();
    const int wpx = frame_.width();
    const int idx = y * wpx + x;
    if (wpx <= 0 || idx < 0 || idx >= (int)pxv.size()) return;
    const uint32_t col = (pxv[idx] & 0x00FFFFFFu) | 0xFF000000u;
    if (col == searchBgCol_) return;
    searchBgCol_ = col;
    if (searchBgBrush_) {
        ::DeleteObject(searchBgBrush_);
        searchBgBrush_ = nullptr;
    }
    if (searchEdit_) ::InvalidateRect(searchEdit_, nullptr, TRUE);
}

void IslandWindow::layoutSearchEdit() {
    if (!searchEdit_) return;

    // ⚠ 基准用**实际窗口矩形**，不用成员 x_/y_（记忆位置在窗口被系统
    //   挪动/缩放后会短暂不一致，输入框就会错位）。
    RECT wr{};
    if (!::GetWindowRect(hwnd_, &wr)) return;

    // ⚠ 输入框的矩形要和**画出来的提亮条完全对齐**（同一个 top、同一个高度）。
    //
    //   踩过的坑：之前输入框 top 比提亮条高 2px、高度又多 4px，
    //   于是 EDIT 内部那套垂直居中是以"更高的矩形"为基准算的 ——
    //   结果打上去的字贴在提亮条的上沿，不在中间。
    //   两者对齐之后，EDIT 自己居中就等于在提亮条里居中。
    const int sx = px(12);
    const int sy = px(8);
    const int sw = w_ - px(24);
    const int sh = searchH_ - px(16);
    const int iconW = px(38);          // 左边留给放大镜（见 redraw 里画的那个字形）
    // ★ + 外阴影留白：窗口外框在"内容左上角"的左上方，正好差这一圈。
    //   不加的话搜索框会整体偏出留白那么多（视觉上错位一整条）。
    const int shd = islandShadowPx();
    const int ex = wr.left + shd + sx + iconW;
    // ⚠ 下移 px(6)（实测：贴着提亮条时文字中心比提亮条中心高约 8.5px，
    //   下移 px(6)=9px 正好居中）。高度同时减 px(12)，
    //   保证整块仍在提亮条内 —— 越出去会露出生硬的直角边。
    const int ey = wr.top + shd + sy + px(6);
    int ew = sw - iconW;
    int eh = sh - px(12);
    if (ew < px(80)) ew = px(80);
    if (eh < px(18)) eh = px(18);

    // ⚠ 双保险：不只看 visible_，还要看面板窗口**此刻真的可见**。
    //   否则像 hide() 那种消息还没走完的瞬间，会把输入框又显示回来。
    const bool showIt = visible_ && ::IsWindowVisible(hwnd_);
    ::SetWindowPos(searchEdit_, HWND_TOPMOST, ex, ey, ew, eh,
                   SWP_NOACTIVATE | (showIt ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));

    // EDIT 本身是方角矩形，套一个圆角区域才像个搜索框
    // （14:41 那版就有它；这次"只删底"，所以保留）。
    HRGN rgn = ::CreateRoundRectRgn(0, 0, ew + 1, eh + 1, eh, eh);
    if (rgn) ::SetWindowRgn(searchEdit_, rgn, TRUE);   // 成功后系统接管这个 rgn
}

void IslandWindow::showSearchEdit(bool show) {
    if (!searchEdit_) return;
    if (show) {
        layoutSearchEdit();
        ::ShowWindow(searchEdit_, SW_SHOWNOACTIVATE);
    } else {
        ::ShowWindow(searchEdit_, SW_HIDE);
        // 收起面板就把内容清掉 —— 下次打开是干净的搜索框
        ::SetWindowTextW(searchEdit_, L"");
        searchTyping_ = false;
        results_.clear();
        resultSel_ = 0;
        resultScroll_ = 0;
    }
}

void IslandWindow::focusSearchEdit() {
    if (!searchEdit_) return;
    // 真 EDIT 必须拿到前台 + 焦点，键盘和输入法才会把它当输入目标。
    // 用户是"点了搜索框"才走到这里，属于用户操作触发，系统会放行 SetForegroundWindow。
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(searchEdit_);
    logf("island: 点搜索框 -> 聚焦真输入框 fg=%p focus=%p",
         (void*)::GetForegroundWindow(), (void*)::GetFocus());
}

void IslandWindow::runIslandSearch() {
    if (!searchEdit_) return;
    wchar_t buf[512] = {};
    ::GetWindowTextW(searchEdit_, buf, 512);
    std::wstring q = buf;
    while (!q.empty() && (q.front() == L' ' || q.front() == L'\t')) q.erase(q.begin());
    while (!q.empty() && (q.back() == L' ' || q.back() == L'\t')) q.pop_back();

    resultSel_ = 0;
    resultScroll_ = 0;
    if (q.empty()) {
        searchTyping_ = false;
        results_.clear();
    } else {
        searchTyping_ = true;
        // ⚠ 走搜索窗**同一套**匹配与排序（search.cpp 的 queryHits），
        //   不然同一个词在岛台和搜索窗里结果顺序不一样，用户会以为坏了。
        results_ = queryHits(q, 200);
    }
    // ⚠ RECT 的成员是 LONG，不转 int 的话 std::max 会因 int/long 类型不匹配而报错
    resultRowsVis_ = std::max(1, (int)(resultArea_.bottom - resultArea_.top) /
                                     std::max(1, resultRowH_));
    logf("island: 内嵌搜索 -> %d 条", (int)results_.size());
    redraw();
}

void IslandWindow::clearIslandSearch() {
    if (!searchEdit_) return;
    ::SetWindowTextW(searchEdit_, L"");
    searchTyping_ = false;
    results_.clear();
    resultSel_ = 0;
    resultScroll_ = 0;
    redraw();
}

int IslandWindow::resultRowAt(int x, int y) const {
    if (!searchTyping_ || resultRowH_ <= 0) return -1;
    if (x < resultArea_.left || x >= resultArea_.right) return -1;
    if (y < resultArea_.top || y >= resultArea_.bottom) return -1;
    const int idx = resultScroll_ + (y - resultArea_.top) / resultRowH_;
    if (idx < 0 || idx >= (int)results_.size()) return -1;
    return idx;
}

void IslandWindow::moveResultSel(int delta) {
    if (results_.empty()) return;
    const int last = (int)results_.size() - 1;
    resultSel_ = std::max(0, std::min(last, resultSel_ + delta));
    // 选中行始终留在可视区
    const int rows = std::max(1, resultRowsVis_);
    if (resultSel_ < resultScroll_) {
        resultScroll_ = resultSel_;
    } else if (resultSel_ >= resultScroll_ + rows) {
        resultScroll_ = resultSel_ - rows + 1;
    }
    redraw();
}

void IslandWindow::openResult(int index, bool reveal) {
    if (index < 0 || index >= (int)results_.size()) return;
    const SearchHit hit = results_[index];   // 拷贝：下面 hide() 会清空 results_
    logf("island: 打开结果 %s", toUtf8(hit.name).c_str());
    if (reveal) {
        std::wstring sel = L"/select,\"" + hit.path + L"\"";
        ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
        ::ShellExecuteW(nullptr, L"open", hit.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    hide();
}

void IslandWindow::drawResults() {
    const int n = (int)results_.size();
    if (n == 0) {
        RECT r = resultArea_;
        frame_.drawText(L"没有匹配的结果", r, (float)pt(12), textDim_, 1, false);
        return;
    }
    const int rows = std::max(1, resultRowsVis_);
    for (int i = 0; i < rows; ++i) {
        const int idx = resultScroll_ + i;
        if (idx >= n) break;
        const SearchHit& hit = results_[idx];
        const int y1 = resultArea_.top + i * resultRowH_;
        const int y2 = y1 + resultRowH_;

        if (idx == resultSel_) {
            frame_.fillRoundRect(resultArea_.left, y1 + px(2),
                                 resultArea_.right - resultArea_.left, resultRowH_ - px(4),
                                 px(8), dark_ ? 0x30FFFFFFu : 0x28000000u);
        }

        const int isz = px(24);
        const int ix = resultArea_.left + px(10);
        const int iy = (y1 + y2) / 2;
        // 图标走的是同一个缓存（启动时预热过），这里不会再触发磁盘解码
        HBITMAP bmp = iconFor(hit.path, isz);
        if (bmp) frame_.drawBitmap(bmp, ix, iy - isz / 2, isz);

        const int tx = ix + isz + px(12);
        const int right = resultArea_.right - px(24);
        RECT rn = {tx, y1 + px(5), right, y1 + px(5) + px(20)};
        frame_.drawText(hit.name.c_str(), rn, (float)pt(11), textMain_, 0, false);
        RECT rp = {tx, y1 + px(25), right, y2 - px(3)};
        frame_.drawText(hit.parent.c_str(), rp, (float)pt(9), textDim_, 0, false);
    }

    // 操作提示：画在结果区**下面**那条，不和最后一行叠
    RECT rf = {resultArea_.left + px(8), resultArea_.bottom + px(2),
               resultArea_.right, resultArea_.bottom + px(22)};
    frame_.drawText(L"↑↓ 选择　Enter 打开　Ctrl+Enter 所在位置　Esc 清空", rf,
                    (float)pt(9), textDim_, 0, false);
}

LRESULT CALLBACK islandSearchEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    IslandWindow* self = (IslandWindow*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self) {
        switch (msg) {
            case WM_KEYDOWN:
                switch (wp) {
                    case VK_DOWN:  self->moveResultSel(1);  return 0;
                    case VK_UP:    self->moveResultSel(-1); return 0;
                    case VK_NEXT:
                        self->moveResultSel(std::max(1, self->resultRowsVis_) - 1);
                        return 0;
                    case VK_PRIOR:
                        self->moveResultSel(-(std::max(1, self->resultRowsVis_) - 1));
                        return 0;
                    case VK_RETURN:
                        // Ctrl+Enter = 打开所在位置（和搜索窗一致）
                        self->openResult(self->resultSel_,
                                         (::GetKeyState(VK_CONTROL) & 0x8000) != 0);
                        return 0;
                    case VK_ESCAPE:
                        // 有内容先清空（留在搜索态），空了再收面板 —— 和输入法习惯一致
                        if (self->searchTyping_) self->clearIslandSearch();
                        else self->hide();
                        return 0;
                    case VK_TAB:
                        return 0;
                    default:
                        break;
                }
                break;
            case WM_IME_STARTCOMPOSITION: {
                // ★ 把输入法的组合窗口挪到搜索框**下面**。
                //
                //   踩过的坑：输入法（微软拼音）的组合窗口默认就压在
                //   正在输入的文字上 —— 它自带白底黑字，是一块很显眼的白块。
                //   那是输入法自己的窗口（MSCTFIME Composition），
                //   我们的 WM_CTLCOLOREDIT 管不到它，只能把它挪走。
                //
                //   挪到输入框下沿之后，输入框里就只剩（白）文字，
                //   拼音组合显示在下面一行。
                HIMC imc = ::ImmGetContext(h);
                if (imc) {
                    RECT cr{};
                    ::GetClientRect(h, &cr);
                    COMPOSITIONFORM cf{};
                    cf.dwStyle = CFS_POINT;
                    cf.ptCurrentPos.x = 0;
                    cf.ptCurrentPos.y = cr.bottom + px(4);
                    ::ImmSetCompositionWindow(imc, &cf);
                    ::ImmReleaseContext(h, imc);
                }
                break;      // 继续交给 EDIT 自己处理
            }
            case WM_MOUSEWHEEL: {
                const int d = GET_WHEEL_DELTA_WPARAM(wp);
                self->moveResultSel(d > 0 ? -3 : 3);
                return 0;
            }
            default:
                break;
        }
        WNDPROC old = self->searchEditOldProc_;
        return ::CallWindowProcW(old ? old : ::DefWindowProcW, h, msg, wp, lp);
    }
    return ::DefWindowProcW(h, msg, wp, lp);
}

void IslandWindow::redraw() {
    if (!hwnd_ || w_ <= 0 || h_ <= 0 || !cfg_) return;
    if (!baseValid_) rebuildBase();
    frame_ = base_;

    const Theme& th = themeByName(cfg_->themeColor);

    // ── 顶部搜索框：柔和的提亮底 + 左边的放大镜 ──
    //
    // ⚠ 这里只画"壳"：一层提亮 + 放大镜字形。
    //   输入是搬过来的那个真 EDIT 负责的（它没有底，所以透出来的就是这层提亮）。
    //   不画占位文字 —— 标签栏那个搜索窗也没有占位。
    const int sx = px(12);
    const int sy = px(8);
    const int sw = w_ - px(24);
    const int sh = searchH_ - px(16);
    if (sw > px(80) && sh > px(16)) {
        // ★ 输入框的底必须**不透明**：它是那个真 EDIT 窗口自己刷底色的，
        //   而 EDIT 只能填一个实色 —— 两边同色才零色差（下面 updateSearchBgColor 的用意）。
        //
        //   ⚠ 原来画的是半透明白 0x28FFFFFF，而 updateSearchBgColor() 从这一帧取像素时
        //     只取 RGB、把 alpha 强写成 255 → EDIT 拿到的是**纯白实色**画刷，
        //     用户看到的就是搜索框那条"非常亮的白底"。改成不透明深色后才真正一致。
        const uint32_t boxCol = 0xFF1B1D22u;   // 不透明深色：白字黑边压在上面读得清
        frame_.fillRoundRect(sx, sy, sw, sh, sh / 2, boxCol);
        // 一圈细玻璃边，让它看起来是个输入框而不是一块黑板
        frame_.strokeGlassEdge(sx, sy, sw, sh, sh / 2, dark_, 1.0);

        // 放大镜（MDL2 字形）
        const int ix = sx + px(12);
        const int iy = sy + sh / 2;
        RECT ir = {ix, iy - px(10), ix + px(20), iy + px(10)};
        frame_.drawText(L"\uE721", ir, (float)pt(11), textDim_, 0, false, L"Segoe MDL2 Assets");

        // ★ 从**画好的这一帧**里取一个像素当输入框底色。
        //
        //   为什么不用估算（玻璃均值 + 叠加）：那样总有几级色差，
        //   在胶囊里就会显出一块矩形（用户之前就为这个发过火）。
        //   直接取像素 = 零色差，输入框的底和提亮条完全融为一体。
        //   取点在放大镜右边、竖直中间，避开放大镜字形本身。
        updateSearchBgColor(sx + iconSkipForSample(), sy + sh / 2);
    }

    if (searchTyping_) {
        // 搜索态：整块内容区让给结果列表（分类栏和网格都不画）。
        // 下面那一大段（分类 + 网格 + 拖动让位）是它的 else 分支 ——
        // 缩进没跟着改，是为了让这次改动只多两行、不动几百行。
        drawResults();
    } else {

    // ── 左侧分类栏 ──
    {
        const int itemH = px(34);
        int cy = searchH_ + px(8);
        for (size_t i = 0; i < categories_.size(); ++i) {
            const Category& c = categories_[i];
            const bool active = (c.id == cfg_->islandCategory);
            const bool hot = ((int)i == hotCategory_);

            if (active) {
                frame_.fillRoundRect(px(8), cy, sideW_ - px(16), itemH - px(4), px(8),
                                     dark_ ? 0x38FFFFFF : 0x28000000);
            } else if (hot) {
                frame_.fillRoundRect(px(8), cy, sideW_ - px(16), itemH - px(4), px(8),
                                     dark_ ? 0x18FFFFFF : 0x12000000);
            }

            RECT tr = {px(18), cy, sideW_ - px(30), cy + itemH - px(4)};
            std::wstring label = Surface::fitText(c.name.c_str(), (float)pt(10),
                                                  sideW_ - px(48));
            frame_.drawText(label.c_str(), tr, (float)pt(10), textMain_, 0, active);

            // 右侧数量
            wchar_t cnt[16];
            swprintf_s(cnt, L"%d", c.shortcutCount);
            RECT cr = {sideW_ - px(42), cy, sideW_ - px(14), cy + itemH - px(4)};
            frame_.drawText(cnt, cr, (float)pt(9), textDim_, 2, false);

            cy += itemH;
        }
    }

    // ── 大图标网格 ──
    {
        computeGrid();
        const std::vector<Shortcut>* items = currentShortcuts();
        const int total = items ? (int)items->size() : 0;
        const int perPage = cols_ * rows_;
        const int start = scrollRow_ * perPage;

        if (total == 0) {
            RECT tr = {gridX_, gridY_, gridX_ + gridW_, gridY_ + gridH_};
            frame_.drawText(L"右键此处添加快捷方式", tr, (float)pt(10), textDim_, 1, false);
        } else {
            // ── 拖动中的"挤压让位"──
            //
            // 用户要求能看到过程：被拖的图标跟着鼠标走，
            // 其它图标实时让位，而不是松手才"啪"地换。
            //
            // 算法：
            //   设被拖的是 from、目标格是 to（都相对当前页）
            //     · from < to：from+1 .. to 这些项往前挪一格
            //     · from > to：to .. from-1 这些项往后挪一格
            //   位移量再乘一个 0→1 的缓动系数，就有"被挤开"的观感。
            const bool dragging = (dragCell_ >= 0);
            const int dFrom = dragging ? dragCell_ : -1;
            const int dTo = dragging ? dragTarget_ : -1;

            // ★ 让位动画：每格独立缓动（说明见 island.h）。
            //   旧做法是一个全局进度、每次目标格变化就重置为 0 ——
            //   已经让开的格子会先弹回原位再推过去，快速划过几格时一顿一顿。
            //   这里只负责"惰性初始化"和"不再拖动时清掉"，推进交给 stepCellAnim。
            dragPageStart_ = dragging ? start : -1;
            if (dragging) {
                if ((int)cellAnimX_.size() != perPage) {
                    cellAnimX_.assign((size_t)perPage, 0.f);
                    cellAnimY_.assign((size_t)perPage, 0.f);
                    for (int i = 0; i < perPage; ++i) {
                        const int row0 = i / cols_, col0 = i % cols_;
                        cellAnimX_[i] = (float)(gridX_ + col0 * cellW_);
                        cellAnimY_[i] = (float)(gridY_ + row0 * cellH_);
                    }
                    animLastTick_ = 0;
                }
            } else if (!cellAnimX_.empty()) {
                cellAnimX_.clear();
                cellAnimY_.clear();
                animLastTick_ = 0;
            }

            for (int i = 0; i < perPage; ++i) {
                const int idx = start + i;
                if (idx >= total) break;
                if (dragging && idx == dFrom) continue;   // 被拖的那个最后单独画

                // 当前显示位置：拖动中取每格缓动的结果，否则就是它本来的格子
                int cx, cy;
                if (dragging && i < (int)cellAnimX_.size()) {
                    cx = (int)std::lround(cellAnimX_[i]);
                    cy = (int)std::lround(cellAnimY_[i]);
                } else {
                    const int row0 = i / cols_, col0 = i % cols_;
                    cx = gridX_ + col0 * cellW_;
                    cy = gridY_ + row0 * cellH_;
                }
                drawGridCell(idx, cx, cy, cellW_, cellH_, idx == hotCell_);
            }

            // ── 被拖的那个：画在最上层，位置跟着鼠标 ──
            //
            // 浮起来的效果：稍微放大 + 半透明底色 + 描边，
            // 让用户一眼看出"这个被我拿起来了"。
            if (dragging && dFrom >= 0 && dFrom < total) {
                // 图标中心跟着鼠标，但保持在面板范围内
                int cx = dragCurPt_.x - cellW_ / 2;
                int cy = dragCurPt_.y - cellH_ / 2;
                cx = std::max(gridX_, std::min(cx, gridX_ + gridW_ - cellW_));
                cy = std::max(gridY_, std::min(cy, gridY_ + gridH_ - cellH_));

                // ★ 残影做成"面板同款"。
                //
                //   之前这里是 3px 的硬白描边（strokeRoundRect + 0x90F5F5F7），
                //   用户："鼠标长按选中的时候那个边框太难看了"。
                //   现在与面板/标签/图标走同一套观感：
                //     ① 柔和外阴影（只落在矩形外圈，不盖格子内容）
                //     ② 半透明底（和面板的雾同一个色系）
                //     ③ strokeGlassEdge —— 一暗一亮两道细边（玻璃厚度感）
                const int pad = px(6);
                const int gx = cx - pad, gy = cy - pad;
                const int gw = cellW_ + pad * 2, gh = cellH_ + pad * 2;
                const int gr = px(12);

                frame_.drawSoftShadow(gx, gy, gw, gh, gr, dark_, px(10));
                frame_.fillRoundRect(gx, gy, gw, gh, gr,
                                     dark_ ? 0x46101216u : 0x40F1F3F7u);
                frame_.strokeGlassEdge(gx, gy, gw, gh, gr, dark_, (float)1.6);

                drawGridCell(dFrom, cx, cy, cellW_, cellH_, true);
            }

            // 多页时右下角显示页码
            if (maxScrollRow_ > 0) {
                wchar_t pg[32];
                swprintf_s(pg, L"%d / %d", scrollRow_ + 1, maxScrollRow_ + 1);
                RECT pr = {gridX_ + gridW_ - px(70), gridY_ + gridH_ - px(22),
                           gridX_ + gridW_ - px(10), gridY_ + gridH_ - px(4)};
                frame_.drawText(pg, pr, (float)pt(9), textDim_, 2, false);
            }
        }
    }

    }  // ← else（非搜索态：分类 + 网格）

    // ── 左下用户按钮 ──
    {
        frame_.fillRoundRect(userBtn_.left, userBtn_.top,
                             userBtn_.right - userBtn_.left,
                             userBtn_.bottom - userBtn_.top,
                             px(8), dark_ ? 0x18FFFFFF : 0x12000000);
        const int cy = (userBtn_.top + userBtn_.bottom) / 2;
        RECT ir = {userBtn_.left + px(10), cy - px(9), userBtn_.left + px(28), cy + px(9)};
        frame_.drawText(L"\uE77B", ir, (float)pt(11), textMain_, 0, false, L"Segoe MDL2 Assets");

        std::wstring user = currentUserName();
        RECT tr = {userBtn_.left + px(34), userBtn_.top, userBtn_.right - px(8), userBtn_.bottom};
        std::wstring label = Surface::fitText(user.c_str(), (float)pt(10),
                                              (userBtn_.right - userBtn_.left) - px(44));
        frame_.drawText(label.c_str(), tr, (float)pt(10), textMain_, 0, false);
    }

    // ── 右下电源按钮 ──
    {
        frame_.fillRoundRect(powerBtn_.left, powerBtn_.top,
                             powerBtn_.right - powerBtn_.left,
                             powerBtn_.bottom - powerBtn_.top,
                             px(8), dark_ ? 0x18FFFFFF : 0x12000000);
        const int cy = (powerBtn_.top + powerBtn_.bottom) / 2;
        RECT ir = {powerBtn_.left + px(10), cy - px(9), powerBtn_.left + px(28), cy + px(9)};
        frame_.drawText(L"\uE7E8", ir, (float)pt(11), textMain_, 0, false, L"Segoe MDL2 Assets");

        RECT tr = {powerBtn_.left + px(34), powerBtn_.top, powerBtn_.right - px(8), powerBtn_.bottom};
        frame_.drawText(L"电源", tr, (float)pt(10), textMain_, 0, false);
    }

    // 右上角的 × 关闭按钮**已删除**。
    //
    // 用户要求删掉。理由很充分：面板现在点外面就自动收回，
    // × 是多余的，还白占一块地方。

    // ⚠ 提交前把最外圈 alpha 抬到非零，否则边缘拖不动。
    //
    //   分层窗口里 alpha=0 的像素不接收鼠标，系统的命中测试直接穿透。
    //   而圆角是靠 cutRoundCorners 把角上 alpha 切成 0 做的，
    //   所以"拖右下角改大小"收不到 WM_NCHITTEST —— 表现为边框拖不动。
    //
    //   （实测：拖右边中部能拖，拖右下角完全没反应。）
    //
    //   详见 glass.h 里 ensureHitTestableEdges 的说明。
    // ⚠ 提交前要让鼠标能落在面板上：分层窗口的命中测试**只看 alpha**，
    //   而岛台主体是纯透明的（buildFlatPanel 不铺底）——
    //   不抬 alpha，格子空白处的点击/拖动会直接穿透到背后的窗口。
    //   原来只抬了最外圈（ensureHitTestableEdges，为了四角能拖着改大小），
    //   现在整块内容都抬到 alpha=1（1/255，肉眼不可见），见 glass.h 的说明。
    RECT contentRc = {0, 0, w_, h_};
    frame_.ensureHitTestableArea(contentRc, px(14),
                                 hitTestAlphaFor(cfg_ ? cfg_->panelOpacity : 100));

    // ★ 内容 + 外阴影合成到"带留白"的画布，再整幅提交。
    //   落点要向左上各让出留白（dst = 内容位置 - 留白），
    //   所以鼠标坐标要减掉同样的留白（见 contentPt）。
    const int s = shadowInset();
    outer_ = shadow_;
    outer_.blendOver(frame_, s, s);

    HDC screen = GetDC(nullptr);
    HBITMAP bmp = outer_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT src = {0, 0};
        SIZE size = {outer_.width(), outer_.height()};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT dst = {x_ - s, y_ - s};

        // ★ 「从图标位置长出来」：窗口矩形从 growFrom_ 插值到 growTo_，同时淡入。
        //   位图不用重做 —— UpdateLayeredWindow 会把它拉伸到 dst 尺寸，
        //   缩放效果是白送的。t 到 1 就收工（顺手杀掉定时器）。
        if (growActive_) {
            const DWORD el = ::GetTickCount() - growStart_;
            double t = (double)el / (double)kGrowMs;
            if (t >= 1.0) {
                t = 1.0;
                growActive_ = false;
                ::KillTimer(hwnd_, kGrowTimerId);
            }
            const double e = 1.0 - std::pow(1.0 - t, 3.0);   // easeOutCubic：起步快、收尾稳
            auto lerp = [](int a, int b, double k) {
                return (int)std::lround((double)a + ((double)b - (double)a) * k);
            };
            dst.x = lerp(growFrom_.left, growTo_.left, e);
            dst.y = lerp(growFrom_.top, growTo_.top, e);
            size.cx = lerp(growFrom_.right - growFrom_.left, growTo_.right - growTo_.left, e);
            size.cy = lerp(growFrom_.bottom - growFrom_.top, growTo_.bottom - growTo_.top, e);
            blend.SourceConstantAlpha = (BYTE)std::lround(50.0 + 205.0 * e);   // 顺带淡入
        }
        // ★ 全局面板透明度：滑块 0-100 作用在这一帧的整体 alpha 上
        // ★ 不再按「面板透明度」缩放整帧 alpha：滑块现在只把**背景**加雾（见 glass.cpp buildFlatPanel），否则连文字/图标/边框一起变透明。
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
    (void)th;
}

// ── 搜索 ────────────────────────────────────────────────────────────

// ── 用户菜单（复现开始菜单左下角那个）─────────────────────────────────

void IslandWindow::showUserMenu() {
    std::vector<MenuRow> rows;

    // 用户名作为不可点的一项（和开始菜单一样，点它进账户设置）
    MenuRow name;
    name.glyph = L"\uE77B";
    name.text = currentUserName();
    name.cb = [] {
        // 打开"你的账户"设置页
        ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
        ::ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
    };
    rows.push_back(name);

    MenuRow sep1; sep1.sep = true;
    rows.push_back(sep1);

    MenuRow lock; lock.glyph = L"\uE72E"; lock.text = L"锁定";
    lock.cb = [] { lockWorkstation(); };
    rows.push_back(lock);

    // 远程会话才有"断开连接"，本地登录时隐藏
    if (::GetSystemMetrics(SM_REMOTESESSION)) {
        MenuRow disc; disc.glyph = L"\uE8CD"; disc.text = L"断开连接";
        disc.cb = [] { disconnectSession(); };
        rows.push_back(disc);
    }

    MenuRow out; out.glyph = L"\uE7E8"; out.text = L"注销";
    out.cb = [] { logoffSession(); };
    rows.push_back(out);

    MenuRow sw; sw.glyph = L"\uE748"; sw.text = L"切换用户";
    sw.cb = [] {
        // 切换用户没有直接 API。锁定屏幕后用户在登录界面
        // 可以选"其他用户" —— 这是能做到的最接近的行为。
        lockWorkstation();
    };
    rows.push_back(sw);

    MenuRow sep2; sep2.sep = true;
    rows.push_back(sep2);

    MenuRow settings; settings.glyph = L"\uE713"; settings.text = L"账户设置";
    settings.cb = [] {
        ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
        ::ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
    };
    rows.push_back(settings);

    // 菜单弹在按钮上方（面板底部，往下弹会被屏幕裁掉）
    int mx = x_ + userBtn_.left;
    int my = y_ + userBtn_.top - px(8);
    showGlassMenu(hwnd_, rows, mx, my, dark_);
}

// ── 电源菜单（复现开始菜单右下角那个）─────────────────────────────────

void IslandWindow::showPowerMenu() {
    std::vector<MenuRow> rows;

    // ★ 只保留三项：睡眠、关机、重启。
    //
    //   原来还有「休眠」和「注销」—— 用户明确要求去掉，只要这三项。
    //   理由也合理：休眠在很多机器上本来就被禁用（会直接返回失败），
    //   注销则容易误点（正在跑的程序会被关掉）。
    //
    //   顺序按 Windows 自己的电源菜单：睡眠 → 关机 → 重启。
    MenuRow sleep; sleep.glyph = L"\uE708"; sleep.text = L"睡眠";
    sleep.cb = [] { sleepSystem(); };
    rows.push_back(sleep);

    // 有待装更新时，开始菜单会把"关机"变成"更新并关机"。
    // 我们照做 —— 措辞一致，用户不会意外。
    const bool upd = updatesPending();

    MenuRow off;
    off.glyph = L"\uE7E8";
    off.text = upd ? L"更新并关机" : L"关机";
    off.cb = [] { shutdownSystem(); };
    rows.push_back(off);

    MenuRow restart;
    restart.glyph = L"\uE777";
    restart.text = upd ? L"更新并重启" : L"重启";
    restart.cb = [] { restartSystem(); };
    rows.push_back(restart);

    int mx = x_ + powerBtn_.right - px(160);
    int my = y_ + powerBtn_.top - px(8);
    showGlassMenu(hwnd_, rows, mx, my, dark_);
}

// ── 右键菜单 ────────────────────────────────────────────────────────

void IslandWindow::showContextMenu(int screenX, int screenY, int gridIndex) {
    std::vector<MenuRow> rows;
    const std::vector<Shortcut>* items = currentShortcuts();

    if (gridIndex >= 0 && items && gridIndex < (int)items->size()) {
        // 点在某个快捷方式上
        const Shortcut s = items->at(gridIndex);

        MenuRow open; open.glyph = L"\uE8E5"; open.text = L"打开";
        open.cb = [this, s] {
            if (onLaunchPath) onLaunchPath(s.targetPath);
        };
        rows.push_back(open);

        MenuRow loc; loc.glyph = L"\uE838"; loc.text = L"打开文件位置";
        loc.cb = [this, s] {
            if (onLaunchPath) onLaunchPath(L"/select," + s.targetPath);
        };
        rows.push_back(loc);

        MenuRow sep; sep.sep = true;
        rows.push_back(sep);

        // 「全部」分类里的项可以删；标签分类里的项属于那个标签，
        // 删除要动标签数据，这里不给删（避免误操作删掉标签内容）。
        if (cfg_ && cfg_->islandCategory == kAllCategory) {
            MenuRow del; del.glyph = L"\uE74D"; del.text = L"从全部移除";
            del.cb = [this, gridIndex] {
                if (!cfg_) return;
                if (gridIndex >= 0 && gridIndex < (int)cfg_->islandShortcuts.size()) {
                    cfg_->islandShortcuts.erase(cfg_->islandShortcuts.begin() + gridIndex);
                    buildCategories();
                    computeGrid();
                    baseValid_ = false;
                    redraw();
                    if (onConfigDirty) onConfigDirty();
                    logf("island: 从「全部」移除第 %d 项", gridIndex);
                }
            };
            rows.push_back(del);
        }
    }

    // 空白处或任何位置都能加
    if (!rows.empty()) {
        MenuRow sep; sep.sep = true;
        rows.push_back(sep);
    }

    MenuRow add; add.glyph = L"\uE710"; add.text = L"添加快捷方式";
    add.cb = [this] { addShortcutDialog(); };
    rows.push_back(add);

    showGlassMenu(hwnd_, rows, screenX, screenY, dark_);
}

void IslandWindow::addShortcutDialog() {
    if (!cfg_) return;

    // 用系统的文件选择框。
    //
    // 不做自定义界面：选文件这件事系统对话框已经做得很好，
    // 而且支持拖放、最近访问、搜索。自己画一个只会更难用。
    std::wstring path;
    if (!pickFileDialog(hwnd_, L"选择要添加的程序或文件",
                        L"所有文件\0*.*\0程序\0*.exe;*.lnk;*.bat;*.cmd\0\0", path)) {
        return;
    }

    if (path.empty()) return;

    Shortcut s;
    s.id = shortId();
    // 名称默认取文件名（去掉扩展名）
    size_t slash = path.find_last_of(L"\\/");
    std::wstring base = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of(L'.');
    s.name = (dot == std::wstring::npos || dot == 0) ? base : base.substr(0, dot);
    s.targetPath = path;
    s.iconPath = L"";
    s.args = L"";
    s.createdAt = L"";

    cfg_->islandShortcuts.push_back(s);

    // 新加的项一定在「全部」里 —— 如果用户当前在别的分类，
    // 自动切过去，否则他会以为"加了但没看到"。
    cfg_->islandCategory = kAllCategory;

    buildCategories();
    computeGrid();
    baseValid_ = false;
    redraw();
    if (onConfigDirty) onConfigDirty();

    logf("island: 已添加到「全部」：%s", toUtf8(s.name).c_str());
}

// ── 窗口过程 ────────────────────────────────────────────────────────

// ── 鼠标坐标换算 ────────────────────────────────────────────────────
//
// ⚠⚠ 面板的鼠标坐标必须补一个非客户区内缩，否则会差 10px。
//
//   原因（实测确认，不是推测）：
//     ① 画面是用 UpdateLayeredWindow 贴在**窗口外框左上角**渲染的
//        —— 截图看，第 0 行就已经有内容。
//     ② 而鼠标消息（WM_LBUTTONDOWN 等）里的坐标是**客户区坐标**。
//     ③ 面板带 WS_THICKFRAME，客户区原点比外框内缩 (10,10)
//        —— 实测：窗口 1305x791，客户区 1285x771，ClientToScreen(0,0) = 外框+(10,10)。
//
//   于是同一个屏幕点，在画面里是 y=12，在鼠标消息里却是 y=2。
//   表现就是**搜索框最上面那 10px 点不上** ——
//   而搜索程序、文件、快捷方式…这行提示文字恰好画在那一条上，
//   用户点它当然没反应（用户的原话：根本点不上）。
//
//   网格/分类/按钮也都有这 10px 偏移，只是它们的格子大，
//   差 10px 感觉不出来 —— 所以这个问题只在搜索框上暴露。
//
//   这里统一换算一次，让鼠标坐标和画面坐标系一致。
POINT contentPt(HWND h, LPARAM l) {
    POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    RECT wr{}, cr{};
    if (::GetWindowRect(h, &wr) && ::GetClientRect(h, &cr)) {
        POINT c0 = {0, 0};
        ::ClientToScreen(h, &c0);
        p.x += (c0.x - wr.left);
        p.y += (c0.y - wr.top);
    }
    // ★ 再减掉外阴影留白：画面里的**内容**是从留白处开始画的
    //   （提交时 UpdateLayeredWindow 的落点 = 内容位置 - 留白），
    //   所以客户区坐标比内容坐标多出这一圈。
    p.x -= islandShadowPx();
    p.y -= islandShadowPx();
    return p;
}

LRESULT CALLBACK IslandWindow::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    IslandWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<IslandWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<IslandWindow*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->handle(h, msg, w, l);
    return ::DefWindowProcW(h, msg, w, l);
}

LRESULT IslandWindow::handle(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            // ★ 把 WS_THICKFRAME 那圈**非客户区边框宽度变成 0**。
            //
            //   用户要求：“鼠标移动到岛台面板边框的时候光标不变”。
            //   那圈边框是系统自带的（为了让 HTxxx 能用，
            //   窗口必须带 WS_THICKFRAME），它不受 WM_NCHITTEST 控制，
            //   所以只能把它的宽度压成 0：边框不存在了，
            //   光标在边框上就不会变，只剩四个角的感应区。
            //
            //   安全性：contentPt() 是动态算内缩的（ClientToScreen(0,0)-原点），
            //   内缩变 0 后它自动跟着变 0，点击坐标不会偏。
            if (w) return 0;      // wParam=TRUE：客户区 = 整个窗口
            break;
        }

        case WM_NCHITTEST: {
            // 拖边框改大小 + 拖标题区移动。
            //
            // ⚠ 分层窗口（WS_EX_LAYERED）没有非客户区，系统不会自动给边框。
            //   要自己判坐标返回 HTxxx，让系统按标准拖拽逻辑处理。
            //
            //   好处：拖到屏幕边缘自动吸附、双击标题栏最大化这类系统行为
            //   都能白拿，不用自己实现。
            //
            //   感应区放宽到 8px —— 6px 时不太好抓，
            //   尤其是 150% DPI 下 6 物理像素只有 4 逻辑像素。
            // ⚠ 这里**不能**用 contentPt！
            //   WM_NCHITTEST 的 lParam 是**屏幕坐标**（不是客户区坐标），
            //   而下面比较用的 wr 也是屏幕坐标 —— 两者本来就同一坐标系，
            //   再补那 10px 反而会把边框感应区整体挪偏。
            POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT wr{};
            ::GetWindowRect(h, &wr);

            // ★ 感应区贴着**内容**的四角，不是窗口外框 ——
            //   窗口比内容大出一圈外阴影留白，用外框判的话，
            //   用户对着看得见的角去拖，反而落在感应区外面（正好差一圈留白）。
            const int shd = islandShadowPx();
            const int cl = wr.left + shd, ct = wr.top + shd;
            const int crr = wr.right - shd, cbb = wr.bottom - shd;
            const int edge = px(16);   // 四角感应区 24 物理像素（原来 8px 太难抓）
            const bool left = pt.x < cl + edge;
            const bool right = pt.x >= crr - edge;
            const bool top = pt.y < ct + edge;
            const bool bottom = pt.y >= cbb - edge;

            // 调试：记录一次命中，确认 WM_NCHITTEST 确实被调用了。
            // 只在真的命中边框时打，避免鼠标划过刷屏。
            if (left || right || top || bottom) {
                static DWORD lastTick = 0;
                const DWORD now = ::GetTickCount();
                if (now - lastTick > 500) {
                    logf("island: NCHITTEST 命中边框 窗口(%d,%d,%d,%d) 光标(%d,%d) L%d R%d T%d B%d",
                         wr.left, wr.top, wr.right, wr.bottom, pt.x, pt.y,
                         (int)left, (int)right, (int)top, (int)bottom);
                    lastTick = now;
                }
            }

            // ★ 只留**四个角**改大小（拖一个角，上下左右都能改）。
            //
            //   用户要求：“拖动一个角的时候上下左右移动的去改面板大小，
            //   删除拉边框更改的操作”。
            //
            //   所以四条**边**（左/右/上/下）不再返回 HTLEFT 等，
            //   让它们落到下面的处理：上边一条仍然当拖动柄（HTCAPTION），
            //   其余当客户区（HTCLIENT）。
            if (bottom && right) return HTBOTTOMRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (top && right) return HTTOPRIGHT;
            if (top && left) return HTTOPLEFT;

            // 顶部那条（搜索框上方、关闭按钮左边）当拖动柄。
            // ⚠ 不能整个顶部都算标题区 —— 那样点搜索框会被当成拖窗口。
            //   只在搜索框**上面**那条窄带算。
            if (pt.y < ct + px(6) && pt.x < crr - px(40)) return HTCAPTION;

            return HTCLIENT;
        }

        case WM_ENTERSIZEMOVE: {
            // ★ 拖动/缩放开始：进入"实时渲染"模式。
            //
            //   ⚠ 这里**只起定时器**，别的什么都不做。
            //
            //     三条死路都试过了，别再走：
            //       ① WDA_EXCLUDEFROMCAPTURE 想让面板"透出后面" → 抓出来纯黑；
            //       ② 这里 ShowWindow(h, SW_HIDE) → 系统拖动循环被中止，面板不动；
            //       ③ 这里抓整屏（2560x1600）→ 太重，把拖动循环堵死（"拖动不了了"）。
            //
            //     正解：这里什么都不碰，把"抓背景"挪到定时器里，
            //     而且只抓面板那一块、不带 CAPTUREBLT（见 refreshBackdropWhileDragging）。
            // ★ 拖动中**不做任何刷新**。
            //
            //   用户反馈：“拖动的时候，面板有频闪，不要连带面板一起刷新”。
            //   确实如此：每 100ms 重烘焙一遍、再
            //   UpdateLayeredWindow 推一帧，而系统同时在移窗口 ——
            //   两边一叠就是频闪。
            //
            //   现在：拖动中完全不碰（不重烘焙、不重绘），
            //   只让系统移动窗口；松手后在 WM_EXITSIZEMOVE 里刷新一次。
            dragLive_ = true;
            return 0;
        }

        case WM_EXITSIZEMOVE: {
            // 拖动/缩放结束 —— 这时才记录最终尺寸，
            // 拖动过程中每次都写配置会把磁盘刷爆。
            RECT wr{};
            ::GetWindowRect(h, &wr);
            // ★ 窗口矩形 = 内容 + 四周外阴影留白 → 先换算回内容尺寸/位置
            const int shd = islandShadowPx();
            const int nw = (wr.right - wr.left) - shd * 2;
            const int nh = (wr.bottom - wr.top) - shd * 2;
            const int nxp = wr.left + shd, nyp = wr.top + shd;
            if (nw != w_ || nh != h_ || nxp != x_ || nyp != y_) {
                w_ = nw; h_ = nh;
                x_ = nxp; y_ = nyp;
                if (cfg_) {
                    cfg_->islandW = w_;
                    cfg_->islandH = h_;
                    cfg_->islandX = x_;
                    cfg_->islandY = y_;
                }
                layout();
                layoutSearchEdit();     // 面板缩放了，搜索框要跟着
                resolveTextColors();
                // 尺寸变了要重新抓背景重烘焙
                RECT rc = {x_, y_, x_ + w_, y_ + h_};
                Backdrop bd;
                if (Surface::capture(rc, 0, bd)) backdrop_ = bd;
                baseValid_ = false;
                redraw();
                if (onConfigDirty) onConfigDirty();
                logf("island: 尺寸/位置已更新 %dx%d @ (%d,%d)", w_, h_, x_, y_);
            }

            // 拖动结束：退出"实时渲染"模式，抓一次干净的收尾
            if (dragLive_) {
                dragLive_ = false;
                // ★ 只重烘焙背景，**不重抓、不藏窗口**。
                //
                //   用户说的就是这一下：“之前拖完只有面板背景刷新，
                //   现在面板被刷新了”。
                //   原来这里调 refreshBackdropLive()（藏窗口→抓屏→显示→整块重绘），
                //   拖完就像重新打开一次。现在只重烘焙已有底图。
                // ★ 拖动/缩放结束：重抓背景 + 重烘焙 + 重绘。
                //
                //   用户要求：“希望跟着新位置更新，改大小之后背景也要更新”。
                //   （拖动中仍然一次都不刷新，避免频闪；只在松手后刷一次）
                // ⚠ 上面写着"只重烘焙、不重抓、不藏窗口"，但这里原来调的是
                //   refreshBackdropLive() —— 它会藏窗口再抓屏再显示，
                //   所以松手那一下面板看起来被关掉又打开。
                //   现在就在原地重烘焙一次，窗口一次都不藏。
                baseValid_ = false;
                redraw();
layoutSearchEdit();     // 搜索框位置跟上就行
            }
            return 0;
        }

        case WM_WINDOWPOSCHANGED: {
            // ⚠ 用 WM_WINDOWPOSCHANGED 而不是 WM_SIZE。
            //
            //   实测：分层窗口被系统拖动改尺寸时，
            //   WM_SIZE **不一定发**（日志里完全没有），
            //   但 WM_WINDOWPOSCHANGED 一定会来 ——
            //   它是"窗口矩形变了"的通用通知。
            //
            //   不处理这个消息的话，表现就是
            //   "能拖动改大小，但松手后又被画回原尺寸，配置也不保存"。
            auto wp = reinterpret_cast<WINDOWPOS*>(l);
            if (wp && !(wp->flags & SWP_NOSIZE)) {
                const int nw = wp->cx;
                const int nh = wp->cy;
                if (nw > 0 && nh > 0 && (nw != w_ || nh != h_)) {
                    w_ = nw;
                    h_ = nh;
                }
            }
            if (wp && !(wp->flags & SWP_NOMOVE)) {
                x_ = wp->x;
                y_ = wp->y;
            }

            layout();
            layoutSearchEdit();     // 面板动了，搜索框要跟着
            baseValid_ = false;
            // ★ 拖动中**不重绘面板**。
                //
                //   每移动一步都会来 WM_WINDOWPOSCHANGED，
                //   这里原来直接 redraw()（重烘焙+重推 ULW 帧）——
                //   跟系统的移窗叠在一起就是用户说的“面板被刷新的感觉”。
                //   拖动中只让系统移动窗口（分层窗口的已推表面会跟着走），
                //   松手后 WM_EXITSIZEMOVE 里刷新一次。
                if (!dragLive_) redraw();

            // 记忆尺寸/位置，用一个一次性定时器延迟落盘
            // （拖动中会连续触发，每次都写盘会把磁盘刷爆）
            //
            // ⚠⚠ 只有面板**真的显示着**才允许写配置。
            //
            //   踩过的坑（审计发现，并且实测复现过）：
            //     窗口创建时是个占位矩形（kMinW x kMinH = 420x320 @ 0,0），
            //     CreateWindowEx 之后系统会**立刻**发来 WM_WINDOWPOSCHANGED ——
            //     于是那个占位矩形被当成"用户尺寸"写进了配置，
            //     把用户真正调好的 1305x791 覆盖成 420x320 @ (0,0)。
            //     下次打开面板就又小又挤到左上角。
            //
            //     hide() 里我加过一道 visible_ 守卫，但挡不住这条路径 ——
            //     创建时的这个消息在 hide() 之前就到了。
            if (visible_) {
                if (cfg_) {
                    cfg_->islandW = w_;
                    cfg_->islandH = h_;
                    cfg_->islandX = x_;
                    cfg_->islandY = y_;
                }
                ::SetTimer(h, kSaveTimerId, 400, nullptr);
            }

            // 不 return —— 交给 DefWindowProc 做它该做的
            break;
        }

        case WM_TIMER: {
            // 展开动画：每帧重画，进度由 redraw 的提交处按时间算
            if (w == kGrowTimerId) {
                if (!growActive_) {
                    ::KillTimer(h, kGrowTimerId);
                    return 0;
                }
                redraw();
                return 0;
            }
            // ── 长按检测 ──
            //
            // ⚠ 必须在定时器里做，不能只在 WM_MOUSEMOVE ——
            //   按住不动时系统不发鼠标移动消息，
            //   在移动里判断的话静止按住永远进不了拖动状态。
            // ── 拖动让位的缓动推进 ──
            //
            // ⚠ 必须有这个定时器，否则 dragShiftT_ 永远停在 0，
            //   其它图标就不会"挤过去"，等于没有动画。
            //   16ms ≈ 60fps，和显示刷新对齐。
            if (w == kDragAnimTimerId) {
                // ⚠ 跑完不要 KillTimer。
                //
                //   拖动过程中目标格会反复变化，每次都重新 SetTimer
                //   既啰嗦又容易漏。让定时器一直跑着，
                //   进度到 1 就空转（每帧只是判断一下，代价可忽略）。
                //   松开鼠标时才真正 Kill（见 WM_LBUTTONUP）。
                if (dragCell_ < 0) {
                    // 没在拖了，停掉
                    ::KillTimer(h, kDragAnimTimerId);
                    return 0;
                }
                // ★ 每帧推进每格的缓动，再重画。
                //   stepCellAnim 内部按真实 dt 算缓动系数（时间驱动），
                //   所以 WM_TIMER 抖动 / 掉帧都不会让动画忽快忽慢。
                stepCellAnim();
                redraw();
                return 0;
            }

            if (w == kDragTimerId) {
                if (dragArmed_ && dragCell_ < 0) {
                    const DWORD held = ::GetTickCount() - dragStartTick_;
                    if (held >= kDragHoldMs) {
                        // ★ 到点必须确认**左键此刻仍按着**。
                        //
                        //   踩过的坑（用户："有时候点开面板的时候，
                        //   它会自动拖动一个快捷方式"）：
                        //   只要上一次按下的 WM_LBUTTONUP 丢了（点到别的窗口、
                        //   被系统吞掉、面板刚打开时就松了手），
                        //   dragArmed_ 就会一直留着 ——
                        //   长按定时器一到，就凭空抓起一个格子开始拖。
                        if ((::GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                            dragArmed_ = false;
                            ::KillTimer(h, kDragTimerId);
                            logf("island: 长按到点但左键已松开（残留 armed）→ 取消拖动");
                            return 0;
                        }
                        dragCell_ = pressedCell_;
                        dragTarget_ = dragCell_;
                        cellAnimX_.clear();     // 进入拖动：每格缓动由 redraw 惰性初始化
                        logf("island: 长按 %lums，抓起第 %d 个快捷方式",
                             (unsigned long)held, dragCell_);
                        // ⚠ 这里就把动画定时器起起来，让它一直在跑。
                        //
                        //   原来只在"目标格变化"时 SetTimer ——
                        //   但跟手需要每帧 redraw，那之后定时器可能已经
                        //   因为跑完而 KillTimer 了，于是拖到新格子时
                        //   动画不启动，看不到让位过程。
                        ::SetTimer(h, kDragAnimTimerId, 16, nullptr);
                        redraw();
                    }
                } else if (!dragArmed_) {
                    // 已经松开或不在拖动流程，定时器该停了
                    ::KillTimer(h, kDragTimerId);
                }
                return 0;
            }

            if (w == kIslandSearchTimerId) {
                ::KillTimer(h, kIslandSearchTimerId);
                runIslandSearch();
                return 0;
            }

            if (w == kSaveTimerId) {
                ::KillTimer(h, kSaveTimerId);

                // ⚠ 同样要有 visible_ 守卫（理由见 WM_WINDOWPOSCHANGED 那段）：
                //   面板没显示时窗口上那个矩形是占位值，不是用户尺寸。
                if (!visible_) return 0;

                // ⚠ 保存前再从窗口读一次真实矩形。
                //
                //   不能只信 w_/h_ —— 拖动结束时最后那次变化
                //   可能没触发 WM_WINDOWPOSCHANGED（消息被合并），
                //   于是 w_/h_ 停在中间值上。
                //   直接问系统要当前矩形最可靠。
                RECT wr{};
                if (::GetWindowRect(h, &wr)) {
                    // ★ 窗口矩形 = 内容 + 留白 → 换算回内容几何再记
                    const int shd = islandShadowPx();
                    const int nw = (wr.right - wr.left) - shd * 2;
                    const int nh = (wr.bottom - wr.top) - shd * 2;
                    if (nw > 0 && nh > 0) {
                        w_ = nw;
                        h_ = nh;
                        x_ = wr.left + shd;
                        y_ = wr.top + shd;
                    }
                }

                if (cfg_) {
                    cfg_->islandW = w_;
                    cfg_->islandH = h_;
                    cfg_->islandX = x_;
                    cfg_->islandY = y_;
                }
                if (onConfigDirty) onConfigDirty();
                logf("island: 尺寸/位置已保存 %dx%d @ (%d,%d)", w_, h_, x_, y_);
                return 0;
            }
            break;
        }

        case WM_SIZING: {
            // 夹住最小尺寸，防止把面板拖成一条缝找不回来
            RECT* r = reinterpret_cast<RECT*>(l);
            if (r->right - r->left < kMinW + islandShadowPx() * 2) {   // 夹的是内容尺寸，窗口另加四周留白
                if (w == WMSZ_LEFT || w == WMSZ_TOPLEFT || w == WMSZ_BOTTOMLEFT) {
                    r->left = r->right - (kMinW + islandShadowPx() * 2);
                } else {
                    r->right = r->left + (kMinW + islandShadowPx() * 2);
                }
            }
            if (r->bottom - r->top < kMinH + islandShadowPx() * 2) {
                if (w == WMSZ_TOP || w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT) {
                    r->top = r->bottom - (kMinH + islandShadowPx() * 2);
                } else {
                    r->bottom = r->top + (kMinH + islandShadowPx() * 2);
                }
            }

            // ⚠ 关键：拖动过程中就同步内部尺寸并重绘。
            //
            //   分层窗口有个坑：窗口矩形由系统改（SetWindowPos），
            //   但**画出来的大小**由 UpdateLayeredWindow 的 psize 决定。
            //   如果只在拖动结束时更新 w_/h_，拖动全程画面都停在旧尺寸，
            //   看起来就像"拖不动"。
            //
            //   在这里实时同步，拖的时候就能看到面板跟着变。
            //
            //   不写配置（拖动中会连续触发），持久化交给 WM_EXITSIZEMOVE。
            const int nw = (r->right - r->left) - islandShadowPx() * 2;
            const int nh = (r->bottom - r->top) - islandShadowPx() * 2;
            if (nw > 0 && nh > 0 && (nw != w_ || nh != h_)) {
                w_ = nw;
                h_ = nh;
                layout();
                baseValid_ = false;
                redraw();
            }
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            const POINT cp = contentPt(h, l);
            const int mx = cp.x;
            const int my = cp.y;

            // ── 长按拖动 ──
            //
            // 按住不放超过 kDragHoldMs 就进入拖动状态。
            // 之后鼠标移到哪个格子，哪个格子就高亮，松开即换位。
            //
            // ⚠ 为什么用"长按"而不是直接拖：
            //   单击是"启动程序"。如果直接拖，手抖一两像素就会
            //   把图标挪走而不是打开程序，很难用。
            //   加个时间门槛，两个动作就分得干干净净。
            if (dragArmed_ && dragCell_ < 0) {
                const DWORD held = ::GetTickCount() - dragStartTick_;
                // 也允许"移动了一小段"就判定为拖动意图 ——
                // 有些用户习惯按住就走，不等延迟。
                const int movedX = std::abs(mx - dragStartPt_.x);
                const int movedY = std::abs(my - dragStartPt_.y);
                const bool moved = (movedX > px(6) || movedY > px(6));

                if (held >= kDragHoldMs || (held >= 150 && moved)) {
                    dragCell_ = pressedCell_;
                    dragTarget_ = dragCell_;
                    logf("island: 进入拖动状态，抓起第 %d 个快捷方式", dragCell_);
                    redraw();
                }
            }

            if (dragCell_ >= 0) {
                // 拖动中：记住鼠标位置（被拖的图标要跟手），
                // 并更新目标格（其它图标按它让位）。
                dragCurPt_ = {mx, my};
                const int t = cellAt(mx, my);
                if (t != dragTarget_) {
                    dragTarget_ = t;
                    // ★ 目标格变了：**不需要重置任何进度**。
                    //   每格独立缓动（stepCellAnim）会自己朝新目标继续滑 ——
                    //   以前这里把全局进度清零，已经让开的格子会先弹回原位，
                    //   就是"一顿一顿"的来源（用户："要顺滑地推动"）。
                }
                redraw();       // 每帧重画，图标才会跟着鼠标走
                return 0;       // 拖动时不更新 hover
            }

            int newHotCat = categoryAt(mx, my);
            int newHotCell = cellAt(mx, my);

            if (newHotCat != hotCategory_ || newHotCell != hotCell_) {
                hotCategory_ = newHotCat;
                hotCell_ = newHotCell;
                redraw();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT cp = contentPt(h, l);
            const int mx = cp.x;
            const int my = cp.y;

            // （原来这里处理 × 关闭按钮的点击 —— 已删除）

            // （搜索框不在这里处理：它整个就是那个搬过来的 EDIT 窗口，
            //   点击直接落在 EDIT 上，由它自己拿焦点。见 layoutSearchEdit。）

            // 搜索态：点结果行直接打开（打开后 hide()）
            if (searchTyping_) {
                const int row = resultRowAt(mx, my);
                if (row >= 0) {
                    openResult(row, false);
                    return 0;
                }
            }

            // 用户按钮
            if (mx >= userBtn_.left && mx < userBtn_.right &&
                my >= userBtn_.top && my < userBtn_.bottom) {
                showUserMenu();
                return 0;
            }

            // 电源按钮
            if (mx >= powerBtn_.left && mx < powerBtn_.right &&
                my >= powerBtn_.top && my < powerBtn_.bottom) {
                showPowerMenu();
                return 0;
            }

            // 搜索态时分类栏和网格都没画，别让点击误触到它们
            if (searchTyping_) return 0;

            // 分类
            const int cat = categoryAt(mx, my);
            if (cat >= 0 && cfg_) {
                cfg_->islandCategory = categories_[cat].id;
                scrollRow_ = 0;
                hotCell_ = -1;
                baseValid_ = false;
                redraw();
                if (onConfigDirty) onConfigDirty();
                logf("island: 切到分类 %s", cfg_->islandCategory.c_str());
                return 0;
            }

            // ── 网格：按下时不启动，只记下来 ──
            //
            // ⚠ 原来是"按下就启动"，那样和长按拖动冲突 ——
            //   想拖图标换位，一按下去程序就启动了。
            //
            //   改成按下只记录，等松开（WM_LBUTTONUP）再决定：
            //     · 没拖动过 → 启动
            //     · 拖动过   → 换位（已经在移动时做掉了）
            //
            //   这样做还有个附带好处：鼠标按错了可以拖出去再松开取消，
            //   和系统图标的手感一致。
            const int cell = cellAt(mx, my);
            // 诊断：报告点击处的格子判定结果和网格几何 ——
            // 用来确认点击坐标算得对不对（DPI 换算容易出错）。
            logf("island: 按下 (%d,%d) -> 格子 %d  网格原点(%d,%d) 格尺寸 %dx%d",
                 mx, my, cell, gridX_, gridY_, cellW_, cellH_);
            if (cell >= 0) {
                pressedCell_ = cell;
                dragCell_ = -1;
                dragArmed_ = true;              // 已按下，等长按计时
                dragStartTick_ = ::GetTickCount();
                dragStartPt_ = {mx, my};
                dragCurPt_ = {mx, my};
                dragTarget_ = -1;
                cellAnimX_.clear();
                cellAnimY_.clear();
                ::SetCapture(h);                 // 抓住鼠标，移出窗口也能收到消息

                // ⚠ 起定时器来判断长按。
                //
                //   不能只在 WM_MOUSEMOVE 里判断 —— 按住不动时
                //   系统不发移动消息，永远进不了拖动状态。
                ::SetTimer(h, kDragTimerId, 200, nullptr);
                return 0;
            }

            // 拖窗口（在标题区以外的空白处按下）
            ::SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT cp = contentPt(h, l);
            const int mx = cp.x;
            const int my = cp.y;
            if (::GetCapture() == h) ::ReleaseCapture();
            ::KillTimer(h, kDragTimerId);       // 不再需要长按检测
            ::KillTimer(h, kDragAnimTimerId);   // 让位动画也停

            // 正在拖动 → 落到鼠标所在的格子上
            if (dragCell_ >= 0) {
                const int target = (dragTarget_ >= 0) ? dragTarget_ : cellAt(mx, my);
                if (target >= 0) moveShortcut(dragCell_, target);
                dragCell_ = -1;
                dragArmed_ = false;
                dragTarget_ = -1;
                pressedCell_ = -1;
                redraw();
                return 0;
            }

            // 没拖动 → 当单击处理：启动了才算数，而且要在同一个格子上
            if (pressedCell_ >= 0) {
                const int hit = cellAt(mx, my);
                if (hit == pressedCell_ && onLaunchShortcut) {
                    onLaunchShortcut(pressedCell_);
                }
                pressedCell_ = -1;
                dragArmed_ = false;
                return 0;
            }

            pressedCell_ = -1;
            dragArmed_ = false;
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt = contentPt(h, l);
            ::ClientToScreen(h, &pt);
            const POINT cp2 = contentPt(h, l);
            const int cell = cellAt(cp2.x, cp2.y);
            showContextMenu(pt.x, pt.y, cell);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            // ⚠ 搜索框**没有底** —— 这是用户明确要求的：
            //     "我只是让你把那个底给删了，不用再去绕开呀"。
            //
            //   所以这里返回 NULL_BRUSH（不填背景）+ 透明背景模式：
            //   面板的玻璃直接透上来，只有用户打的字浮在上面。
            //   （窗口的圆角区域保留 —— 那是 14:41 那版就有的形状，
            //     用户的要求是"只删底"，不是改形状。）
            // ⚠ 必须给一个**深色背景色**，否则会白底白字。
            //
            //   踩过的坑：把 SetBkColor 去掉之后，DC 的背景色退回默认的**白色**，
            //   而文字色是白的 —— 于是凡是"按背景色画"的地方
            //   （输入法组合串、选中高亮）都变成一块白底，白字盖上去看不见，
            //   表现就是输入框里出现一个白块（用户："文字不要提亮，仅保留文字"）。
            //
            //   SetBkMode(TRANSPARENT) + NULL_BRUSH 保证**正常打字**时没有底色；
            //   SetBkColor 只影响组合/选中那类绘制。
            HDC dc = (HDC)w;
            ::SetTextColor(dc, RGB(rOf(textMain_), gOf(textMain_), bOf(textMain_)));   // 跟随文字颜色设置
            ::SetBkColor(dc, RGB(rOf(searchBgCol_), gOf(searchBgCol_), bOf(searchBgCol_)));
            if (!searchBgBrush_) {
                searchBgBrush_ = ::CreateSolidBrush(
                    RGB(rOf(searchBgCol_), gOf(searchBgCol_), bOf(searchBgCol_)));
            }
            return (LRESULT)searchBgBrush_;
        }

        case WM_SHOWWINDOW: {
            // ★ 面板显示/隐藏时，**同步**那个独立顶层输入框。
            //
            //   踩过的坑：输入框是独立窗口（分层窗口渲染不了子控件），
            //   ShowWindow(面板, SW_HIDE) 不会连带隐藏它。
            //   而它带着和提亮条一样的底色 ——
            //   面板一收，屏幕上就留下一条"没收回去的背景条"。
            if (searchEdit_) {
                if (w) {
                    layoutSearchEdit();
                    ::ShowWindow(searchEdit_, SW_SHOWNOACTIVATE);
                } else {
                    ::ShowWindow(searchEdit_, SW_HIDE);
                }
            }
            return 0;
        }

        case WM_COMMAND: {
            // 输入框内容变了 → 防抖查询。
            // ⚠ 必须防抖：不防抖就是每敲一个键全表扫一遍（索引 3 万多条）。
            if (HIWORD(w) == EN_CHANGE && (HWND)l == searchEdit_) {
                ::SetTimer(h, kIslandSearchTimerId, 60, nullptr);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // 滚轮翻页（网格是一页一页翻，不是逐行滚）
            const int delta = GET_WHEEL_DELTA_WPARAM(w);
            if (searchTyping_) {
                moveResultSel(delta > 0 ? -3 : 3);
                return 0;
            }
            if (delta > 0) {
                if (scrollRow_ > 0) { --scrollRow_; redraw(); }
            } else {
                if (scrollRow_ < maxScrollRow_) { ++scrollRow_; redraw(); }
            }
            return 0;
        }

        case WM_KEYDOWN: {
            // 岛台不再自己收输入（搜索交给 HotrainySearch 窗口），
            // 这里只保留 Esc = 收起面板。
            if (w == VK_ESCAPE) {
                hide();
                return 0;
            }
            return 0;
        }

        case WM_SETCURSOR: {
            // ★ 删掉了“靠近边框就显示缩放光标”的手写逻辑。
            //
            //   用户要求：“鼠标移动到岛台面板边框的时候光标不变”。
            //   这里原来不管 WM_NCHITTEST 返回什么，只要光标在边框
            //   kResizeBorder 范围内就强行 SetCursor(IDC_SIZENWSE) ——
            //   所以之前改 WM_NCHITTEST / WM_NCCALCSIZE 都没用，真正的光标是这里定的。
            //
            //   现在交给默认处理：客户区用类光标（箭头），
            //   四角因为 WM_NCHITTEST 返回了 HTxxx，系统会自己给缩放光标。
            return ::DefWindowProcW(h, msg, w, l);
        }

        case WM_ACTIVATE: {
            // 面板不因失焦而关闭 —— 用户明确要求"不因鼠标离开而关闭"。
            return 0;
        }

        case WM_CLOSE:
            hide();
            return 0;
    }
    return ::DefWindowProcW(h, msg, w, l);
}

}  // namespace hr