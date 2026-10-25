// hr/bar.cpp — 对照 ui/tab_bar.py
#include "hr/bar.h"

#include <algorithm>
#include <cmath>
#include <shellapi.h>

#include "hr/common.h"
#include "hr/theme.h"

namespace hr {

namespace {
IslandEnableHook g_islandHook = nullptr;
IslandTextHook g_islandTextHook = nullptr;
IslandApplyStateHook g_islandApplyStateHook = nullptr;
}

void setIslandEnableHook(IslandEnableHook hook) {
    g_islandHook = hook;
}

void islandSetEnabled(bool on) {
    if (g_islandHook) g_islandHook(on);
}

void setIslandApplyStateHook(IslandApplyStateHook hook) {
    g_islandApplyStateHook = hook;
}

void islandApplyState() {
    if (g_islandApplyStateHook) g_islandApplyStateHook();
}

void setIslandTextHook(IslandTextHook hook) {
    g_islandTextHook = hook;
}

void islandSetTextMode(const std::wstring& mode) {
    if (g_islandTextHook) g_islandTextHook(mode.c_str());
}

namespace {

const wchar_t* kBarClass = L"HotrainyBar";
const UINT_PTR kHoverTimerId = 0xB1;

// 右侧图标字形（Segoe UI Symbol）
const wchar_t* kGlyphs[BarWindow::ICON_COUNT] = {L"✂", L"🗑", L"🖥", L"⚙"};

int measureW(const std::wstring& text, int fontPx) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfHeight = -(LONG)fontPx;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, L"Microsoft YaHei", 31);
    HFONT f = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    DeleteObject(f);
    ReleaseDC(nullptr, dc);
    return (int)sz.cx;
}

}  // namespace

bool BarWindow::create(HINSTANCE hInst, Config* cfg) {
    hinst_ = hInst;
    cfg_ = cfg;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BarWindow::proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBarClass;
    if (!RegisterClassExW(&wc)) {
        logf("bar: RegisterClass 失败 %lu", (unsigned long)GetLastError());
        return false;
    }

    POINT origin = {0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    width_ = mi.rcMonitor.right - mi.rcMonitor.left;

    height_ = px(24);  // 对照 tab_bar.py: _tab_height = px(24)
    iconW_ = px(24);
    // 先按未启用岛台算；setIslandEnabled 之后会重算
    islandEnabled_ = cfg && cfg->islandEnabled;
    recomputeIconArea();

    // ⚠ 创建时就放在屏幕外（y = 顶边 - 高度），不要放在 y=0。
    //
    //   踩过的坑：原来创建在 y=0 并且立刻 ShowWindow ——
    //   于是启动瞬间标签栏是**可见的**，要等第一个 autoHideTick
    //   （最多 120ms）才收起。用户看到的就是
    //   "标签栏有时候自己冒出来一下"。
    //
    //   现在直接以"已收起"的状态出生：位置在屏幕外、
    //   visible_=false、slidingTo_ 也是收起的值，
    //   之后由 autoHideTick 决定要不要唤出。
    const int hiddenY = mi.rcMonitor.top - height_;

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            kBarClass, L"hotrainy", WS_POPUP, mi.rcMonitor.left, hiddenY,
                            width_, height_, nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        logf("bar: CreateWindowEx 失败 %lu", (unsigned long)GetLastError());
        return false;
    }

    relayout();
    refreshBackdrop(true);
    redraw();

    barY_ = hiddenY;
    slidingTo_ = hiddenY;
    visible_ = false;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    place(hiddenY);

    // 关掉自动隐藏时，标签栏是常驻的 —— 这种情况下出生就显示。
    if (!cfg || !cfg->autoHide) showBar();

    logf("bar: 创建 %dx%d 标签宽 %d 图标区从 %d 起（初始收起=%d）",
         width_, height_, tabW_, iconsStart_, (int)!visible_);
    return true;
}

