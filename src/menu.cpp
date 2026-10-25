// hr/menu.cpp — 玻璃右键菜单（逐行对照 ui/glass_menu.py + ui/context_menu.py）
#include "hr/menu.h"

#include <algorithm>
#include <string>
#include <vector>

#include "hr/common.h"
#include "hr/dialog.h"
#include "hr/glass.h"
#include "hr/theme.h"

#ifndef GET_X_LPARAM
#include <windowsx.h>
#endif

namespace hr {
namespace {

const wchar_t* kClass = L"HotrainyMenu";
bool g_classReady = false;
std::function<void(bool)> g_onMenuOpenClose;

// 当前正在模态运行的菜单（同一时刻最多一个）。
// 让外部能请求关闭它 —— 例如岛台面板收起时，菜单要跟着一起关。
class MenuWindow;
MenuWindow* g_modalMenu = nullptr;

// 设计稿尺寸（对照 GlassMenu 的类常量）
const int kPadX = 10;
const int kRowH = 30;
const int kSepH = 9;
const int kRadius = 10;
const int kGlyphW = 20;
const int kMinW = 180;

const wchar_t* kUI = L"Microsoft YaHei";
// ⚠ 图标字体必须是 Segoe MDL2 Assets，不是 Segoe UI Symbol。
//
//   两者都叫"符号字体"，字形表却完全不同：
//     Segoe UI Symbol    —— 杂项符号（♪ ★ ➜ 之类），码位在 U+2000 段
//     Segoe MDL2 Assets  —— Windows 自己的图标字体，码位在私用区 U+E700+
//
//   菜单项用的是 \uE708（睡眠）、\uE777（重启）这些私用区码位，
//   只有 MDL2 有。用错字体的表现是每行左边一个空方块 ▯。
const wchar_t* kSym = L"Segoe MDL2 Assets";

class MenuWindow {
public:
    bool open(HWND owner, const std::vector<MenuRow>& rows, int x, int y, bool dark);
    void runModal();
    std::function<void()> takePending();

    // 从外部请求关闭（供 requestCloseGlassMenu 使用）。
    //
    // 先投一条空消息唤醒模态循环，再真正关闭 ——
    // 只 close() 的话，循环可能正卡在 GetMessage 上，
    // 没有新消息就不会醒过来重新检查 closed_。
    void requestClose();

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void measure();
    void clampPosition(int x, int y);
    void buildGlass();
    void redraw();
    void pushFrame();
    void close();
    int rowAt(int y) const;
    void activate(int index);
    void moveSelection(int delta);

    struct Pal {
        uint32_t text = 0xFFF2F3F5;
        uint32_t dim = 0xFF9AA0A8;
        uint32_t hot = 0x2EFFFFFF;
        uint32_t line = 0x29FFFFFF;
    };
    Pal palette() const;

    HWND hwnd_ = nullptr;
    std::vector<MenuRow> rows_;
    std::vector<RECT> itemRects_;   // 与 rows_ 一一对应，sep 为空
    std::vector<int> itemIndex_;    // rows_ 下标 → 第几个可点项

    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool dark_ = true;
    Surface glass_;
    Surface frame_;
    uint32_t avg_ = 0xFF1B1C20;

