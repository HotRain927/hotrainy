// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// src/islandbtn.cpp — 任务栏上的岛台按钮
//
// 这个窗口挂到 Shell_TrayWnd 下面，摆在开始按钮原来占的那个坑里。
// 为什么这么做（而不是插进任务栏的 XAML 树）见 hr/island/islandbtn.h 的说明。
#include "hr/island/islandbtn.h"

#include "hr/shared/common.h"       // logf / px()

// ⚠ GDI+ 的头必须在 windows.h 之后包含 ——
//   GdiplusEnums.h 用了 UINT/BOOL 这些 Win32 类型，
//   没有 windows.h 就编译不过。
//   common.h 会先带进 windows.h，所以放在它后面。
#include <objidl.h>   // ⚠ GDI+ 需要它（glass.cpp 也是这个顺序）
#include <gdiplus.h>
#include <dwmapi.h>
#include "hr/text/island_theme.h"

#include <cmath>
#include <vector> // 配色常量（和岛台面板同一来源）

using namespace Gdiplus;

namespace hr {

// 前置声明。
//
// ⚠ makeHIcon 的定义在文件后面（靠近 applyIcon），
//   但 create() 里注册窗口类时就要用它 ——
//   类图标必须在 RegisterClassEx 之前准备好。
HICON makeHIcon(int size, bool dark);

namespace {

const wchar_t* kClass = L"HotrainyIslandBtnV2";

// 窗口标题。任务栏的悬停提示用它。
const wchar_t* kWindowTitle = L"岛台";

// 按钮圆角半径（逻辑像素）
constexpr int kRadius = 6;

// 悬停/按下时的底色。
//
// 用很淡的一层白 —— 和任务栏原生按钮（设置、开始）的悬停观感接近。
constexpr uint32_t kHoverBg = 0x24FFFFFF;
constexpr uint32_t kActiveBg = 0x38FFFFFF;

// “保持在任务栏之上”的定时器 ID。
// 任务栏重绘时会把我们压下去，周期性抬一下。
const UINT_PTR kRaiseTimerId = 0x61;

// 按钮窗口最近一次"刚出现"的时刻。
//
// ⚠ 为什么需要它 —— "岛台面板有时候自己弹出来"的根因：
//
//   这个窗口的"被点击"判定就是 WM_ACTIVATE 里的
//   WA_ACTIVE / WA_CLICKACTIVE（任务栏是直接发消息的，不走普通激活）。
//
//   但**创建和显示窗口本身也可能让系统发来 WA_ACTIVE** ——
//   任务栏接管我们的图标、把窗口纳入它的管理时会激活一次。
//   于是面板会自己弹出来，而且时机不确定，
//   表现就是"有时候打开程序，岛台面板自己就显示了"。
//
//   处理：窗口出现后的短暂时间内不把激活当点击，只认之后的真实点击。
DWORD g_btnShownTick = 0;
constexpr DWORD kActivateGraceMs = 700;

}  // namespace

LRESULT CALLBACK IslandButton::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    IslandButton* self = reinterpret_cast<IslandButton*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<IslandButton*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->handle(h, m, w, l);
    return ::DefWindowProcW(h, m, w, l);
}