void BarWindow::destroy() {
    if (hoverTimer_) KillTimer(hwnd_, kHoverTimerId);
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

// 图标区起点。
//
// ⚠ 岛台不再占标签栏的一格 —— 它的按钮在系统任务栏上。
//   所以这里始终按 4 个图标算，不随 islandEnabled_ 变化。
void BarWindow::recomputeIconArea() {
    iconsStart_ = width_ - ICON_COUNT * iconW_;
}

void BarWindow::relayout() {
    if (!cfg_) return;
    tabRects_.clear();

    // 岛台按钮不在标签栏上（在系统任务栏里），所以这里恒为空。
    // 保留这个字段是为了 islandButtonRect() 的接口不变。
    islandRect_ = {0, 0, 0, 0};

    int n = (int)cfg_->tabs.size();
    if (n <= 0 || iconsStart_ <= 0) {
        tabW_ = 0;
        return;
    }
    // 所有标签等分"屏宽 − 右侧图标区"，保证彼此 1:1
    tabW_ = iconsStart_ / n;
    for (int i = 0; i < n; ++i) {
        RECT r = {i * tabW_, 0, (i + 1) * tabW_, height_};
        tabRects_.push_back(r);
    }
}

RECT BarWindow::islandButtonRect() const {
    if (!islandEnabled_ || !hwnd_) {
        RECT z = {0, 0, 0, 0};
        return z;
    }
    RECT r = islandRect_;
    POINT tl = {r.left, r.top};
    POINT br = {r.right, r.bottom};
    ::ClientToScreen(hwnd_, &tl);
    ::ClientToScreen(hwnd_, &br);
    RECT out = {tl.x, tl.y, br.x, br.y};
    return out;
}

void BarWindow::setIslandEnabled(bool on) {
    if (islandEnabled_ == on) return;
    islandEnabled_ = on;
    // 图标区整体左移一格，标签条相应变短
    recomputeIconArea();
    relayout();
    rebuildBase();
    redraw();
    logf("bar: 岛台按钮 %s", on ? "已启用" : "已关闭");
}

void BarWindow::setIslandOpen(bool open) {
    if (islandOpen_ == open) return;
    islandOpen_ = open;
    redraw();
}

RECT BarWindow::tabRect(int idx) const {
    if (idx < 0 || idx >= (int)tabRects_.size()) {
        RECT z = {0, 0, 0, 0};
        return z;
    }
    return tabRects_[idx];
}

int BarWindow::tabAt(int x) const {
    if (x < 0 || x >= iconsStart_ || tabW_ <= 0) return -1;
    int idx = x / tabW_;
    return (idx >= 0 && idx < (int)tabRects_.size()) ? idx : -1;
}

int BarWindow::iconAt(int x) const {
    // 岛台按钮占最左一格，系统图标整体右移一格
    // ⚠ base 不能加 islandEnabled_ 的偏移。
    //
    //   踩过的坑：岛台按钮原来占图标区最左一格，
    //   所以判定时要 +iconW_ 跳过它。
    //   后来岛台按钮挪到系统任务栏上了，绘制那边改了（sysIconBase = iconsStart_），
    //   但这里忘了跟着改 —— 结果"图标画在左边、点击判定在右边"，
    //   点上去总是偏一格。
    //
    //   现在绘制和判定都用 iconsStart_，两边对齐。
    const int base = iconsStart_;
    if (x < base) return -1;
    int idx = (x - base) / iconW_;
    return (idx >= 0 && idx < ICON_COUNT) ? idx : -1;
}

// 岛台按钮是否被点中。
//
// ⚠ 始终返回 false —— 标签栏上不再有岛台按钮。
//   真正的按钮插在系统任务栏的 XAML 树里，点击由 explorer 侧的
//   Tapped 回调处理（见 visualtree.cpp 的 g_islandTapped）。
bool BarWindow::islandAt(int x) const {
    (void)x;
    return false;
}

void BarWindow::setActive(int idx) {
    if (active_ == idx) return;
    active_ = idx;
    redraw();
}

void BarWindow::setDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    rebuildBase();
    redraw();
}

void BarWindow::clearHoverIfMouseAway() {
    if (hover_ < 0) return;
    POINT cp{};
    ::GetCursorPos(&cp);
    RECT bwr{};
    ::GetWindowRect(hwnd_, &bwr);
    if (!::PtInRect(&bwr, cp)) {
        logf("bar: 鼠标已离开标签栏 -> 清悬停 %d", hover_);
        hover_ = -1;
        redraw();
    }
}

void BarWindow::setPanelOpen(bool open) {
    panelOpen_ = open;
    if (!open && hover_ >= 0) {
        hover_ = -1;
        redraw();
    }
}

