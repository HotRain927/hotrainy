// hr/dialog.cpp — 玻璃弹窗（逐行对照 ui/glass_dialog.py）
//
// 与设置窗口同一套烘焙玻璃，只是面积小、多压一层雾（深 40 / 浅 34）。
// 输入框用原生 EDIT：中文要走 IME，自绘字没法输入。
// 底色调成玻璃的凹槽色，看起来像玻璃上挖出来的一条。
#include "hr/settings/dialog.h"

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "hr/shared/common.h"
#include "hr/glass/glass.h"
#include "hr/text/theme.h"

namespace hr {
namespace {

const wchar_t* kClass = L"HotrainyDialog";
bool g_classReady = false;
bool g_dark = true;

const wchar_t* kUI = L"Microsoft YaHei";

// 设计稿尺寸
const int kDesignW = 420;
const int kPad = 22;
const int kRadius = 12;
const int kBtnH = 32;
const int kBtnW = 84;
const int kBtnGap = 10;
const int kLineH = 21;
const int kEntryH = 32;

enum Action { ACT_NONE = 0, ACT_OK, ACT_CANCEL };

struct Hit {
    RECT rc;
    int action;
};

class DialogWindow {
public:
    bool open(HWND owner, const wchar_t* title, const wchar_t* message, bool withEntry,
              const std::wstring& initial, const wchar_t* okText, const wchar_t* cancelText);

    // 局部消息循环，直到窗口关闭（相当于 Tk 的 wait_window）
    void runModal();

    bool ok() const { return ok_; }
    std::wstring text() const { return text_; }

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void wrapText();
    void layoutEntry();
    void buildGlass();
    void redraw();
    void pushFrame();
    void finish(bool ok);
    int actionAt(int x, int y) const;
    void drawButton(int x1, int y1, int x2, int y2, const std::wstring& label, uint32_t fill,
                    uint32_t hot, int action, uint32_t fg);

    struct Pal {
        uint32_t text = 0xFFF2F3F5;
        uint32_t dim = 0xFF9AA0A8;
        uint32_t pill = 0x1AFFFFFF;
        uint32_t pillLine = 0x33FFFFFF;
        uint32_t btn = 0x24FFFFFF;
        uint32_t btnHot = 0x3DFFFFFF;
        uint32_t primary = 0x4DFFFFFF;
        uint32_t primaryHot = 0x66FFFFFF;
        uint32_t edge = 0x4DFFFFFF;
    };
    Pal palette() const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_ = nullptr;
    HBRUSH editBrush_ = nullptr;

    std::wstring title_;
    std::wstring message_;
    std::vector<std::wstring> lines_;
    bool withEntry_ = false;
    std::wstring okText_;
    std::wstring cancelText_;

    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    Surface glass_;
    Surface frame_;
    uint32_t avg_ = 0xFF1B1C20;

    std::vector<Hit> hits_;
    int hover_ = ACT_NONE;
    int pressed_ = ACT_NONE;
    bool dragging_ = false;
    POINT dragOffset_ = {0, 0};