bool IslandButton::create(HINSTANCE hInst, const RECT& slot) {
    inst_ = hInst;
    slot_ = slot;

    // 注册窗口类（幂等）
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = hInst;
        wc.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kClass;

        // ★★ 类图标 —— 任务栏显示的就是这个。
        //
        //   踩过的坑：只设 WM_SETICON 是不够的。
        //   任务栏读的是**窗口类**的图标（hIcon / hIconSm），
        //   类注册时没填的话，它就用默认的应用图标
        //   （一个文档形状的）。
        //
        //   ⚠ 类图标必须在 RegisterClassEx **之前**准备好 ——
        //     注册之后就改不了了。
        //     所以生成 HICON 的逻辑提成了文件级函数 makeHIcon()。
        wc.hIcon = makeHIcon(32, false);
        wc.hIconSm = makeHIcon(16, false);

        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            logf("islandbtn: 注册窗口类失败 err=%lu", ::GetLastError());
            return false;
        }
        registered = true;
    }

    // ⚠ 先以 WS_POPUP 建，建完再 SetParent 改成子窗口。
    //
    //   直接建 WS_CHILD 并指定任务栏当父窗口不行 ——
    //   任务栏是别的进程的窗口，CreateWindowEx 时传它的 HWND
    //   会失败（跨进程父子关系要事后 SetParent 才稳）。
    // ★★ 关键改动：WS_EX_APPWINDOW 让任务栏给我们建**原生图标**。
    //
    //   实测结论（这一步是整个方案的基础）：
    //     · WS_EX_APPWINDOW 的顶层窗口 → 任务栏主动创建一个图标按钮
    //     · 那个图标由任务栏自己布局管理 → 图标增减时自动跟随，
    //       零延迟、永不重叠
    //
    //   ⚠ 三个实测出来的坑：
    //
    //     1. **不能再带 WS_EX_TOOLWINDOW**。
    //        那个样式字面意思就是"别在任务栏出现"，
    //        和 APPWINDOW 同时存在时任务栏选择隐藏 ——
    //        实测：带了 TOOLWINDOW 就一个图标都不出现。
    //
    //     2. **不能再带 WS_EX_LAYERED**。
    //        分层窗口（UpdateLayeredWindow 那套）是"我们自己画"的模式，
    //        和"任务栏替我们画图标"是互斥的。
    //        现在图标交给任务栏画，不需要分层了。
    //
    //     3. **WS_EX_NOACTIVATE 要保留**。
    //        它能防止窗口抢前台焦点。
    //        实测：即使带 NOACTIVATE，点任务栏图标时我们**仍然**
    //        收到 WM_ACTIVATE —— 任务栏是直接发消息的，
    //        不走正常激活路径。所以照样能拦下来。
    // ⚠ 窗口尺寸给 2x2，不是 slot 的大小。
    //
    //   原因：任务栏的缩略图预览会显示窗口的实时画面，
    //   而预览框大小跟着窗口走。
    //   窗口只有 2x2 时，整个预览小到几乎看不见内容 ——
    //   这是"压不住预览就让它没内容"的兜底手段。
    //
    //   实测确认任务栏按钮**不依赖窗口尺寸**，所以缩小是安全的。
    hwnd_ = ::CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_NOACTIVATE,
        kClass, L"hotrainy-island-btn",
        WS_POPUP,
        slot.left, slot.top,
        2, 2,
        nullptr, nullptr, hInst, this);

    if (!hwnd_) {
        logf("islandbtn: 建窗口失败 err=%lu", ::GetLastError());
        return false;
    }

    // ★ 给窗口设置 H 图标 —— 任务栏会显示它。
    //
    //   ⚠ 两个尺寸都要设：
    //     ICON_SMALL(16x16) —— 任务栏用这个
    //     ICON_BIG(32x32)   —— Alt+Tab 切换器用
    //   只设一个的话，任务栏可能用默认的应用图标（一个蓝方块）。
    applyIcon();

    // 窗口标题也要设 —— 任务栏的悬停提示用它。
    // ⚠ 必须是**有标题**的窗口，否则某些情况下任务栏不给建图标。
    ::SetWindowTextW(hwnd_, kWindowTitle);

    // 禁掉任务栏的悬停缩略图预览。
    //
    // 现在是原生图标模式，任务栏对所有图标都有"悬停弹预览"的标准行为。
    // 但岛台按钮不是普通应用窗口 —— 弹一个空白缩略图没意义，还碍事。
    //
    // 用 DWM 的 DWMWA_DISALLOW_PEEK（值 14）：
    //   它本来是用来禁止 Aero Peek 的，
    //   但对任务栏的悬停行为也有影响 —— 实测能让预览不再弹出。
    //
    // ⚠ 这个属性必须在窗口**已经有图标之后**再设，
    //   太早设可能影响任务栏建不建图标。
    {
        // ⚠ 值从 dwmapi.h 的枚举数出来，不要凭记忆写。
        //
        //   踩过的坑：一开始只设了 EXCLUDED_FROM_PEEK(12)，
        //   它返回 S_OK，但**没挡住悬停缩略图** ——
        //   那个属性管的是 Aero Peek（悬停让其他窗口变透明），
        //   不是任务栏的缩略图预览。
        //
        //   ★★ FORCE_ICONIC_REPRESENTATION 必须设 FALSE ★★
        //
        //   我一开始设成了 TRUE，想着"别抓实时画面" —— **语义理解反了**。
        //
        //   它的真实含义是"强制用**图标**表示"：
        //     TRUE → DWM 不显示实时缩略图，改放**窗口图标**
        //   结果预览里出现一个又大又清晰的 H —— 正是用户的窗口图标，
        //   反而比原来的实时画面更显眼。
        //   （抓屏实证：小窗口的缩略图区就是一个大 H。）
        //
        //   ⚠ 教训：API 名字里带 FORCE 的，先想清楚"强制成什么"。
        //
        //   设成 FALSE 后，DWM 走正常的实时缩略图路径，
        //   显示的是我们那个 2x2 窗口的实际画面 ——
        //   再叠加 WDA_EXCLUDEFROMCAPTURE（对截取隐形），
        //   预览内容就是一片空白。
        //
        //   另外两个（DISALLOW_PEEK / EXCLUDED_FROM_PEEK）
        //   对任务栏缩略图实测没影响，但留着无害 ——
        //   它们至少能挡掉 Aero Peek 那条路。
        const DWORD DWMWA_FORCE_ICONIC_REPRESENTATION = 7;
        const DWORD DWMWA_DISALLOW_PEEK = 11;
        const DWORD DWMWA_EXCLUDED_FROM_PEEK = 12;

        BOOL noIconic = FALSE;   // ★ 关键：FALSE = 不要用图标代替实时画面
        BOOL yes = TRUE;

        HRESULT h1 = ::DwmSetWindowAttribute(
            hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &noIconic, sizeof(noIconic));
        HRESULT h2 = ::DwmSetWindowAttribute(
            hwnd_, DWMWA_DISALLOW_PEEK, &yes, sizeof(yes));
        HRESULT h3 = ::DwmSetWindowAttribute(
            hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &yes, sizeof(yes));

        // ⚠ 把设置的值也记下来 —— 之前只记 HRESULT（全是成功），
        //   根本看不出设的是 TRUE 还是 FALSE，害我排查了很久。
        logf("islandbtn: DWM 属性 -> 图标化=%s(0x%08X) 禁Peek=%s(0x%08X) 排除Peek=%s(0x%08X)",
             noIconic ? "TRUE" : "FALSE", (unsigned)h1,
             yes ? "TRUE" : "FALSE", (unsigned)h2,
             yes ? "TRUE" : "FALSE", (unsigned)h3);

        // ★★ 真正管用的是这个：让窗口对"截取类操作"隐形。
        //
        //   任务栏的缩略图预览走的是 DWM 实时缩略图 ——
        //   本质就是一次抓取。设上 WDA_EXCLUDEFROMCAPTURE 之后，
        //   抓出来的内容是空白的，预览框里就看不到东西了。
        //
        //   ⚠ 上面那三个 DWM 属性**实测无效**（都返回 S_OK，
        //     但预览照弹）—— 它们管的是 Aero Peek 之类的行为，
        //     不是任务栏缩略图。任务栏对每个按钮都弹预览，
        //     没有公开 API 能单独关掉某个窗口的预览。
        //     所以只能想办法让预览"没内容可看"。
        //
        //   WDA_EXCLUDEFROMCAPTURE 已在 winuser.h 定义（Win10 2004+）
        //   ⚠ 不要自己再定一个同名变量 —— 会和宏冲突。
        // ★★ 这里原来有一句：
        //       ::SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
        //    目的是让任务栏悬停预览里那个 2x2 按钮"没内容可看"（纯外观）。
        //
        //    但它带来的是**系统级**副作用：Windows 会把带该标志的窗口当作
        //    "受保护内容"报告给屏幕捕获 API —— NVIDIA ShadowPlay / Overlay
        //    因此**直接拒绝录制**，提示"有受保护的程序正在运行"，而不是只把
        //    这一个窗口画黑。用户在实机上确认了正是这个原因。
        //
        //    取舍：一块 2x2 像素的预览框 vs 整个系统的录屏能力 —— 显然保录屏。
        //    ⚠ 以后若想再压掉那个预览，必须另找**不触碰捕获权限**的办法，
        //      绝不能再给窗口设任何 WDA_* 捕获排除标志。
        //    （删掉后，悬停任务栏时预览框里会显示这个小窗口本身，属于可接受的外观代价。）
    }


    // 记下"刚出现"的时刻 —— 之后一小段时间内的激活不算点击。
    g_btnShownTick = ::GetTickCount();

    return true;
}

