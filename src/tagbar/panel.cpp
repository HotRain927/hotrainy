// hr/panel.cpp — 对照 ui/hover_panel.py
#include "hr/tagbar/panel.h"

#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>

#include "hr/shared/common.h"
#include "hr/shared/icons.h"
#include "hr/text/theme.h"
#include <thread>
#include <shlobj.h>

namespace hr {

namespace {

const wchar_t* kPanelClass = L"HotrainyPanel";

// 各行高 / 图标边长：字号 10 时行高 26、图标 18（再叠 DPI）
int rowHeightFor(int size) { return std::max(px(20), (int)std::lround(px(size * 2.6))); }
int iconSizeFor(int size) { return std::max(px(14), (int)std::lround(px(size * 1.8))); }

}  // namespace

bool HoverPanel::create(HINSTANCE hInst, Config* cfg) {
    hinst_ = hInst;
    cfg_ = cfg;
    fontSize_ = cfg ? cfg->shortcutFontSize : 10;
    rowH_ = rowHeightFor(fontSize_);
    iconSize_ = iconSizeFor(fontSize_);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HoverPanel::proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPanelClass;
    if (!RegisterClassExW(&wc)) {
        logf("panel: RegisterClass 失败 %lu", (unsigned long)GetLastError());
        return false;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            kPanelClass, L"hotrainy-panel", WS_POPUP, 0, 0, 1, 1, nullptr,
                            nullptr, hInst, this);
    if (!hwnd_) {
        logf("panel: CreateWindowEx 失败 %lu", (unsigned long)GetLastError());
        return false;
    }
    return true;
}

void HoverPanel::destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void HoverPanel::setDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    baseValid_ = false;
    if (visible_) rebuildBase(), redraw();
}

void HoverPanel::setFontSize(int size) {
    size = std::max(kFontMin, std::min(kFontMax, size));
    if (size == fontSize_) return;
    fontSize_ = size;
    rowH_ = rowHeightFor(fontSize_);
    iconSize_ = iconSizeFor(fontSize_);
    if (visible_) layout(), rebuildBase(), redraw();
}

void HoverPanel::onConfigChanged() {
    if (visible_) layout(), rebuildBase(), redraw();
}

void HoverPanel::layout() {
    // 内容边距
    top_ = px(6);
    rowX1_ = px(6);
    rowX2_ = w_ - px(6);
    iconX_ = px(10);
    nameX_ = iconX_ + iconSize_ + px(10);
    buildScrollbar();
}