    int hover_ = -1;
    bool closed_ = false;
    std::function<void()> pending_;
};

void MenuWindow::measure() {
    float fs = (float)pt(10);
    int w = px(kMinW);
    int h = px(kPadX) / 2;
    for (const auto& r : rows_) {
        if (r.sep) {
            h += px(kSepH);
            continue;
        }
        int tw = Surface::measureText(r.text.c_str(), fs, kUI) + px(kGlyphW) + px(kPadX) * 2;
        w = std::max(w, tw);
        h += px(kRowH);
    }
    h += px(kPadX) / 2;
    w_ = w;
    h_ = h;
}

void MenuWindow::clampPosition(int x, int y) {
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR mon = MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    x_ = std::max<int>(wa.left + 4, std::min<int>(x, wa.right - w_ - 4));
    y_ = std::max<int>(wa.top + 4, std::min<int>(y, wa.bottom - h_ - 4));
}

void MenuWindow::buildGlass() {
    glass_.create(w_, h_);
    const int r = px(kRadius);
    Backdrop bd;
    RECT rc = {x_, y_, x_ + w_, y_ + h_};
    if (!Surface::capture(rc, 0, bd)) {
        avg_ = dark_ ? rgb(27, 28, 32) : rgb(242, 243, 245);
        glass_.fillRect(0, 0, w_, h_, avg_);
        return;
    }
    // 菜单面积小、常贴着屏幕边弹出，背后常是花哨壁纸 → 雾压得更重
    glass_.buildCard(bd, dark_, r, 22, dark_ ? 60 : 52);
    avg_ = glass_.average();
    glass_.addBorder(r, dark_);
    glass_.cutRoundCorners(r);
}

MenuWindow::Pal MenuWindow::palette() const {
    Pal p;
    uint32_t avg = avg_ ? avg_ : (dark_ ? rgb(27, 28, 32) : rgb(242, 243, 245));
    const uint32_t W = 0xFFFFFFFFu, B = 0xFF000000u;
    readableText(avg, dark_, p.text, p.dim);
    if (dark_) {
        p.hot = blendColor(avg, W, 0.18) | 0xFF000000u;
        p.line = blendColor(avg, W, 0.16) | 0xFF000000u;
    } else {
        p.hot = blendColor(avg, B, 0.09) | 0xFF000000u;
        p.line = blendColor(avg, B, 0.12) | 0xFF000000u;
    }
    return p;
}

void MenuWindow::redraw() {
    if (!hwnd_) return;
    if (frame_.width() != w_ || frame_.height() != h_) frame_.create(w_, h_);
    frame_ = glass_;
    Pal p = palette();

    int pad = px(kPadX);
    int y = px(kPadX) / 2;
    int idx = 0;
    itemRects_.assign(rows_.size(), RECT{0, 0, 0, 0});
    itemIndex_.assign(rows_.size(), -1);

    for (size_t i = 0; i < rows_.size(); ++i) {
        const MenuRow& r = rows_[i];
        if (r.sep) {
            int ly = y + px(kSepH) / 2;
            frame_.fillRect(pad, ly, w_ - pad * 2, 1, p.line);
            y += px(kSepH);
            continue;
        }
        int h = px(kRowH);
        if (idx == hover_) {
            frame_.fillRoundRect(pad / 2, y + px(2), w_ - pad, h - px(4), px(6), p.hot);
        }
        int hh = h / 2;
        RECT rg = {pad, y + hh - px(10), pad + px(kGlyphW), y + hh + px(10)};
        frame_.drawText(r.glyph.c_str(), rg, (float)pt(10), p.dim, 0, false, kSym);
        RECT rt = {pad + px(kGlyphW), y, w_ - pad, y + h};
        frame_.drawText(r.text.c_str(), rt, (float)pt(10), p.text, 0, false, kUI);

        itemRects_[i] = RECT{0, y, w_, y + h};
        itemIndex_[i] = idx;
        y += h;
        idx += 1;
    }
    pushFrame();
}

void MenuWindow::pushFrame() {
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    HBITMAP bmp = frame_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT dst = {x_, y_};
        SIZE sz = {w_, h_};
        POINT src = {0, 0};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

int MenuWindow::rowAt(int y) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].sep) continue;
        const RECT& r = itemRects_[i];
        if (r.bottom > r.top && y >= r.top && y <= r.bottom) return itemIndex_[i];
    }
    return -1;
}

void MenuWindow::moveSelection(int delta) {
    int n = (int)std::count_if(rows_.begin(), rows_.end(), [](const MenuRow& r) { return !r.sep; });
    if (n <= 0) return;
    if (hover_ < 0) {
        hover_ = delta > 0 ? 0 : n - 1;
    } else {
        hover_ = (hover_ + delta + n) % n;
    }
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    redraw();
}

void MenuWindow::activate(int index) {
    if (index >= 0) {
        int idx = 0;
        for (const auto& r : rows_) {
            if (r.sep) continue;
            if (idx == index) {
                pending_ = r.cb;
                break;
            }
            idx += 1;
        }
    }
    close();
}

void MenuWindow::requestClose() {
    if (closed_) return;
    if (hwnd_) ::PostMessageW(hwnd_, WM_NULL, 0, 0);   // 唤醒模态循环
    close();
}