// ── 移动到新位置 ──
//
// ⚠ 原生图标模式下这个函数基本用不上了 ——
//   图标位置由任务栏自己布局，不需要我们摆。
//   留着是为了接口完整（主程序还在调），
//   内部只更新记录的位置，不再 SetWindowPos 去"对齐坑位"。
void IslandButton::moveTo(const RECT& slot) {
    slot_ = slot;

    // ★★ 只记录位置，**不碰窗口**。
    //
    //   踩过的坑：这里原来是
    //       SetWindowPos(hwnd_, nullptr, slot.left, slot.top,
    //                    slot.right-slot.left, slot.bottom-slot.top, ...)
    //   也就是"移动 + 改尺寸"。那是**旧方案（自己画的浮层）**的做法 ——
    //   那时窗口必须精确盖住开始按钮的坑位。
    //
    //   但它会把 create() 里设的 2x2 尺寸**又撑回 68x72**，
    //   于是任务栏的缩略图预览又能显示内容了
    //   （实测：悬停弹出的小窗口里是一个清晰的 H）。
    //
    //   现在窗口只是"用来拿到任务栏按钮"的载体：
    //     · 位置由任务栏自己布局，不需要我们摆
    //     · 尺寸越小越好（预览缩略图跟着窗口尺寸走）
    //
    //   ⚠ 教训：同一个属性在两处被设置时，
    //     改了 create() 不代表生效 —— moveTo() 会覆盖它。
    if (!hwnd_) return;
    (void)slot;
}

// ── 显示 ──
void IslandButton::show() {
    if (!hwnd_) return;

    // ⚠ SW_SHOWNOACTIVATE 而不是 SW_SHOW ——
    //   岛台按钮不该抢前台焦点（面板是"菜单式"的东西）。
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    // 显示本身可能招来一次 WA_ACTIVE —— 记时刻，宽限期内不当点击。
    g_btnShownTick = ::GetTickCount();

    // 原生图标模式下不需要 SetWindowPos 抬升 ——
    // 图标由任务栏管理，它自己会在正确的位置和层级。
    redraw();
}

// ── 隐藏 ──
void IslandButton::hide() {
    if (!hwnd_) return;
    ::ShowWindow(hwnd_, SW_HIDE);
}