void HoverPanel::showForTab(int tabIndex, int x, int y, int width, int screenH) {
    // ★ 每次显示都重申置顶：防止被菜单/对话框的临时降级或别的窗口顶掉后一直沉在软件下面
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!cfg_ || tabIndex < 0 || tabIndex >= (int)cfg_->tabs.size()) return;

    bool wasVisible = visible_;
    bool needShow = !wasVisible;  // 这一轮结束后要不要 ShowWindow
    RECT oldRect = {x_, y_, x_ + w_, y_ + h_};

    // 位置尺寸（对照 show_for_tab）
    int inset = px(4), gap = px(6), pad = px(6);
    tabIndex_ = tabIndex;
    x_ = x + inset;
    y_ = y + gap;
    w_ = std::max(width - inset * 2, px(180));

    int maxH = (int)(screenH * 0.6);
    h_ = std::min(std::max(screenH / 4, rowH_ + pad * 2), maxH);

    rowH_ = rowHeightFor(fontSize_);
    iconSize_ = iconSizeFor(fontSize_);
    scrollRow_ = 0;
    hoverRow_ = -1;
    dragging_ = false;

    // hover_panel.py:296 的复用 key 是 (w, h, x, y, wallpaper if use_wallpaper else "")。
    // 少判壁纸路径的话，从「没壁纸的标签」切到「有壁纸的标签」时位置尺寸完全一样，
    // 会直接复用上一张玻璃底，壁纸永远出不来。
    std::wstring wallKey;
    if (useWallpaperFor(*cfg_, tabIndex)) wallKey = cfg_->tabs[tabIndex].wallpaper;

    bool sameGeom = wasVisible && baseValid_ && oldRect.left == x_ && oldRect.top == y_ &&
                    (oldRect.right - oldRect.left) == w_ &&
                    (oldRect.bottom - oldRect.top) == h_ && wallKey == wallKey_;
    wallKey_ = wallKey;

    layout();

    const DWORD tEntry0 = ::GetTickCount();

    if (!sameGeom) {
        // 面板已经开着时它自己正盖在要抓的区域上。
        // 相邻标签的新位置和旧位置不重叠（干净屏幕），直接抓就行 —— 省一次
        // 收起/重现，也不会有「面板闪一下」的观感；
        // 只有真的重叠（同一标签重开、尺寸变了）才需要先收起来再抓。
        bool overlap = wasVisible && !(x_ + w_ <= oldRect.left || x_ >= oldRect.right ||
                                       y_ + h_ <= oldRect.top || y_ >= oldRect.bottom);
        if (overlap) {
            // 直接收起来，等 DWM 把它从合成画面里摘掉再抓。
            // 原来用 SWP_NOREDRAW 挪到屏幕外：对分层窗口没用 —— SWP_NOREDRAW 只挡
            // WM_PAINT，DWM 照样按新坐标合成，中间那两帧面板是「凭空消失」的。
            ShowWindow(hwnd_, SW_HIDE);
            DwmFlush();
            needShow = true;  // 收起来抓完背景，下面还要按新帧重新出现
        }
        RECT rc = {x_, y_, x_ + w_, y_ + h_};
        Backdrop bd;
        if (Surface::capture(rc, 0, bd)) backdrop_ = bd;
        baseValid_ = false;
    }
    const DWORD tCap = ::GetTickCount();

    // 展开耗时打点。
    //
    // ⚠ 为什么要量：用户反馈"标签面板出现有点慢"。
    //   这条路上有两块重活 ——
    //     ① Surface::capture：BitBlt 抓屏（带 CAPTUREBLT）+ 逐像素转换 + 缩小
    //     ② rebuildBase：boxBlur + 双三次上采样烘焙玻璃底
    //   不量就不知道哪块占大头（实测烘焙只有 15ms，所以嫌疑在抓屏）。
    const DWORD tBuild0 = tCap;
    if (!baseValid_) rebuildBase();
    const DWORD tBuild = ::GetTickCount();

    // 先出帧、后出现。UpdateLayeredWindow 会把「位置 + 尺寸 + 像素」一次性交给
    // DWM，所以窗口露出来的第一帧就是成品。
    // 反过来（先 SetWindowPos(SWP_SHOWWINDOW) 再 redraw）就会频闪：DWM 先用窗口
    // 里上一次留下的旧表面合成一帧，切标签时那一帧正是上一个标签的内容，
    // 肉眼看到的就是「鼠标刚碰到标签，面板闪一下才正常」。
    redraw();
    const DWORD tRedraw = ::GetTickCount();
    if (needShow) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
    // hover_panel.py:367 `_start_polling()`：`self._fg_hwnd = self._get_foreground()`
    // 是**记录**当前前台，不是置空！置空的话下一拍就会误判「前台窗口变了」，
    // 跟着 refreshBackdropLive 重抓一次背景 —— 那是另一个「展开瞬间频闪」的根因。
    fgHwnd_ = GetForegroundWindow();
    logf("panel: 展开标签 %d @(%d,%d) %dx%d 行高 %d 几何复用=%d 抓屏=%lums 烘焙=%lums 绘制=%lums 合计=%lums",
         tabIndex_, x_, y_, w_, h_, rowH_, (int)sameGeom,
         (unsigned long)(tCap - tEntry0), (unsigned long)(tBuild - tBuild0),
         (unsigned long)(tRedraw - tBuild), (unsigned long)(tRedraw - tEntry0));
}

void HoverPanel::hide() {
    if (!visible_) return;
    visible_ = false;
    dragging_ = false;
    tracking_ = false;      // 下次进入要重新登记 TME_LEAVE（见 WM_MOUSEMOVE）
    ShowWindow(hwnd_, SW_HIDE);
    tabIndex_ = -1;
    hoverRow_ = -1;
    scrollRow_ = 0;
    baseValid_ = false;
    logf("panel: 收起");
}

bool HoverPanel::mouseInside() const {
    if (!visible_) return false;
    POINT p = {};
    GetCursorPos(&p);
    return p.x >= x_ && p.x <= x_ + w_ && p.y >= y_ && p.y <= y_ + h_;
}