void BarWindow::setMenuOpen(bool open) { menuOpen_ = open; }

void BarWindow::onConfigChanged() {
    relayout();
    if (cfg_ && active_ >= (int)cfg_->tabs.size()) active_ = (int)cfg_->tabs.size() - 1;
    redraw();
}

void BarWindow::refreshBackdrop(bool force) {
    if (!hwnd_) return;
    RECT wr = {};
    GetWindowRect(hwnd_, &wr);
    if (wr.bottom - wr.top <= 0) return;
    Backdrop bd;
    // 抓标签栏正下方同样高度的一条：模糊后与正后方几乎无差，
    // 又不会把自己正在显示的这帧抓进去（Python 版同样的做法）。
    if (!Surface::capture(wr, height_, bd)) return;
    if (!force && bd.sig == backdrop_.sig) return;
    backdrop_ = bd;
    rebuildBase();
}

void BarWindow::tickBackdrop() {
    // main.py:520 `_refresh_bar_backdrop` 开头就是 `if not self._glass_appearance_on(): return`
    // 壁纸模式下标签栏同样是玻璃，所以这里必须用「或」判定，不能只看 glassMode
    if (!cfg_ || !glassAppearanceOn(*cfg_)) return;

    // main.py:524 面板开着时它正好挡在标签栏下面，抓到的是自己（亮色面板），
    // 不跳过的话标签栏会被重新烘焙成亮色 —— 这就是「鼠标移到标签上标签栏提亮」的根因
    if (panelOpen_) return;

    RECT wr = {};
    GetWindowRect(hwnd_, &wr);
    if (wr.bottom - wr.top <= 0) return;
    Backdrop bd;
    if (!Surface::capture(wr, height_, bd)) return;
    if (bd.sig == backdrop_.sig) return;
    backdrop_ = bd;

    // 永远深色玻璃（用户定调），不做浅色判定 —— 亮度压制在玻璃管线里做

    rebuildBase();
    redraw();
}

void BarWindow::rebuildBase() {
    if (width_ <= 0 || height_ <= 0) return;
    base_.create(width_, height_);
    if (cfg_ && glassAppearanceOn(*cfg_)) {
        base_.buildBar(backdrop_, dark_, 12);
        surfaceAvg_ = base_.average();
        // tab_bar.py:263 `_palette()`：玻璃底烘焙完才知道成品实际颜色，文字色据此再定一次
        readableText(surfaceAvg_, dark_, textMain_, textDim_);
    } else {
        const Theme& t = themeByName(cfg_ ? cfg_->themeColor : std::string("默认"));
        base_.fillRect(0, 0, width_, height_, t.bg);
        textMain_ = t.text;
        textDim_ = t.textDim;
    }
}