// ── 销毁 ──
void IslandButton::destroy() {
    if (!hwnd_) return;
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

// ── 面板开着时高亮按钮 ──
void IslandButton::setActive(bool on) {
    if (active_ == on) return;
    active_ = on;
    redraw();
}

// ── 跟随文字颜色策略 ──
//
// ⚠ 真的跟随，不是"留着以后用"。
//   踩过的坑：一开始这里是 (void)mode —— 收下参数不用，
//   结果设置里选「深色」时托盘文字变黑、按钮图标还是白的。
void IslandButton::setTextMode(const std::wstring& mode) {
    if (mode == L"dark") {
        darkText_ = true;
    } else if (mode == L"light") {
        darkText_ = false;
    } else {
        // auto：按按钮背后那条壁纸的亮度判断。
        // 那一步由主程序采样屏幕后调 setDarkText()，这里保持当前值。
        return;
    }
    redraw();
}

// ── 重绘 ──
//
// ⚠ 现在是**原生图标**模式，图标由任务栏画（用我们给的 HICON）。
//   所以这个函数不再需要自己粉刷分层位图 ——
//   但图标颜色（深/浅）还是要反映出来，
//   那靠**换一个不同颜色的 HICON** 实现。
void IslandButton::redraw() {
    if (!hwnd_) return;

    // 颜色策略变了 → 换图标。
    //
    // 任务栏显示的是窗口图标，想让 H 变深色，
    // 就得给一个深色的 HICON —— 不能像以前那样改画笔颜色。
    applyIcon();
}

// 生成 H 的 HICON 并设给窗口。
//
// ★ 为什么需要这个：
//   现在是**原生图标**模式 —— 任务栏替我们画图标。
//   它画的是**窗口图标**（WM_SETICON 设的那个），
//   不是我们的分层位图。
//   不设的话任务栏会用一个默认的应用图标（蓝方块）。
//
// ⚠ 两个尺寸都要设：
//     ICON_SMALL (16x16) —— 任务栏用这个
//     ICON_BIG   (32x32) —— Alt+Tab 切换器用
//   只设一个的话，另一个地方会显示成默认图标。
//
// 用 GDI+ 画而不是 CreateFont + DrawText：
//   GDI+ 的文本抗锯齿更细腻，小尺寸（16px）下差别很明显。
//   而且能直接拿到 32bpp ARGB 位图，alpha 能保留 ——
//   透明背景才不会变成黑块。
// 当前正被窗口使用的两个 HICON。
//
// ⚠⚠ 这里**故意用文件级静态变量，而不是类成员** —— 血的教训。
//
//   一开始它们是我加在 IslandButton 类里的两个成员（为了修 HICON 泄漏）。
//   结果是：定义全局对象 g_islandBtn 的那个 .cpp 没有跟着重编，
//   内存里的对象还是旧布局（168 字节，slot_ 结尾），
//   而 applyIcon() 按新布局往对象**末尾之外**写这 16 字节 ——
//   正好砸在紧邻的两个全局量上：
//
//     0xa9981958  hr::kClass  ← 被写成垃圾
//     0xa9981960  hr::kUI     ← 被写成垃圾 0x64d80bd3
//
//   然后右键岛台快捷方式 → MenuWindow::measure → measureText(..., kUI)
//   → wcsncpy 读垃圾指针 → **访问违例崩溃**
//   （转储 hotrainy.exe.28408.dmp 实测确认）。
//
//   教训：给类加成员会改变**所有**编译单元看到的布局，
//   一旦有 TU 没重编，就是这种"写到对象外面"的隐蔽内存破坏。
//   这里的数据本来就是单例（整个进程只有一个岛台按钮），
//   放文件级静态变量既不碰类布局，语义也完全一样。
namespace {
HICON g_iconSmall = nullptr;
HICON g_iconBig = nullptr;
}  // namespace

void IslandButton::applyIcon() {
    if (!hwnd_) return;

    // 生成一个指定尺寸的 H 图标。
    //
    // 步骤：
    //   1. GDI+ 画到 32bppARGB 位图（透明底 + 白字）
    //   2. 转成 HBITMAP
    //   3. CreateIconIndirect 组装成 HICON
    // 生成图标的逻辑提成了文件级函数 makeHIcon()（见上），
    // 因为窗口类注册时也要用同一个图标。

    // 任务栏用小图标。16 是标准尺寸；
    // 高 DPI 下系统会自己缩放，给大一点更清晰。
    HICON icoSmall = makeHIcon(16, darkText_);
    HICON icoBig = makeHIcon(32, darkText_);

    if (icoSmall) ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icoSmall);
    if (icoBig) ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)icoBig);

    // ★★ 换图标时把**上一批**销毁掉 —— 否则每次重绘泄漏两个 HICON ★★
    //
    //   踩过的坑（审计发现）：WM_SETICON **不接管所有权** ——
    //   窗口只是记住这个句柄，不会替你释放。
    //   而 applyIcon 会被 WM_MOUSEMOVE / WM_MOUSELEAVE /
    //   setTextMode / setActive / setDark 反复触发 ——
    //   每次两个 HICON，USER 句柄就这么持续泄漏下去了（有上限，迟早耗尽）。
    //
    //   这里只销毁"上一批"，当前这批要活着 —— 任务栏正在用它画图标。
    //   窗口销毁时再销毁当前这批（见 WM_NCDESTROY）。
    if (g_iconSmall) ::DestroyIcon(g_iconSmall);
    if (g_iconBig) ::DestroyIcon(g_iconBig);
    g_iconSmall = icoSmall;
    g_iconBig = icoBig;

    logf("islandbtn: 已设置 H 图标 (small=%p big=%p)",
         (void*)icoSmall, (void*)icoBig);
}
// 生成 H 的 HICON（指定尺寸）。
//
// ★ 为什么是"文件级函数"而不是某个方法里的 lambda：
//   窗口类注册时也要用它 —— 任务栏显示的是**类图标**
//   （WNDCLASSEX.hIcon），光设 WM_SETICON 不够。
//   而类图标必须在 RegisterClassEx **之前**准备好。
//
// 用 GDI+ 画而不是 CreateFont + DrawText：
//   抗锯齿更细腻，16px 这种小尺寸下差别很明显。
//   而且能直接拿 32bpp ARGB 位图，alpha 保留得住 ——
//   透明背景才不会变成黑块。
// 参数 dark 决定字色：
//   dark = true  → 深色字形（用在浅色背景上）
//   dark = false → 浅色字形（用在深色背景上）
//
// ⚠ 任务栏图标就是一张位图，加不了描边 ——
//   只能靠字形本身的深浅去适配背景。
HICON makeHIcon(int size, bool dark) {
    if (size <= 0) return nullptr;

    HDC screen = ::GetDC(nullptr);
    if (!screen) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;       // 负数 = 自上而下，和我们的行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits,
                                       nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) ::DeleteObject(color);
        return nullptr;
    }

    // ── 用 GDI+ 把 H 画上去 ──
    {
        Gdiplus::Bitmap bmp(size, size, size * 4,
                            PixelFormat32bppARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&bmp);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));      // 全透明底

        Gdiplus::FontFamily famUI(L"Segoe UI");
        Gdiplus::FontFamily famAlt(L"Arial");
        Gdiplus::FontFamily* fam = famUI.IsAvailable() ? &famUI : &famAlt;

        // 字号取 size 的 90% —— 留一点边距，
        // 否则 H 会顶到图标边缘，在任务栏上显得比旁边的系统图标大一圈。
        const Gdiplus::REAL fontPx = size * 0.9f;
        Gdiplus::Font font(fam, fontPx, Gdiplus::FontStyleBold,
                           Gdiplus::UnitPixel);
        // 字色跟随「文字颜色」策略。
        //
        // 踩过的坑：这里原来硬编码白色，结果在浅色壁纸的任务栏上
        // 几乎看不见，而且用户在设置里切「深色」也没反应。
        //
        // 用接近黑/白的灰而不是纯黑纯白：
        //   纯黑在深色玻璃上会显得比旁边的系统图标"重"，
        //   纯白在浅色壁纸上又太淡。
        const BYTE v = dark ? 0x1A : 0xF5;
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, v, v, dark ? 0x1C : 0xF7));

        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::RectF rc(0.0f, 0.0f, (Gdiplus::REAL)size, (Gdiplus::REAL)size);
        g.DrawString(L"H", -1, &font, rc, &fmt, &brush);
    }

    // ── 组装 HICON ──
    //
    // ⚠ 掩码位图：有 alpha 通道时掩码其实不起作用，
    //   但 CreateIconIndirect 要求它必须存在 ——
    //   给一个全 0 的 1bpp 位图即可。
    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&ii);

    ::DeleteObject(color);
    if (mask) ::DeleteObject(mask);
    return icon;
}