void MenuWindow::close() {
    if (closed_) return;
    closed_ = true;
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT CALLBACK MenuWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MenuWindow* self = (MenuWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (MenuWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MenuWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            int i = rowAt(GET_Y_LPARAM(lp));
            if (i != hover_) {
                hover_ = i;
                SetCursor(LoadCursorW(nullptr, i >= 0 ? IDC_HAND : IDC_ARROW));
                redraw();
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
            activate(rowAt(GET_Y_LPARAM(lp)));
            return 0;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                close();
                return 0;
            }
            if (wp == VK_DOWN) {
                moveSelection(1);
                return 0;
            }
            if (wp == VK_UP) {
                moveSelection(-1);
                return 0;
            }
            if (wp == VK_RETURN) {
                activate(hover_);
                return 0;
            }
            return 0;

        case WM_TIMER: {
            // 兜底：菜单拿不到焦点时，靠定时看鼠标在菜单外有没有按下
            bool lbtn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool rbtn = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (lbtn || rbtn) {
                POINT pt = {};
                GetCursorPos(&pt);
                bool inside = pt.x >= x_ && pt.x <= x_ + w_ && pt.y >= y_ && pt.y <= y_ + h_;
                if (!inside) close();
            }
            return 0;
        }

        case WM_CLOSE:
            close();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd_, 1);
            hwnd_ = nullptr;
            closed_ = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool MenuWindow::open(HWND owner, const std::vector<MenuRow>& rows, int x, int y, bool dark) {
    rows_ = rows;
    dark_ = dark;
    measure();
    clampPosition(x, y);

    if (!g_classReady) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = MenuWindow::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        g_classReady = true;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"menu",
                            WS_POPUP, x_, y_, w_, h_, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    buildGlass();
    redraw();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    SetTimer(hwnd_, 1, 80, nullptr);
    (void)owner;
    return true;
}

void MenuWindow::runModal() {
    if (!hwnd_) return;

    // 登记"当前模态菜单"，让外部能请求关闭（见 requestCloseGlassMenu）。
    // 同一时刻只可能有一个菜单是模态的，所以用单指针就够。
    g_modalMenu = this;

    MSG msg;
    while (!closed_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_modalMenu = nullptr;
}

std::function<void()> MenuWindow::takePending() { return pending_; }

// ── 上下文菜单内容（对照 ShortcutContextMenu.show 的 rows 组装） ──

std::wstring fileNameOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring stripExt(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

std::wstring baseName(const std::wstring& path) {
    std::wstring s = path;
    while (s.size() > 1 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return fileNameOf(s);
}

}  // namespace

void setContextMenuOpenCallback(std::function<void(bool)> cb) { g_onMenuOpenClose = std::move(cb); }

void showGlassMenu(HWND owner, const std::vector<MenuRow>& rows, int x, int y, bool dark) {
    MenuWindow menu;
    if (!menu.open(owner, rows, x, y, dark)) return;
    menu.runModal();
    auto cb = menu.takePending();
    if (cb) cb();
}

void requestCloseGlassMenu() {
    if (!g_modalMenu) return;
    g_modalMenu->requestClose();
}

void showContextMenu(HWND owner, int tabIndex, int shortcutIndex, int x, int y, bool dark,
                     const ContextMenuHooks& hooks) {
    std::vector<MenuRow> rows;

    auto item = [](const wchar_t* glyph, const wchar_t* text, std::function<void()> cb) {
        MenuRow r;
        r.sep = false;
        r.glyph = glyph;
        r.text = text;
        r.cb = std::move(cb);
        return r;
    };
    auto sep = []() {
        MenuRow r;
        r.sep = true;
        return r;
    };

    // 添加快捷方式：选文件 → 问名字 → 交给主程序去重/落库
    rows.push_back(item(L"\uE710", L"添加快捷方式", [owner, tabIndex, hooks]() {
        SetWindowPos(owner, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        std::wstring path;
        bool picked = pickFileDialog(owner, L"选择程序或快捷方式",
                                     L"可执行文件和快捷方式\0*.exe;*.lnk\0所有文件\0*.*\0\0", path);
        SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (!picked) return;
        std::wstring name = stripExt(fileNameOf(path));
        std::wstring custom;
        if (askString(owner, L"快捷方式名称", L"请输入快捷方式显示名称:", name, custom)) {
            if (!custom.empty() && hooks.onAdd) hooks.onAdd(tabIndex, path, custom);
        }
    }));

    // 添加文件夹
    rows.push_back(item(L"\uE8B7", L"添加文件夹", [owner, tabIndex, hooks]() {
        SetWindowPos(owner, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        std::wstring path;
        bool picked = pickFolderDialog(owner, L"选择文件夹", path);
        SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (!picked) return;
        std::wstring name = baseName(path);
        std::wstring custom;
        if (askString(owner, L"文件夹名称", L"请输入文件夹显示名称:", name, custom)) {
            if (!custom.empty() && hooks.onAdd) hooks.onAdd(tabIndex, path, custom);
        }
    }));

    if (shortcutIndex >= 0) {
        rows.push_back(item(L"\uE70F", L"编辑快捷方式", [owner, tabIndex, shortcutIndex, hooks]() {
            if (hooks.onEdit) hooks.onEdit(tabIndex, shortcutIndex);
        }));
        rows.push_back(item(L"\uE8CB", L"调换序列", [tabIndex, shortcutIndex, hooks]() {
            if (hooks.onMove) hooks.onMove(tabIndex, shortcutIndex);
        }));
        rows.push_back(item(L"\uE74D", L"删除快捷方式", [owner, tabIndex, shortcutIndex, hooks]() {
            if (!askYesNo(owner, L"确认删除", L"确定要删除此快捷方式吗？此操作不可恢复。")) return;
            if (hooks.onDelete) hooks.onDelete(tabIndex, shortcutIndex);
        }));
        rows.push_back(sep());
        rows.push_back(item(L"\uE838", L"打开文件位置", [tabIndex, shortcutIndex, hooks]() {
            if (hooks.onOpenLocation) hooks.onOpenLocation(tabIndex, shortcutIndex);
        }));
    } else {
        rows.push_back(sep());
        rows.push_back(item(L"\uE8AC", L"重命名标签", [owner, tabIndex, hooks]() {
            std::wstring name;
            if (!askString(owner, L"重命名标签", L"请输入新的标签名称:", L"", name)) return;
            // 去掉首尾空白
            while (!name.empty() && (name.front() == L' ' || name.front() == L'\t'))
                name.erase(name.begin());
            while (!name.empty() && (name.back() == L' ' || name.back() == L'\t'))
                name.pop_back();
            if (!name.empty() && hooks.onRenameTab) hooks.onRenameTab(tabIndex, name);
        }));
        rows.push_back(item(L"\uE710", L"添加标签", [tabIndex, hooks]() {
            // 名字由 onAddTab 自己弹框询问。
            // （以前这里也问一次，导致连弹两次输入框；
            //   第二次一取消就 return 了，标签自然没加上。）
            if (hooks.onAddTab) hooks.onAddTab(tabIndex);
        }));
        rows.push_back(item(L"\uE74D", L"删除此标签", [tabIndex, hooks]() {
            if (hooks.onDeleteTab) hooks.onDeleteTab(tabIndex);
        }));
        rows.push_back(item(L"\uE76B", L"标签左移", [tabIndex, hooks]() {
            if (hooks.onMoveTabLeft) hooks.onMoveTabLeft(tabIndex);
        }));
        rows.push_back(item(L"\uE76C", L"标签右移", [tabIndex, hooks]() {
            if (hooks.onMoveTabRight) hooks.onMoveTabRight(tabIndex);
        }));
    }

    rows.push_back(sep());
    rows.push_back(item(L"\uE713", L"设置…", [hooks]() {
        if (hooks.onOpenSettings) hooks.onOpenSettings();
    }));
    rows.push_back(item(L"\uE7E8", L"退出程序", [hooks]() {
        if (hooks.onQuit) hooks.onQuit();
    }));

    if (g_onMenuOpenClose) g_onMenuOpenClose(true);
    MenuWindow menu;
    if (menu.open(owner, rows, x, y, dark)) {
        menu.runModal();
        auto cb = menu.takePending();
        if (g_onMenuOpenClose) g_onMenuOpenClose(false);
        if (cb) cb();
    } else {
        if (g_onMenuOpenClose) g_onMenuOpenClose(false);
    }
}

}  // namespace hr