void BarWindow::redraw() {
    if (!hwnd_ || width_ <= 0 || height_ <= 0 || !cfg_) return;
    if (base_.width() != width_ || base_.height() != height_) rebuildBase();

    frame_ = base_;  // 拷贝缓存底，只叠胶囊/文字

    // tab_bar.py:291 `if self.glass and glass_render.PIL_OK:` —— 这里的 glass
    // 由 main.py 的 `_glass_appearance_on()` 决定，即「玻璃 OR 壁纸」
    bool glass = cfg_ && glassAppearanceOn(*cfg_);
    const GlassPalette& p = glassPalette(dark_);
    const Theme& th = themeByName(cfg_->themeColor);
    int padY = px(4);

    if (glass) {
        // 胶囊分两种（对照 tab_bar.py:335）：激活标签用实色强胶囊，鼠标停着的标签用淡胶囊。
        // 以前只画 active_ 一种，悬停时又顺手 setActive —— 鼠标一走胶囊就留在那儿褪不掉。
        auto capsuleFor = [&](int idx, uint32_t color) {
            if (idx < 0 || idx >= (int)cfg_->tabs.size() || idx >= (int)tabRects_.size()) return;
            int cx = idx * tabW_ + tabW_ / 2;
            int labelW = measureW(cfg_->tabs[idx].name, pt(10));
            int cw = labelW + px(26);
            int x1 = std::max(idx * tabW_ + px(6), cx - cw / 2);
            int x2 = std::min((idx + 1) * tabW_ - px(6), cx + cw / 2);
            int r = (height_ - padY * 2) / 2;
            frame_.drawCapsule(x1, padY, x2, height_ - padY, r, color);
        };
        if (hover_ >= 0) capsuleFor(hover_, p.capsule);   // 只留淡色悬停胶囊
        // 右侧图标悬停胶囊
        if (iconHover_ >= 0) {
            int ix1 = iconsStart_ + iconHover_ * iconW_;
            int ix2 = ix1 + iconW_;
            int r = (height_ - padY * 2) / 2;
            frame_.drawCapsule(ix1 + px(3), padY, ix2 - px(3), height_ - padY, r, p.capsule);
        }
        frame_.addHairline(dark_);
    } else {
        // 普通模式：格子底色 + 分隔线 + 激活底部强调线（悬停格用 hover 底色，对照 tab_bar.py:361）
        for (int i = 0; i < (int)tabRects_.size(); ++i) {
            const RECT& r = tabRects_[i];
            uint32_t bg = (i == active_) ? th.active : (i == hover_ ? th.hover : th.bg);
            frame_.fillRect(r.left, 0, r.right - r.left, height_, bg);
            frame_.fillRect(r.left, px(4), 1, height_ - px(8), th.hover);
            if (i == (int)tabRects_.size() - 1) {
                frame_.fillRect(r.right, px(4), 1, height_ - px(8), th.hover);
            }
        }
        frame_.fillRect(iconsStart_, 0, width_ - iconsStart_, height_, th.bg);
        frame_.fillRect(iconsStart_, px(4), 1, height_ - px(8), th.hover);
        frame_.fillRect(0, height_ - 1, width_, 1, th.hover);
    }

    // 标签文字
    float fs = (float)pt(10);
    for (int i = 0; i < (int)cfg_->tabs.size() && i < (int)tabRects_.size(); ++i) {
        RECT r = tabRects_[i];
        uint32_t color = (i == active_) ? textMain_ : textDim_;
        frame_.drawText(cfg_->tabs[i].name.c_str(), r, fs, color, 1, false);
        if (!glass && i == active_) {
            int x1 = (int)(i * tabW_ + tabW_ * 0.25);
            int x2 = (int)((i + 1) * tabW_ - tabW_ * 0.25);
            frame_.fillRect(x1, height_ - px(2), x2 - x1, px(2), th.accent);
        }
    }

    // ── 岛台按钮 ──
    //
    // ⚠ 不画在标签栏上了。
    //
    //   岛台按钮现在已经真正插进了系统任务栏（见
    //   docs/岛台注入任务栏实现记录.md），任务栏上那一个就是唯一入口。
    //   标签栏右侧再放一个会变成两个入口，反而多余。
    //
    //   保留 islandEnabled_ 是因为图标区的排版还要用它 ——
    //   岛台启用时标签条要让出一格（虽然现在是让给任务栏那边，
    //   但配置语义上仍是"岛台开着"）。
    //
    //   下面这段代码留着不删：以后如果要恢复"标签栏也放一个"，改回来即可。
#if 0
    if (islandEnabled_) {
        const int ix1 = iconsStart_;
        uint32_t color = islandOpen_ ? (dark_ ? 0xFFFFFFFF : 0xFF17181B) : textDim_;
        if (islandOpen_) {
            frame_.fillRoundRect(ix1 + px(2), px(2), iconW_ - px(4), height_ - px(4),
                                 px(5), dark_ ? 0x38FFFFFF : 0x28000000);
        }
        RECT r = {ix1, 0, ix1 + iconW_, height_};
        frame_.drawText(L"\uE71D", r, (float)pt(12), color, 1, false,
                        L"Segoe MDL2 Assets");
    }
#endif

    // 右侧图标。岛台不再占标签栏的位置，所以从 iconsStart_ 开始排。
    const int sysIconBase = iconsStart_;
    for (int i = 0; i < ICON_COUNT; ++i) {
        int ix1 = sysIconBase + i * iconW_;
        uint32_t color;
        if (glass) {
            color = (i == iconHover_) ? (dark_ ? 0xFFFFFFFF : 0xFF17181B) : textDim_;
        } else {
            if (i == iconHover_) {
                frame_.fillRect(ix1 + 1, 0, iconW_ - 2, height_, th.hover);
                color = th.text;
            } else {
                color = th.textDim;
            }
        }
        RECT r = {ix1, 0, ix1 + iconW_, height_};
        frame_.drawText(kGlyphs[i], r, (float)pt(12), color, 1, false, L"Segoe UI Symbol");
    }

    HDC screen = GetDC(nullptr);
    HBITMAP bmp = frame_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT src = {0, 0};
        SIZE size = {width_, height_};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT dst = {0, barY_};
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

void BarWindow::place(int y) {
    if (!hwnd_) return;
    barY_ = y;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);

    HDC screen = GetDC(nullptr);
    HBITMAP bmp = frame_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT src = {0, 0};
        SIZE size = {width_, height_};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT dst = {0, y};
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

void BarWindow::slideTo(int target) {
    // ⚠ 直接到位，不做动画。
    //
    //   踩过的坑：原来这里是 4 帧动画 ——
    //     for (i=1..4) { place(y); Sleep(16); PeekMessage/DispatchMessage... }
    //   在 GUI 线程上 Sleep 并且**嵌套派发消息**，后果有三个：
    //     ① 卡顿：一次显隐要占住线程 64ms 以上，叠加 120ms 的轮询
    //        定时器，鼠标移到顶边后要等很久才有反应；
    //     ② 收回慢：同上，因为动画本身阻塞了消息处理；
    //     ③ 显示不全：嵌套派发里如果又进来一次 show/hide，
    //        帧序列被打断，窗口就停在中间位置（半截露在屏幕外）。
    //
    //   系统自己的任务栏也是瞬间出现的，没有滑动动画。
    //   直接到位换来的是"跟手"和"永远停在正确位置"。
    slidingTo_ = target;
    if (barY_ == target) return;
    place(target);
}

void BarWindow::showBar() {
    if (visible_ && slidingTo_ == 0) return;
    visible_ = true;
    slideTo(0);
    logf("bar: 显示");
}

void BarWindow::hideBar() {
    if (!visible_ && slidingTo_ == -height_) return;
    visible_ = false;
    clearHover();
    slideTo(-height_);
    logf("bar: 隐藏");
}

void BarWindow::clearHover() {
    hoverPending_ = -1;
    if (hoverTimer_) {
        KillTimer(hwnd_, kHoverTimerId);
        hoverTimer_ = 0;
    }
    if (hover_ >= 0) {
        hover_ = -1;
        redraw();
    }
}

// \u2605 \u5168\u5c4f\u5e94\u7528\uff08\u6e38\u620f / \u5168\u5c4f\u89c6\u9891\uff09\u5728\u8dd1\u65f6\uff0c\u6807\u7b7e\u680f\u4e0d\u8be5\u51fa\u6765\u6253\u6270\u3002
//   \u2460 \u7cfb\u7edf\u901a\u77e5\u72b6\u6001\uff1a\u5168\u5c4f D3D / \u6f14\u793a\u6a21\u5f0f / \u5f59\u795e\u4f1a\u795e\u7684\u65f6\u5019\u90fd\u7b97\u201c\u522b\u6253\u6270\u201d\u3002
//   \u2461 \u515c\u5e95\uff1a\u524d\u53f0\u7a97\u53e3\u76d6\u6ee1\u5b83\u6240\u5728\u663e\u793a\u5668\uff0c\u4e14\u4e0d\u662f\u684c\u9762/\u4efb\u52a1\u680f/\u6211\u4eec\u81ea\u5df1\u7684\u7a97\u53e3\u3002
static bool fullscreenAppActive() {
    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(::SHQueryUserNotificationState(&st))) {
        if (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE || st == QUNS_BUSY)
            return true;
    }
    HWND fg = ::GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {};
    ::GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcsncmp(cls, L"Hotrainy", 8) == 0)
        return false;
    RECT wr{};
    if (!::GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
           wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

void BarWindow::autoHideTick() {
    // ★ 鼠标不在标签栏里就清掉悬停态。
    //
    //   原来只有 setPanelOpen(false) / WM_MOUSELEAVE 会清 hover_，
    //   而悬停模式下点了标签、面板一开，panelOpen_ 就是 true，
    //   鼠标再移开也没人清 —— 实色胶囊就留在那儿了。
    //   这里用轮询兜底：光标不在标签栏窗口内就清（和面板开没开无关）。
    // （清理逻辑已挪到 clearHoverIfMouseAway()，这里不再重复）

    if (!cfg_) return;
    if (!cfg_->autoHide) {
        // ★ 全屏应用时，即使是“常驻”模式也先收起来
        if (fullscreenAppActive()) {
            hideBar();
            return;
        }
        showBar();
        return;
    }
    // ★ 打游戏 / 看全屏视频时不弹标签栏（正在显示的也收起来）。
    //   菜单开着的时候不硬收，免得把用户正在用的菜单打断。
    if (fullscreenAppActive() && !menuOpen_) {
        clearHover();
        hideBar();
        return;
    }

    POINT p = {};
    GetCursorPos(&p);
    int trigger = px(2);
    bool show = (p.y <= trigger);
    if (!show && visible_ && p.y <= height_ + px(4)) show = true;
    if (panelOpen_ || menuOpen_) show = true;
    if (show) {
        showBar();
    } else {
        clearHover();  // 收起时连悬停态一起清掉，下次唤出不会残留胶囊
        hideBar();
    }
}

LRESULT CALLBACK BarWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    BarWindow* self = (BarWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (BarWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT BarWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;  // 不抢焦点（否则第一次点击只激活窗口）
        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(lp);
            int icon = iconAt(x);
            if (icon != iconHover_) {
                iconHover_ = icon;
                redraw();
            }

            int idx = tabAt(x);
            if (idx != hoverPending_) {
                hoverPending_ = idx;
                if (hoverTimer_) {
                    KillTimer(hwnd, kHoverTimerId);
                    hoverTimer_ = 0;
                }
                if (idx >= 0) {
                    // 35ms 停留阈值：扫过去不弹，停下才弹
                    hoverTimer_ = SetTimer(hwnd, kHoverTimerId, 35, nullptr);
                } else if (hover_ >= 0) {
                    hover_ = -1;
                    redraw();
                    if (onLeave) onLeave();
                }
            }
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            iconHover_ = -1;
            clearHover();
            if (onLeave) onLeave();
            redraw();
            return 0;
        case WM_TIMER:
            if (wp == kHoverTimerId) {
                KillTimer(hwnd, kHoverTimerId);
                hoverTimer_ = 0;
                int idx = hoverPending_;
                if (idx >= 0) {
                    hover_ = idx;
                    redraw();
            if (onHoverTab) onHoverTab(idx, tabRect(idx));
                }
                return 0;
            }
            return 0;
        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(lp);
            int y = (short)HIWORD(lp);

            // ⚠ 标签栏上不再有岛台按钮 —— 它在系统任务栏上。
            //   islandAt() 始终返回 false，这里保留调用是为了
            //   以后要恢复时不用改结构。
            if (islandAt(x)) {
                RECT r = islandButtonRect();
                logf("bar: 点岛台按钮");
                if (onIslandClick) onIslandClick(r);
                return 0;
            }

            int icon = iconAt(x);
            if (icon >= 0) {
                logf("bar: 点右侧图标 %d", icon);
                if (onIconClick) onIconClick(icon);
                return 0;
            }
            int idx = tabAt(x);
            if (idx >= 0) {
                logf("bar: 点标签 %d", idx);
                if (onTabClick) {
                    POINT pt = {x, y};
                    onTabClick(idx, false, pt);
                }
                // ★ 点完清掉悬停态：淡胶囊是画在 hover_ 上的，
                //   不清的话点过那一块会一直亮着（用户："点击标签后会保留那个光胶囊"）。
                //   鼠标还在标签上的话，下一次 WM_MOUSEMOVE 会重新置上，不影响悬停反馈。
                clearHover();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            int x = (short)LOWORD(lp);
            int y = (short)HIWORD(lp);
            if (islandAt(x)) return 0;     // 岛台按钮上不弹标签菜单
            if (iconAt(x) >= 0) return 0;  // 图标区不弹标签菜单
            int idx = tabAt(x);
            if (idx >= 0) {
                POINT pt = {x, y};
                ClientToScreen(hwnd, &pt);
                logf("bar: 右键标签 %d", idx);
                if (onTabClick) onTabClick(idx, true, pt);
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace hr