void HoverPanel::rebuildBase() {
    if (w_ <= 0 || h_ <= 0) return;
    base_.create(w_, h_);
    const int radius = px(14);

    // ★ 抓一次背后的画面（不模糊），只喂给边框内侧的折射；抓不到就只画斜面边框
    RECT rc = { x_, y_, x_ + w_, y_ + h_ };
    if (!Surface::capture(rc, 0, backdrop_)) { /* 忽略：无背景时折射自动跳过 */ }

    // ★ 纯透明底 + 液态玻璃边框（有背景则边框内侧折射）
    const int op = cfg_ ? cfg_->panelOpacity : 0;
    base_.buildFlatPanel(op, radius, dark_, &backdrop_);
    base_.cutRoundCorners(radius);
    // ★ 整幅内缩 8px 并画出外圈柔和投影（窗口尺寸不变）
    base_.applyOuterShadow(px(8), radius, dark_);
    baseValid_ = true;
}

void HoverPanel::buildScrollbar() {
    sbVisible_ = false;
    if (maxScroll_ <= 0 || w_ <= 0) return;
    int barW = px(4);
    int xMid = w_ - px(6) - barW / 2;
    int trackY1 = top_ + px(2);
    int trackY2 = h_ - top_ - px(2);
    int trackH = trackY2 - trackY1;
    if (trackH <= barW * 3) return;

    int total = (tabIndex_ >= 0 && tabIndex_ < (int)cfg_->tabs.size())
                    ? (int)cfg_->tabs[tabIndex_].shortcuts.size()
                    : 0;
    if (total <= 0) return;
    int thumbH = std::max(px(28), (int)((double)trackH * visibleRows_ / total));
    int span = std::max(1, trackH - thumbH);
    int thumbY1 = trackY1 + (int)((double)span * scrollRow_ / std::max(1, maxScroll_));

    sbVisible_ = true;
    sbX1_ = xMid - barW;
    sbX2_ = xMid + barW;
    sbTrackY1_ = trackY1;
    sbTrackY2_ = trackY2;
    sbThumbY1_ = thumbY1;
    sbThumbY2_ = thumbY1 + thumbH;
    sbSpan_ = span;
    sbThumbH_ = thumbH;
}

void HoverPanel::setScroll(int row) {
    row = std::max(0, std::min(row, maxScroll_));
    if (row == scrollRow_) return;
    scrollRow_ = row;
    if (visible_) redraw();
}

int HoverPanel::rowAt(int y) const {
    if (tabIndex_ < 0 || tabIndex_ >= (int)cfg_->tabs.size()) return -1;
    const auto& sc = cfg_->tabs[tabIndex_].shortcuts;
    for (int d = 0; d < visibleRows_; ++d) {
        int idx = scrollRow_ + d;
        if (idx >= (int)sc.size()) break;
        int y1 = top_ + d * rowH_;
        int y2 = y1 + rowH_;
        if (y >= y1 && y <= y2) return idx;
    }
    return -1;
}