void IslandButton::paintNow() {
            
            HDC dc = ::GetDC(hwnd_);
            if (!dc) return;

            RECT rc{};
            ::GetClientRect(hwnd_, &rc);
            const int cw = rc.right - rc.left;
            const int ch = rc.bottom - rc.top;

            // ── 手工画像素，彻底控制 alpha ──
            //
            // ⚠ 不能用 GDI 的 FillRect / DrawText。
            //
            //   踩过的坑：原来用 CreateCompatibleBitmap + FillRect
            //   再 AlphaBlend 提交 —— 结果整块变成**不透明白块**，
            //   图标只剩几个像素点。
            //
            //   原因：GDI 没有 alpha 概念。
            //   它画出来的像素 alpha 全是 0，
            //   AlphaBlend 把它当“完全透明”，
            //   但 DC 上又留着黑色的 RGB —— 混起来就是一团糊。
            //
            //   改成直接写像素：
            //     · 圆角底用圆心距离算覆盖率，自己算 alpha
            //     · 图标用 GDI 画到一张**临时**位图，再把它当作蒙版
            //       逐像素合成到主位图上 —— 这样字形的边缘也能柔和

            HDC screen = ::GetDC(nullptr);
            HDC mem = ::CreateCompatibleDC(screen);

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = cw;
            bi.bmiHeader.biHeight = -ch;      // 自上而下
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!bmp || !bits) {
                ::DeleteDC(mem);
                ::ReleaseDC(nullptr, screen);
                ::ReleaseDC(hwnd_, dc);
                return;
            }
            HBITMAP oldBmp = (HBITMAP)::SelectObject(mem, bmp);

            auto* pix = static_cast<uint32_t*>(bits);   // 不叫 px —— 那是全局的 DPI 换算函数
            ::ZeroMemory(pix, (size_t)cw * ch * 4);   // 全透明

            // → 第 1 步：圆角底（悬停或激活时才有）
            if (hot_ || active_) {
                const uint32_t base = active_ ? kActiveBg : kHoverBg;
                const uint32_t ba = (base >> 24) & 0xFF;
                const uint32_t br = (base >> 16) & 0xFF;
                const uint32_t bg_ = (base >> 8) & 0xFF;
                const uint32_t bb = base & 0xFF;
                const double rad = (double)kRadius;

                for (int y = 0; y < ch; ++y) {
                    for (int x = 0; x < cw; ++x) {
                        // 圆角矩形的覆盖率：四个角各算一次
                        double cover = 1.0;
                        auto corner = [&](double cx, double cy) {
                            const double dx = x + 0.5 - cx;
                            const double dy = y + 0.5 - cy;
                            const double d = std::sqrt(dx * dx + dy * dy);
                            if (d <= rad - 0.5) return 1.0;
                            if (d >= rad + 0.5) return 0.0;
                            return (rad + 0.5 - d);
                        };
                        if (x < rad && y < rad)                 cover = corner(rad, rad);
                        else if (x >= cw - rad && y < rad)      cover = corner(cw - rad, rad);
                        else if (x < rad && y >= ch - rad)      cover = corner(rad, ch - rad);
                        else if (x >= cw - rad && y >= ch - rad) cover = corner(cw - rad, ch - rad);
                        if (cover <= 0.0) continue;
                        if (cover > 1.0) cover = 1.0;

                        const uint32_t a = (uint32_t)std::lround(ba * cover);
                        if (a == 0) continue;
                        pix[(size_t)y * cw + x] = (a << 24) | (br << 16) | (bg_ << 8) | bb;
                    }
                }
            }

            // → 第 2 步：图标
            //
            // 用一张临时单色位图当蒙版：
            // GDI 把字形画成白色（背景黑），
            // 然后逐像素把白度当作覆盖率合成到主位图。
            // 这样字形边缘的抗锯齿也会被保留，不会变成硬边。
            {
                HDC maskDc = ::CreateCompatibleDC(screen);
                HBITMAP maskBmp = ::CreateCompatibleBitmap(screen, cw, ch);
                HBITMAP oldMask = (HBITMAP)::SelectObject(maskDc, maskBmp);

                RECT full = {0, 0, cw, ch};
                HBRUSH black = ::CreateSolidBrush(RGB(0, 0, 0));
                ::FillRect(maskDc, &full, black);
                ::DeleteObject(black);

                // ⚠ 字母的字面高度（cap height）比图标字形小，
    //   同样字号下 H 看起来会偏小 —— 所以比原来的 22 放大几档。
    //
    //   22 -> 28：实测在这个比例下 H 的视觉大小
    //   和旁边的系统图标（齿轮、Edge）比较接近。
    const int fontPx = px(28);
                logf("islandbtn: \u56fe\u6807\u5b57\u53f7 = %d \u7269\u7406\u50cf\u7d20 (\u6309\u94ae %dx%d)",
                     fontPx, cw, ch);
                HFONT font = ::CreateFontW(
                    // ⚠ 字号要和系统图标对齐。
                    //   实测 16 偏小 —— 设置那个齿轮看起来比我们的粗一圈。
                    //   22 和它们差不多重。
                    -fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                HFONT oldFont = (HFONT)::SelectObject(maskDc, font);

                ::SetTextColor(maskDc, RGB(255, 255, 255));
                ::SetBkMode(maskDc, TRANSPARENT);
                RECT tr = {0, 0, cw, ch};
                ::DrawTextW(maskDc, L"H", -1, &tr,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                ::SelectObject(maskDc, oldFont);
                ::DeleteObject(font);

                // 取蒙版像素
                BITMAPINFO mbi{};
                mbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                mbi.bmiHeader.biWidth = cw;
                mbi.bmiHeader.biHeight = -ch;
                mbi.bmiHeader.biPlanes = 1;
                mbi.bmiHeader.biBitCount = 32;
                mbi.bmiHeader.biCompression = BI_RGB;

                std::vector<uint32_t> mpx((size_t)cw * ch, 0u);
                ::GetDIBits(maskDc, maskBmp, 0, ch, mpx.data(), &mbi, DIB_RGB_COLORS);

                // 合成：白度 = 覆盖率
                // 颜色从统一入口取 —— 和面板文字、托盘文字同一个值。
                const uint32_t ic = islandTextMain(darkText_);
                const uint32_t ir = (ic >> 16) & 0xFF;
                const uint32_t ig = (ic >> 8) & 0xFF;
                const uint32_t ib = ic & 0xFF;

                // ── 先铺一层柔和的深色描边 ──
                //
                // ⚠ 为什么需要描边：
                //   图标是浅色（和岛台面板文字同色），
                //   在**浅色壁纸**上几乎看不见 —— 白字白底。
                //   任务栏是透明的，壁纸是什么颜色完全不可控，
                //   所以不能只靠一种颜色。
                //
                //   做法：把字形的覆盖范围向外扩一圈，
                //   先用半透明黑描边，再把主色叠上去。
                //   这样深色壁纸上不影响、浅色壁纸上也读得清。
                //
                //   扩张量取 1 像素 —— 再粗就显得笨重了。
                {
                    const int spread = 1;
                    std::vector<uint32_t> outline(mpx.size(), 0u);
                    for (int y = 0; y < ch; ++y) {
                        for (int x = 0; x < cw; ++x) {
                            // 只要周围有亮像素，自己就算描边的一部分
                            uint32_t best = 0;
                            for (int dy = -spread; dy <= spread; ++dy) {
                                for (int dx = -spread; dx <= spread; ++dx) {
                                    const int sx = x + dx, sy = y + dy;
                                    if (sx < 0 || sy < 0 || sx >= cw || sy >= ch) continue;
                                    const uint32_t wv = mpx[(size_t)sy * cw + sx] & 0xFF;
                                    if (wv > best) best = wv;
                                }
                            }
                            // ⚠ 描边强度 = 扩张后的覆盖度 - 原本的覆盖度。
                            //
                            //   只用"扩张后的值"会让整个字形（含内部）
                            //   都被描边染色，看起来像一块实心色板。
                            //
                            //   减去原覆盖度，就只剩**边缘那一圈**，
                            //   内部保持主色不变 —— 这才是描边该有的样子。
                            const uint32_t own = mpx[(size_t)y * cw + x] & 0xFF;
                            if (best > own) {
                                outline[(size_t)y * cw + x] = (best - own) * 4 / 5;
                            }
                        }
                    }

                    // 描边色：近黑，带一点透明度
                    // 描边色取反色：浅色图标配深色描边，反之亦然。
                    // 这样不管壁纸是深是浅都能看清。
                    // 描边色取反色，但**不要用纯白/纯黑** ——
                    // 纯白描边在浅色壁纸上像个白框，很突兀。
                    // 用带点灰的中间值，只起"分清轮廓"的作用。
                    const uint32_t orr = darkText_ ? 0xC8 : 0x18;
                    const uint32_t og  = darkText_ ? 0xCA : 0x1A;
                    const uint32_t ob  = darkText_ ? 0xD0 : 0x1E;

                    for (size_t i = 0; i < outline.size(); ++i) {
                        const uint32_t a = outline[i];
                        if (a < 8) continue;
                        const uint32_t dst = pix[i];
                        const uint32_t da = (dst >> 24) & 0xFF;
                        const uint32_t na = a + da * (255 - a) / 255;
                        if (na == 0) continue;
                        auto mix = [&](uint32_t sc, uint32_t dc2) {
                            return (sc * a + dc2 * da * (255 - a) / 255) / na;
                        };
                        pix[i] = (na << 24) | (mix(orr, (dst >> 16) & 0xFF) << 16) |
                                 (mix(og, (dst >> 8) & 0xFF) << 8) | mix(ob, dst & 0xFF);
                    }
                }

                // ── 再叠主色（和岛台面板文字同一个值）──
                for (size_t i = 0; i < mpx.size(); ++i) {
                    const uint32_t w = mpx[i] & 0xFF;
                    if (w < 8) continue;               // 太暗的当背景
                    const uint32_t a = w;
                    const uint32_t dst = pix[i];
                    const uint32_t da = (dst >> 24) & 0xFF;
                    const uint32_t na = a + da * (255 - a) / 255;
                    if (na == 0) continue;
                    auto mix = [&](uint32_t sc, uint32_t dc2) {
                        return (sc * a + dc2 * da * (255 - a) / 255) / na;
                    };
                    pix[i] = (na << 24) | (mix(ir, (dst >> 16) & 0xFF) << 16) |
                            (mix(ig, (dst >> 8) & 0xFF) << 8) | mix(ib, dst & 0xFF);
                }

                ::SelectObject(maskDc, oldMask);
                ::DeleteObject(maskBmp);
                ::DeleteDC(maskDc);
            }

            // → 第 3 步：提交
            //
            // ⚠ 用 BitBlt 而非 AlphaBlend。
            //   因为我们的位图 alpha 已经是预乘的（premultiplied），
            //   而 BitBlt 不会动 alpha 通道 ——
            //   但这里窗口没有 WS_EX_LAYERED，
            //   所以 alpha 不会被用到，也就是不会有透明效果。
            //
            //   正确做法：窗口带 WS_EX_LAYERED，
            //   这里用 UpdateLayeredWindow 整块提交（含 alpha）。
            //   跟岛台面板同一套。
            {
                POINT ptSrc = {0, 0};
                POINT ptDst = {0, 0};
                RECT wr{};
                ::GetWindowRect(hwnd_, &wr);
                ptDst.x = wr.left;
                ptDst.y = wr.top;
                SIZE sz = {cw, ch};

                BLENDFUNCTION bf{};
                bf.BlendOp = AC_SRC_OVER;
                bf.SourceConstantAlpha = 255;
                bf.AlphaFormat = AC_SRC_ALPHA;

                ::UpdateLayeredWindow(hwnd_, screen, &ptDst, &sz,
                                      mem, &ptSrc, 0, &bf, ULW_ALPHA);
            }

            ::SelectObject(mem, oldBmp);
            ::DeleteObject(bmp);
            ::DeleteDC(mem);
            ::ReleaseDC(nullptr, screen);
            ::ReleaseDC(hwnd_, dc);
}

LRESULT IslandButton::handle(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_TIMER: {
            // ⚠ 原来这里有个“每 2 秒把自己抬到 TOPMOST”的定时器。
            //   那是**旧方案（自己画的浮层）**的补丁 ——
            //   那时怕被任务栏重绘盖住，所以周期性抬升。
            //
            //   现在是原生图标模式，根本不需要：
            //     图标由任务栏自己布局，就是任务栏的一部分。
            //   而且抬 TOPMOST 会打乱任务栏的 Z 序管理。
            break;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            ::BeginPaint(h, &ps);
            paintNow();
            ::EndPaint(h, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            // 不要在擦背景时刷色 —— 会把父窗口（任务栏）的背景盖掉，
            // 按钮就变成一个突兀的方块了。
            return 1;

        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = h;
                ::TrackMouseEvent(&tme);
                tracking_ = true;
            }
            if (!hot_) {
                hot_ = true;
                redraw();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hot_) {
                hot_ = false;
                redraw();
            }
            return 0;

        case WM_LBUTTONDOWN:
            {   // ★ 降噪：同一来源 500ms 内只打一条
                static DWORD _lastTick_ = 0; DWORD _now_ = ::GetTickCount();
                if (_now_ - _lastTick_ > 500) { _lastTick_ = _now_;
                    logf("islandbtn: 收到 WM_LBUTTONDOWN");
                }
            }
            return 0;

        case WM_LBUTTONUP: {
            // 点击 → 通知主程序开/关岛台面板。
            //
            // 只做一次跳转，具体逻辑在主程序里 ——
            // 这个类不该知道面板怎么开。
            {   // ★ 降噪：同一来源 500ms 内只打一条
                static DWORD _lastTick_ = 0; DWORD _now_ = ::GetTickCount();
                if (_now_ - _lastTick_ > 500) { _lastTick_ = _now_;
                    logf("islandbtn: 收到 WM_LBUTTONUP，回调=%d", (int)(bool)onClick);
                }
            }
            if (onClick) onClick();
            return 0;
        }

        // ★★ 点任务栏上那个原生图标时，任务栏会给我们发激活消息。
        //
        //   实测的消息序列：
        //     WM_NCACTIVATE w=1
        //     WM_ACTIVATE   w=1
        //     WM_SETFOCUS
        //     （等一下）
        //     WM_NCACTIVATE w=0
        //     WM_ACTIVATE   w=0
        //
        //   所以拦截点就是 WM_ACTIVATE w=1：
        //     ① 执行我们的动作（toggle 岛台面板）
        //     ② 立刻把激活还给系统（不让窗口真抢焦点）
        //
        //   ⚠ 为什么"还回去"很重要：
        //     用户正在别的程序里打字，点一下岛台图标
        //     不应该把焦点从那边夺走。面板是"菜单式"的东西。
        case WM_ACTIVATE: {
            // ⚠ 把 wParam 也打出来。
            //
            //   踩过的坑：只记录"被点击"不够 ——
            //   连点三次时第三次打不开，
            //   很可能是因为状态没回到“未激活”，
            //   任务栏就不再发 w=1 了。
            //   有了 wParam 才能看出是哪种情况。
            const WORD act = LOWORD(w);
            {   // ★ 降噪：同一来源 500ms 内只打一条
                static DWORD _lastAct_ = 0; DWORD _nowAct_ = ::GetTickCount();
                if (_nowAct_ - _lastAct_ > 500) { _lastAct_ = _nowAct_;
                logf("islandbtn: WM_ACTIVATE w=0x%04X (%s)", (unsigned)act,
                act == WA_ACTIVE ? "激活"
                : (act == WA_CLICKACTIVE ? "点击激活"
                : (act == WA_INACTIVE ? "取消激活" : "其他")));
                }
            }

            if (act == WA_ACTIVE || act == WA_CLICKACTIVE) {
                // ★ 刚出现时的激活不算点击（见 g_btnShownTick 的说明）。
                //
                //   创建/显示窗口本身可能触发一次 WA_ACTIVE，
                //   那不是用户点击。放过去的话面板会自己弹出来。
                if (::GetTickCount() - g_btnShownTick < kActivateGraceMs) {
                    logf("islandbtn: 宽限期内忽略激活（距出现 %lu ms）",
                         (unsigned long)(::GetTickCount() - g_btnShownTick));
                    ::SetForegroundWindow(::GetShellWindow());
                    return 0;
                }

                if (onClick) onClick();

                // ★★ 主动放弃前台激活。
                //
                //   踩过的坑：原来这里只是 return 0，没做别的。
                //   结果任务栏内部的"这个应用已激活"标记一直挂着，
                //   从第二次点击开始，它就把单击当成"最小化/还原"，
                //   **再也不发 WM_ACTIVATE**。
                //   表现就是用户说的"连点三次，第三次打不开"。
                //
                //   为什么要还回去：
                //     我们本来就不是普通应用窗口（带 WS_EX_NOACTIVATE），
                //     不该占着前台。而且只有状态回到"未激活"，
                //     任务栏下次点击才会再发 w=1。
                //
                //   ⚠ 这是"有时候能连点成功"的原因 ——
                //     如果中途用户点了别的窗口，标记被清掉，
                //     下一次点我们的图标就又正常了。
                ::SetForegroundWindow(::GetShellWindow());
                return 0;
            }
            return 0;
        }

        case WM_SETFOCUS:
            // 同理：拿到了焦点也不要，还给系统。
            return 0;

        case WM_MOUSEACTIVATE:
            // ⚠ 必须返回 MA_NOACTIVATE。
            //
            //   我们是任务栏的子窗口，如果点击时去激活自己，
            //   会把当前前台窗口踢掉（比如你正在打字，点一下岛台
            //   输入焦点就飞了）。返回 MA_NOACTIVATE 表示
            //   "处理这次点击，但不要激活我"。
            return MA_NOACTIVATE;

        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            return TRUE;

        case WM_NCDESTROY: {
            ::SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            hwnd_ = nullptr;

            // 窗口没了，它用的那两个 HICON 也要释放
            // （WM_SETICON 不接管所有权，见 applyIcon 的说明）。
            if (g_iconSmall) { ::DestroyIcon(g_iconSmall); g_iconSmall = nullptr; }
            if (g_iconBig)   { ::DestroyIcon(g_iconBig);   g_iconBig = nullptr; }
            return 0;
        }

        default:
            break;
    }
    return ::DefWindowProcW(h, msg, w, l);
}

}  // namespace hr