    bool done_ = false;
    bool ok_ = false;
    std::wstring text_;
};

// ── 排版 ──

void DialogWindow::wrapText() {
    int limit = px(kDesignW) - px(kPad) * 2;
    float fs = (float)pt(10);
    lines_.clear();

    size_t start = 0;
    while (true) {
        size_t nl = message_.find(L'\n', start);
        std::wstring para = message_.substr(start, nl == std::wstring::npos ? std::wstring::npos
                                                                           : nl - start);
        if (para.empty()) {
            lines_.push_back(L"");
        } else {
            std::wstring line;
            for (wchar_t ch : para) {
                std::wstring probe = line + ch;
                if (!line.empty() && Surface::measureText(probe.c_str(), fs, kUI) > limit) {
                    lines_.push_back(line);
                    line = ch;
                } else {
                    line = probe;
                }
            }
            if (!line.empty()) lines_.push_back(line);
        }
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    if (lines_.empty()) lines_.push_back(L"");
}

void DialogWindow::layoutEntry() {
    if (!edit_) return;
    int pad = px(kPad);
    int titleH = title_.empty() ? 0 : px(22);
    int ey = pad + titleH + (titleH ? px(8) : 0) + (int)lines_.size() * px(kLineH) + px(6);
    int ex = pad + px(10);
    int ew = w_ - pad * 2 - px(20);
    int eh = px(kEntryH) - px(12);
    // 输入框是独立窗口，坐标要用屏幕坐标
    SetWindowPos(edit_, HWND_TOPMOST, x_ + ex, y_ + ey, ew, eh, SWP_NOACTIVATE);
}

// ── 玻璃 ──

void DialogWindow::buildGlass() {
    glass_.create(w_, h_);
    const int r = px(kRadius);
    Backdrop bd;
    RECT rc = {x_, y_, x_ + w_, y_ + h_};
    if (!Surface::capture(rc, 0, bd)) {
        avg_ = g_dark ? rgb(27, 28, 32) : rgb(242, 243, 245);
        glass_.fillRect(0, 0, w_, h_, avg_);
        return;
    }
    glass_.buildCard(bd, g_dark, r, 24, g_dark ? 40 : 34);
    avg_ = glass_.average();
    glass_.addBorder(r, g_dark);
    glass_.cutRoundCorners(r);
}

DialogWindow::Pal DialogWindow::palette() const {
    Pal p;
    uint32_t avg = avg_ ? avg_ : (g_dark ? rgb(27, 28, 32) : rgb(242, 243, 245));
    const uint32_t W = 0xFFFFFFFFu, B = 0xFF000000u;

    uint32_t mainCol = 0, dimCol = 0;
    readableText(avg, g_dark, mainCol, dimCol);
    p.text = mainCol;
    p.dim = dimCol;

    if (g_dark) {
        p.pill = blendColor(avg, W, 0.10) | 0xFF000000u;
        p.pillLine = blendColor(avg, W, 0.20) | 0xFF000000u;
        p.btn = blendColor(avg, W, 0.14) | 0xFF000000u;
        p.btnHot = blendColor(avg, W, 0.24) | 0xFF000000u;
        p.primary = blendColor(avg, W, 0.30) | 0xFF000000u;
        p.primaryHot = blendColor(avg, W, 0.40) | 0xFF000000u;
        p.edge = blendColor(avg, W, 0.30) | 0xFF000000u;
    } else {
        p.pill = blendColor(avg, B, 0.05) | 0xFF000000u;
        p.pillLine = blendColor(avg, B, 0.14) | 0xFF000000u;
        p.btn = blendColor(avg, B, 0.07) | 0xFF000000u;
        p.btnHot = blendColor(avg, B, 0.13) | 0xFF000000u;
        p.primary = blendColor(avg, B, 0.16) | 0xFF000000u;
        p.primaryHot = blendColor(avg, B, 0.24) | 0xFF000000u;
        p.edge = blendColor(avg, B, 0.22) | 0xFF000000u;
    }
    return p;
}

// ── 绘制 ──

void DialogWindow::drawButton(int x1, int y1, int x2, int y2, const std::wstring& label,
                              uint32_t fill, uint32_t hot, int action, uint32_t fg) {
    bool isHot = (hover_ == action);
    frame_.fillRoundRect(x1, y1, x2 - x1, y2 - y1, px(8), isHot ? hot : fill);
    int hh = px(kBtnH) / 2;
    RECT rc = {x1, (y1 + y2) / 2 - hh, x2, (y1 + y2) / 2 + hh};
    frame_.drawText(label.c_str(), rc, (float)pt(10), fg, 1, false, kUI);
    Hit h;
    h.rc.left = x1;
    h.rc.top = y1;
    h.rc.right = x2;
    h.rc.bottom = y2;
    h.action = action;
    hits_.push_back(h);
}

void DialogWindow::redraw() {
    if (!hwnd_) return;
    if (frame_.width() != w_ || frame_.height() != h_) frame_.create(w_, h_);
    frame_ = glass_;
    hits_.clear();
    Pal p = palette();

    // 背景正好是白色时 1px 内描边看不见，再沿边缘补一圈稍深的描边
    frame_.strokeRoundRect(px(1), px(1), w_ - px(2), h_ - px(2), px(kRadius), p.edge,
                           (float)px(1));

    int pad = px(kPad);
    int y = pad;

    if (!title_.empty()) {
        RECT rc = {pad, y, w_ - pad, y + px(22)};
        frame_.drawText(title_.c_str(), rc, (float)pt(11), p.text, 0, true, kUI);
        y += px(22) + px(8);
    }

    for (const auto& line : lines_) {
        if (!line.empty()) {
            RECT rc = {pad, y, w_ - pad, y + px(kLineH)};
            frame_.drawText(line.c_str(), rc, (float)pt(10), p.dim, 0, false, kUI);
        }
        y += px(kLineH);
    }

    // 输入框凹槽
    if (withEntry_) {
        int ey = y + px(6);
        frame_.fillRoundRect(pad, ey, w_ - pad * 2, px(kEntryH), px(8), p.pill);
        frame_.strokeRoundRect(pad, ey, w_ - pad * 2, px(kEntryH), px(8), p.pillLine, 1);
    }

    int bw = px(kBtnW), bh = px(kBtnH), gap = px(kBtnGap);
    int by2 = h_ - pad;
    int by1 = by2 - bh;
    int okX2 = w_ - pad;
    int okX1 = okX2 - bw;

    drawButton(okX1, by1, okX2, by2, okText_, p.primary, p.primaryHot, ACT_OK, p.text);
    if (!cancelText_.empty()) {
        int cX2 = okX1 - gap;
        int cX1 = cX2 - bw;
        drawButton(cX1, by1, cX2, by2, cancelText_, p.btn, p.btnHot, ACT_CANCEL, p.text);
    }

    pushFrame();
}

void DialogWindow::pushFrame() {
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

// ── 交互 ──

int DialogWindow::actionAt(int x, int y) const {
    for (const auto& h : hits_) {
        if (x >= h.rc.left && x <= h.rc.right && y >= h.rc.top && y <= h.rc.bottom) return h.action;
    }
    return ACT_NONE;
}

void DialogWindow::finish(bool ok) {
    if (done_) return;
    done_ = true;
    ok_ = ok;
    if (withEntry_ && edit_) {
        int len = GetWindowTextLengthW(edit_);
        std::wstring buf((size_t)len + 1, L'\0');
        GetWindowTextW(edit_, &buf[0], len + 1);
        buf.resize((size_t)len);
        text_ = buf;
    }
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT CALLBACK DialogWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DialogWindow* self = (DialogWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (DialogWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DialogWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLOREDIT: {
            // 输入框底色跟玻璃凹槽一致
            Pal p = palette();
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(rOf(p.text), gOf(p.text), bOf(p.text)));
            SetBkColor(dc, RGB(rOf(p.pill), gOf(p.pill), bOf(p.pill)));
            if (!editBrush_) editBrush_ = CreateSolidBrush(RGB(rOf(p.pill), gOf(p.pill), bOf(p.pill)));
            return (LRESULT)editBrush_;
        }

        case WM_MOUSEMOVE: {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (dragging_) {
                POINT cur = {};
                GetCursorPos(&cur);
                x_ = cur.x - dragOffset_.x;
                y_ = cur.y - dragOffset_.y;
                SetWindowPos(hwnd_, nullptr, x_, y_, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (edit_) layoutEntry();
                return 0;
            }
            int a = actionAt(mx, my);
            if (a != hover_) {
                hover_ = a;
                SetCursor(LoadCursorW(nullptr, a ? IDC_HAND : IDC_ARROW));
                redraw();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            int a = actionAt(mx, my);
            if (a) {
                pressed_ = a;
                return 0;
            }
            POINT cur = {};
            GetCursorPos(&cur);
            dragOffset_.x = cur.x - x_;
            dragOffset_.y = cur.y - y_;
            dragging_ = true;
            SetCapture(hwnd_);
            return 0;
        }

        case WM_LBUTTONUP: {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                return 0;
            }
            int a = actionAt(mx, my);
            if (a && a == pressed_) {
                pressed_ = ACT_NONE;
                finish(a == ACT_OK);
                return 0;
            }
            pressed_ = ACT_NONE;
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                finish(false);
                return 0;
            }
            if (wp == VK_RETURN) {
                finish(true);
                return 0;
            }
            return 0;

        case WM_CLOSE:
            finish(false);
            return 0;

        case WM_DESTROY:
            if (edit_) {
                DestroyWindow(edit_);
                edit_ = nullptr;
            }
            if (editBrush_) {
                DeleteObject(editBrush_);
                editBrush_ = nullptr;
            }
            hwnd_ = nullptr;
            done_ = true;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 打开 / 模态 ──

bool DialogWindow::open(HWND owner, const wchar_t* title, const wchar_t* message, bool withEntry,
                        const std::wstring& initial, const wchar_t* okText,
                        const wchar_t* cancelText) {
    owner_ = owner;
    title_ = title ? title : L"";
    message_ = message ? message : L"";
    withEntry_ = withEntry;
    okText_ = okText ? okText : L"确定";
    cancelText_ = cancelText ? cancelText : L"";

    wrapText();

    int pad = px(kPad);
    int titleH = title_.empty() ? 0 : px(22);
    int bodyH = (int)lines_.size() * px(kLineH);
    int entryH = withEntry ? px(kEntryH) : 0;

    w_ = px(kDesignW);
    h_ = pad + titleH + (titleH ? px(8) : 0) + bodyH + pad +
         (withEntry ? entryH + px(10) : 0) + px(kBtnH) + pad;

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int sw = work.right - work.left, sh = work.bottom - work.top;
    x_ = work.left + std::max(0, (sw - w_) / 2);
    y_ = work.top + std::max(0, (int)((sh - h_) * 0.40));

    if (!g_classReady) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        g_classReady = true;
    }

    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass,
                            title_.c_str(), WS_POPUP, x_, y_, w_, h_, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    if (withEntry) {
        // 输入框必须是「独立弹窗」而不是 WS_CHILD：主窗用 UpdateLayeredWindow 上屏，
        // 这种窗口不渲染子控件（文字整块消失，但键盘照样能输入，特别容易看漏）。
        // 做成 owner 为主窗的 WS_POPUP 就正常了（对照 search.cpp 的同样处理）。
        edit_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", initial.c_str(),
                                WS_POPUP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10,
                                hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (edit_) {
            HFONT f = CreateFontW(-(int)(pt(11)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, kUI);
            SendMessageW(edit_, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
        }
    }

    buildGlass();
    redraw();
    if (edit_) layoutEntry();

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    if (edit_) {
        ShowWindow(edit_, SW_SHOW);
        SetFocus(edit_);
    } else {
        SetFocus(hwnd_);
    }
    return true;
}

void DialogWindow::runModal() {
    if (!hwnd_) return;
    MSG msg;
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 输入框里的回车 / Esc 直接当确定 / 取消
        if (edit_ && msg.hwnd == edit_ && (msg.message == WM_KEYDOWN || msg.message == WM_CHAR)) {
            if (msg.wParam == VK_RETURN) {
                finish(true);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                finish(false);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

void setDialogDark(bool dark) { g_dark = dark; }

bool askString(HWND owner, const wchar_t* title, const wchar_t* message,
               const std::wstring& initial, std::wstring& out) {
    DialogWindow dlg;
    if (!dlg.open(owner, title, message, true, initial, L"确定", L"取消")) return false;
    dlg.runModal();
    if (!dlg.ok()) return false;
    out = dlg.text();
    return true;
}

bool askYesNo(HWND owner, const wchar_t* title, const wchar_t* message) {
    DialogWindow dlg;
    if (!dlg.open(owner, title, message, false, L"", L"确定", L"取消")) return false;
    dlg.runModal();
    return dlg.ok();
}

void showWarning(HWND owner, const wchar_t* title, const wchar_t* message) {
    DialogWindow dlg;
    if (!dlg.open(owner, title, message, false, L"", L"知道了", L"")) return;
    dlg.runModal();
}

}  // namespace hr