void HoverPanel::redraw() {
    if (!hwnd_ || w_ <= 0 || h_ <= 0 || !cfg_) return;
    if (!baseValid_) rebuildBase();
    frame_ = base_;

    // hover_panel.py:158 `_glass_like`
    bool glass = cfg_ && glassLikePanel(*cfg_, tabIndex_);
    const Theme& th = themeByName(cfg_->themeColor);

    bool tabOk = tabIndex_ >= 0 && tabIndex_ < (int)cfg_->tabs.size();
    if (tabOk) {
        const Tab& tab = cfg_->tabs[tabIndex_];
        if (tab.shortcuts.empty()) {
            maxScroll_ = 0;
            sbVisible_ = false;
            RECT r = {0, 0, w_, h_};
            frame_.drawText(L"右键标签添加快捷方式", r, (float)pt(10), textDim_, 1, false);
        } else {
            int total = (int)tab.shortcuts.size();
            // hover_panel.py:558 `need_wallpaper = wallpaper_mode and not wallpaper`
            int hintH = (cfg_->wallpaperMode && tab.wallpaper.empty()) ? rowH_ : 0;
            visibleRows_ = std::max(1, (h_ - top_ * 2 - hintH) / rowH_);
            maxScroll_ = std::max(0, total - visibleRows_);
            scrollRow_ = std::max(0, std::min(scrollRow_, maxScroll_));
            buildScrollbar();

            int start = scrollRow_;
            int end = std::min(total, start + visibleRows_);
            int avail = rowX2_ - nameX_ - px(12);
            float namePx = (float)pt(fontSize_);

            for (int i = start; i < end; ++i) {
                int d = i - start;
                int y1 = top_ + d * rowH_;
                int y2 = y1 + rowH_;
                int iconY = (y1 + y2) / 2;

                if (i == hoverRow_) {
                    if (glass) {
                        // 对照 Python：普通矩形（不是圆角），颜色是成品平均色的实色
                        frame_.fillRect(rowX1_, y1 + px(2), rowX2_ - rowX1_,
                                        (y2 - px(2)) - (y1 + px(2)), rowHoverColor_);
                    } else {
                        frame_.fillRect(rowX1_, y1, rowX2_ - rowX1_, y2 - y1, th.hover);
                    }
                } else if (!glass && i % 2 == 0) {
                    frame_.fillRect(rowX1_, y1, rowX2_ - rowX1_, y2 - y1, th.active);
                }

                const Shortcut& s = tab.shortcuts[i];
                std::wstring target = !s.iconPath.empty() ? s.iconPath : s.targetPath;
                HBITMAP bmp = iconFor(target, std::max(48, iconSize_));
                if (bmp) {
                    frame_.drawBitmap(bmp, iconX_, iconY - iconSize_ / 2, iconSize_);
                } else {
                    DWORD attr = GetFileAttributesW(shellPath(s.targetPath).c_str());
                    bool isDir = attr != INVALID_FILE_ATTRIBUTES &&
                                 (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    uint32_t fill =
                        isDir ? 0xFF4E7A57u : (glass && dark_ ? 0xFF3A3D44u : 0xFFD8DBE0u);
                    frame_.fillRect(iconX_, iconY - iconSize_ / 2, iconSize_, iconSize_, fill);
                    std::wstring ph = isDir ? std::wstring(L"📁") : s.name.substr(0, 2);
                    RECT pr = {iconX_, iconY - iconSize_ / 2, iconX_ + iconSize_,
                               iconY + iconSize_ / 2};
                    frame_.drawText(ph.c_str(), pr, (float)pt(std::max(7, fontSize_ - 2)),
                                    (glass && dark_) ? 0xFFE6E6EAu : 0xFFCCCCCCu, 1, true);
                }

                std::wstring label = Surface::fitText(s.name.c_str(), namePx, avail);
                RECT tr = {nameX_, y1, nameX_ + avail, y2};
                frame_.drawText(label.c_str(), tr, namePx, textMain_, 0, false);
            }


            if (sbVisible_) {
                uint32_t toward = dark_ ? 0xFFFFFFFFu : 0xFF000000u;
                uint32_t trackCol = blendColor(avg_, toward, 0.12);
                uint32_t thumbCol = blendColor(avg_, toward, 0.42);
                int barW = px(4);
                frame_.fillRoundRect((sbX1_ + sbX2_) / 2 - barW / 2, sbTrackY1_ + barW / 2, barW,
                                     (sbTrackY2_ - barW / 2) - (sbTrackY1_ + barW / 2), barW / 2,
                                     trackCol);
                frame_.fillRoundRect((sbX1_ + sbX2_) / 2 - barW / 2, sbThumbY1_ + barW / 2, barW,
                                     (sbThumbY2_ - barW / 2) - (sbThumbY1_ + barW / 2), barW / 2,
                                     thumbCol);
            }
        }
    }
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = frame_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT src = {0, 0};
        SIZE size = {w_, h_};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        POINT dst = {x_, y_};
        // ★ 全局面板透明度：滑块 0-100 作用在这一帧的整体 alpha 上
        if (cfg_) { int _op = cfg_->panelOpacity; if (_op < 0) _op = 0; if (_op > 100) _op = 100; blend.SourceConstantAlpha = (BYTE)((int)blend.SourceConstantAlpha * _op / 100); }
        UpdateLayeredWindow(hwnd_, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

// ★ 把目标程序"已经存在的"窗口拉到前台。
//   AllowSetForegroundWindow 只对我们直接启动的那个进程有效；而 WorkBuddy 这类
//   带常驻 daemon 的程序是"新进程交接给老实例后立刻退出"，权限给不到老实例，
//   所以只能自己去找它的主窗口抢前台。此刻面板正是前台窗口，调用是合法的。
static std::wstring lnkTarget(const std::wstring& path) {
    if (path.size() < 4) return path;
    std::wstring ext = path.substr(path.size() - 4);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext != L".lnk") return path;
    std::wstring out = path;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, (void**)&link))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->Load(path.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH] = {};
                if (SUCCEEDED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) && buf[0]) out = buf;
            }
            pf->Release();
        }
        link->Release();
    }
    CoUninitialize();
    return out;
}

static bool bringToFront(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    struct Ctx { const std::wstring* want; HWND found; };
    Ctx ctx{&exePath, nullptr};
    ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        if (!::IsWindowVisible(h)) return TRUE;
        if (::GetWindow(h, GW_OWNER) != nullptr) return TRUE;      // 只要主窗口
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        if (!pid) return TRUE;
        HANDLE ph = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!ph) return TRUE;
        wchar_t buf[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (::QueryFullProcessImageNameW(ph, 0, buf, &n)) {
            std::wstring a = buf, b = *c->want;
            for (auto& ch : a) ch = (wchar_t)towlower(ch);
            for (auto& ch : b) ch = (wchar_t)towlower(ch);
            if (a == b) c->found = h;
        }
        ::CloseHandle(ph);
        return c->found ? FALSE : TRUE;
    }, (LPARAM)&ctx);
    if (!ctx.found) return false;
    ::ShowWindow(ctx.found, SW_RESTORE);
    ::SetForegroundWindow(ctx.found);
    return true;
}

void HoverPanel::launch(int rowIndex) {
    if (tabIndex_ < 0 || tabIndex_ >= (int)cfg_->tabs.size()) return;
    const auto& sc = cfg_->tabs[tabIndex_].shortcuts;
    if (rowIndex < 0 || rowIndex >= (int)sc.size()) return;
    const Shortcut& s = sc[rowIndex];
    std::wstring target = shellPath(s.targetPath);
    if (target.empty()) return;
    std::wstring args = s.args;
    // ★ 两个坑（实测：日志报成功、进程却没起来）：
    //   ① 路径里的正斜杠：ShellExecute 对 D:/a/b.exe 处理不可靠 —— 统一换成反斜杠
    //   ② 工作目录传 nullptr：子进程会继承本程序的目录，而 Electron/游戏启动器
    //      这类程序**必须在自己的目录下启动**，否则"返回成功但起不来"。
    //      所以把目标所在目录作为 lpDirectory 传进去。
    std::wstring norm(target);
    for (auto& ch : norm) if (ch == L'/') ch = L'\\';
    std::wstring workdir = norm;
    const size_t slash = workdir.find_last_of(L'\\');
    workdir = (slash == std::wstring::npos) ? std::wstring() : workdir.substr(0, slash);

    ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
    HINSTANCE r = ShellExecuteW(hwnd_, L"open", norm.c_str(),
                                args.empty() ? nullptr : args.c_str(),
                                workdir.empty() ? nullptr : workdir.c_str(),
                                SW_SHOWNORMAL);
    logf("panel: 启动 %s (cwd=%s) -> %lld", toUtf8(norm).c_str(),
         toUtf8(workdir).c_str(), (long long)(INT_PTR)r);

    // 启动后去抢前台：这类程序会交接给已运行实例并立刻退出，
    // 所以我们自己去找它的主窗口拉到前台（面板此刻正是前台窗口）。
    std::wstring want = lnkTarget(norm);
    std::thread([want]() {
        for (int k = 0; k < 12; ++k) {
            ::Sleep(150);
            if (bringToFront(want)) break;
        }
    }).detach();
}

bool HoverPanel::isOwnWindow(HWND h) const {
    if (!h) return false;
    if (h == hwnd_) return true;
    // 标签栏 / 面板自己的 popup 都算自己人
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, 64)) return false;
    return wcscmp(cls, L"HotrainyBar") == 0 || wcscmp(cls, L"HotrainyPanel") == 0;
}

void HoverPanel::refreshBackdropLive() {
    // 对照 Python 的 _rebuild_offscreen：面板可见时抓屏会把自己拍进去，
    // 所以先收起来、等 DWM 合成一帧，抓完再按新帧重新出现。
    // 不用「挪到屏幕外」那套：SWP_NOREDRAW 只挡 WM_PAINT，DWM 依旧按新坐标
    // 合成，面板会实打实地消失两帧再回来，那就是一次频闪。
    // 也不用 WDA_EXCLUDEFROMCAPTURE：那会让用户自己的截图/录屏也拍不到面板。
    ShowWindow(hwnd_, SW_HIDE);
    DwmFlush();

    RECT rc = {x_, y_, x_ + w_, y_ + h_};
    Backdrop bd;
    bool ok = Surface::capture(rc, 0, bd);

    if (ok && bd.sig != backdrop_.sig) {
        backdrop_ = bd;
        baseValid_ = false;
        rebuildBase();
    }
    redraw();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void HoverPanel::pollTick(const RECT& barRect) {
    if (!visible_) return;
    // 只有前台窗口换了才重抓背景（对照 _check_background_changed）。
    // 原来这里每 150ms 无条件 hide->抓屏->show，鼠标停在标签上时会一直频闪。
    // hover_panel.py:387 `_check_background_changed`：
    //   不是玻璃外观 → 直接返回；用壁纸当底 → 也直接返回（壁纸是固定的，不用重抓）
    bool useWall = cfg_ && useWallpaperFor(*cfg_, tabIndex_);
    if (cfg_ && glassLikePanel(*cfg_, tabIndex_) && !useWall) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != fgHwnd_) {
            fgHwnd_ = fg;
            if (!isOwnWindow(fg)) refreshBackdropLive();
        }
    }

    if (mouseInside()) return;
    POINT p = {};
    GetCursorPos(&p);
    bool inBar = p.x >= barRect.left && p.x < barRect.right && p.y >= barRect.top &&
                 p.y < barRect.bottom;
    if (inBar) return;

    // 标签栏和面板之间留着一条缝（栏高 36，面板从 45 起）。从标签往面板走
    // 必然要穿过它；手一慢，150ms 的轮询正好落在缝里，面板就会在人眼皮底下收掉。
    // 缝里（面板正上方那一条竖带）也算「还在路上」。
    bool inGap = p.y > barRect.bottom && p.y < y_ && p.x >= x_ - px(6) &&
                 p.x <= x_ + w_ + px(6);
    if (inGap) return;

    hide();
    if (onLeavePanel) onLeavePanel();
}

LRESULT CALLBACK HoverPanel::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HoverPanel* self = (HoverPanel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (HoverPanel*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HoverPanel::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            int y = (short)HIWORD(lp);
            int row = rowAt(y);
            if (row != hoverRow_) {
                hoverRow_ = row;
                redraw();
            }
            if (!dragging_) {
                // ⚠ 这里原来是个函数内的 `static bool tracked` —— bug。
                //
                //   static 只初始化一次、跨所有实例和所有"进入面板"共享。
                //   第一次进面板时登记了 TME_LEAVE，之后 tracked 永远是 true，
                //   于是**再也不登记** —— 系统只在登记后才会发 WM_MOUSELEAVE，
                //   所以第二次及以后鼠标快速移出面板，我们收不到离开通知，
                //   悬停行不消失、面板该收的时候不收。
                //
                //   改成用成员（每次 hide 时复位），语义才正确。
                if (!tracking_) {
                    TRACKMOUSEEVENT tme = {};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    tracking_ = true;
                }
            } else {
                int span = std::max(1, sbSpan_);
                setScroll(dragStartRow_ + (int)((double)(y - dragStartY_) * maxScroll_ / span));
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverRow_ >= 0) {
                hoverRow_ = -1;
                redraw();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (maxScroll_ > 0) setScroll(scrollRow_ - (delta > 0 ? 3 : -3));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = (short)LOWORD(lp);
            int y = (short)HIWORD(lp);

            if (sbVisible_ && x >= sbX1_ && x <= sbX2_) {
                if (y >= sbThumbY1_ && y <= sbThumbY2_) {
                    dragging_ = true;
                    dragStartY_ = y;
                    dragStartRow_ = scrollRow_;
                    SetCapture(hwnd);
                    return 0;
                }
                if (y >= sbTrackY1_ && y <= sbTrackY2_) {
                    setScroll(scrollRow_ + (y < sbThumbY1_ ? -visibleRows_ : visibleRows_));
                    return 0;
                }
            }

            int row = rowAt(y);
            if (row >= 0) launch(row);
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_RBUTTONUP: {
            int y = (short)HIWORD(lp);
            int row = rowAt(y);
            if (row >= 0) {
                POINT pt = {(short)LOWORD(lp), y};
                ClientToScreen(hwnd, &pt);
                if (onShortcutRightClick) onShortcutRightClick(row, pt);
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace hr
