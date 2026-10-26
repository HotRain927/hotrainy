// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/settings.cpp — 玻璃设置窗口（逐行对照 ui/settings_window.py）
//
// 只有一条原则：Python 版能点、能改、能存的东西，这里一模一样；
// Python 版里没有入口的死代码（例如已经没人调用的 _change_color）不搬。
#include "hr/settings/settings.h"

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "hr/shared/common.h"
#include "hr/shared/hotkey.h"
#include "hr/tagbar/bar.h"
#include "hr/island/taskbar.h"

namespace hr {
// ★ 设置里选中的“要加壁纸的标签”。
//   存标签 id（而不是下标）：标签增删/调换顺序后不会指错人；
//   空串 = 跟随当前标签。id 不存在了（标签被删）也自动回到当前标签。
static std::string g_wallTabId;

namespace {

const wchar_t* kClass = L"HotrainySettings";
bool g_classReady = false;

const wchar_t* kUI = L"Microsoft YaHei";
const wchar_t* kGlyph = L"Segoe UI Symbol";

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kPolicyKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";

// ── 注册表：开机自启动（对照 main.py 的 RUN_KEY / RUN_VALUE） ──
bool autostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf), type = 0;
    bool ok = RegQueryValueExW(key, L"hotrainy", nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

// 读取自启项当前记录的路径（去掉引号）。不存在返回空。
static std::wstring autostartPath() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return std::wstring();
    }
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf), type = 0;
    LSTATUS st = RegQueryValueExW(key, L"hotrainy", nullptr, &type, (LPBYTE)buf, &cb);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) return std::wstring();

    std::wstring v(buf);
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

bool applyAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        logf("autostart: 打开注册表失败");
        return false;
    }
    bool ok = true;
    if (enabled) {
        // 每次都写当前真实路径，不因为"键已存在"就跳过。
        // 旧写法（if (!autostartEnabled()) 才写）有个真问题：exe 被移动或
        // 重新编译到新路径后，自启项会一直指向失效路径。指向不存在路径的
        // 自启项会让系统反复尝试解析它，有时会把父目录钉住导致删不掉。
        std::wstring cmd = L"\"" + exePath() + L"\"";
        ok = RegSetValueExW(key, L"hotrainy", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        if (ok) logf("autostart: 路径写为 %s", toUtf8(cmd).c_str());
    } else {
        RegDeleteValueW(key, L"hotrainy");
    }
    RegCloseKey(key);
    logf("autostart: %s -> %s", enabled ? "开" : "关", ok ? "已写入" : "失败");
    return ok;
}

// 自愈：自启项开着但指向的 exe 已不存在时，重写为当前路径。
// 在设置窗口打开时调用，避免留下死项。
static void healAutostart() {
    std::wstring cur = autostartPath();
    if (cur.empty()) return;   // 本来就没开

    // 路径有效就不动
    if (GetFileAttributesW(cur.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    std::wstring self = exePath();
    if (self.empty() || _wcsicmp(cur.c_str(), self.c_str()) == 0) return;

    logf("autostart: 指向失效路径 %s，重写为 %s", toUtf8(cur).c_str(), toUtf8(self).c_str());
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        std::wstring cmd = L"\"" + self + L"\"";
        RegSetValueExW(key, L"hotrainy", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}



// 改完注册表要重启资源管理器才生效（Python 版同样的做法）
void restartExplorer() {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"taskkill /f /im explorer.exe";
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                       &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 4000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    std::wstring start = L"explorer.exe";
    if (CreateProcessW(nullptr, &start[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    logf("startmenu: 已重启资源管理器");

    // ★★ 重新注入 ★★
    //
    //   踩过的坑（代码审计发现）：taskkill 掉 explorer 会**连带杀掉**
    //   注入进去的 GlassBoot / GlassTAP —— 它们是 explorer 进程里的模块。
    //   而这里原来从不重新注入，于是：
    //     透明失效（任务栏回到系统默认）
    //     岛台接管失效（开始/搜索按钮又回来了）
    //   用户看到的就是"一重启资源管理器，美化就没了"，
    //   而且没有任何日志提示。
    //
    //   TaskbarService::Refresh() 本来就是为这个场景写的
    //   （taskbar.cpp 里写着"explorer 重启后需要重新走一遍注入"），
    //   但在整个工程里**一次都没被调用过**。这里补上。
    //
    //   先等新 explorer 把任务栏建出来：注入要靠 FindWindow(Shell_TrayWnd)
    //   拿目标线程，任务栏没起来就会直接失败。
    for (int i = 0; i < 50; ++i) {          // 最多等 10 秒
        if (::FindWindowW(L"Shell_TrayWnd", nullptr)) break;
        ::Sleep(200);
    }

    taskbarService().Refresh();             // 透明（want 为真时重新注入）
    islandApplyState();                     // 岛台接管 + 透明，按当前开关重来一遍
    logf("startmenu: explorer 重启后已重新注入（透明 want=%d）",
         (int)taskbarService().transparent());
}

std::wstring randomSuffix() {
    unsigned v = (unsigned)(GetTickCount64() ^ (rand() * 2654435761u));
    wchar_t buf[16];
    swprintf(buf, 16, L"%08x", v);
    return buf;
}

bool pickImageFile(HWND owner, std::wstring& out) {
    wchar_t buf[1024] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter =
        L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif\0所有文件\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"选择壁纸";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

bool pickFolder(HWND owner, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        logf("folder: 创建选择框失败");
        return false;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"选择要加入搜索的文件夹");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// 去掉末尾反斜杠、正斜杠统一（对照 os.path.normpath）
std::wstring normPath(const std::wstring& in) {
    std::wstring s = in;
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring lowerOf(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

}  // namespace

// ── 打开 / 关闭 ─────────────────────────────────

bool SettingsWindow::open(HINSTANCE hInst, Config* cfg, bool dark, const std::string& version) {
    if (hwnd_) {
        SetForegroundWindow(hwnd_);
        return true;
    }
    if (!cfg) return false;

    hinst_ = hInst;
    cfg_ = cfg;
    dark_ = dark;
    version_ = L"hotrainy " + toWide(version);

    createWindow(version);
    if (!hwnd_) return false;
    logf("settings: 窗口已建 %dx%d @(%d,%d)", winW_, winH_, x_, y_);

    refreshRegistryCache();
    logf("settings: 注册表缓存已刷新");
    // 自启项自愈：如果它指向的 exe 已不存在（程序被移动过），
    // 重写为当前路径。指向失效路径的自启项会让系统反复尝试解析它。
    healAutostart();
    logf("settings: 自启项已检查");
    // 窗口还没显示，现在抓到的就是它背后的桌面
    buildGlass();
    logf("settings: 玻璃已烘焙 hasGlass=%d", (int)hasGlass_);
    redraw();
    logf("settings: 首帧已画");
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    logf("settings: 已打开 %dx%d @(%d,%d) 玻璃=%d", winW_, winH_, x_, y_, (int)hasGlass_);
    return true;
}

void SettingsWindow::createWindow(const std::string& version) {
    winW_ = px(720);
    // 720 → 796：右栏新增「文字颜色」三态 + 「岛台」开关两行
    winH_ = px(796);
    pad_ = px(24);
    colGap_ = px(28);
    rowH_ = px(38);
    headH_ = px(52);
    colW_ = (winW_ - pad_ * 2 - colGap_) / 2;

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int sw = work.right - work.left;
    int sh = work.bottom - work.top;
    // 小屏（例如 1366×768）压进工作区，宁可底部挤一点也不让窗口跑出屏幕
    if (sh > 0 && winH_ > sh - px(12)) winH_ = sh - px(12);
    x_ = work.left + std::max(0, (sw - winW_) / 2);
    // 略偏上：视觉重心比正中舒服
    y_ = work.top + std::max(0, (int)((sh - winH_) * 0.42));

    if (!g_classReady) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWindow::proc;
        wc.hInstance = hinst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
        g_classReady = true;
    }

    // ★ 窗口 = 内容 + 四周外阴影留白（x_/y_ 记的是**内容**左上角）
    const int shd0 = shadowInset();
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"设置",
                            WS_POPUP, x_ - shd0, y_ - shd0, winW_ + shd0 * 2,
                            winH_ + shd0 * 2, nullptr, nullptr, hinst_, this);
    if (!hwnd_) logf("settings: CreateWindow 失败 %lu", (unsigned long)GetLastError());
    (void)version;
}

void SettingsWindow::close() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

// ── 窗口过程 ────────────────────────────────────

LRESULT CALLBACK SettingsWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (SettingsWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE: {
            // ★ 客户区原点在「内容 - 留白」处（外阴影要留白）→ 命中判定用内容坐标
            int mx = GET_X_LPARAM(lp) - shadowInset(), my = GET_Y_LPARAM(lp) - shadowInset();
            if (dragging_) {
                POINT cur = {};
                GetCursorPos(&cur);
                int nx = dragOrigin_.x + (cur.x - dragStart_.x);
                int ny = dragOrigin_.y + (cur.y - dragStart_.y);
                if (abs(nx - x_) + abs(ny - y_) > px(2)) dragged_ = true;
                x_ = nx;
                y_ = ny;
                // ★ 纯移动：**内容像素一个都没变**（位置由 ULW 的落点决定，
                //   不在像素里），所以不需要重画整页控件 —— 只把上一帧原样
                //   重新提交一次即可。redraw() 每帧要画上百个控件（几十毫秒，
                //   用户感受到的卡就是它）；pushFrame() 只做位图 + 提交。
                // ⚠ SetWindowPos 的落点必须与 pushFrame() 里 ULW 的 dst 一致
                //   （都是 x_-留白）。写成内容坐标 x_ 的话，一帧内窗口先被摆到
                //   偏 21 像素的位置、再被 ULW 拉回来 —— 就是用户看到的
                //   "长按抖动频闪"。
                const int shd = shadowInset();
                SetWindowPos(hwnd_, nullptr, x_ - shd, y_ - shd, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                pushFrame();
                return 0;
            }
            const Hit* h = hitAt(mx, my);
            std::string key = h ? h->key : std::string();
            if (key != hoverKey_) {
                hoverKey_ = key;
                SetCursor(LoadCursorW(nullptr, key.empty() ? IDC_ARROW : IDC_HAND));
                redraw();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // ★ 客户区原点在「内容 - 留白」处（外阴影要留白）→ 命中判定用内容坐标
            int mx = GET_X_LPARAM(lp) - shadowInset(), my = GET_Y_LPARAM(lp) - shadowInset();
            const Hit* h = hitAt(mx, my);
            if (h) {
                pendingCb_ = h->cb;
                pendingKey_ = h->key;
                dragging_ = false;
                dragged_ = false;
            } else {
                pendingCb_ = nullptr;
                pendingKey_.clear();
                // 空白处 / 标题栏 → 拖窗口
                if (my <= headH_) {
                    POINT cur = {};
                    GetCursorPos(&cur);
                    dragStart_ = cur;
                    dragOrigin_.x = x_;
                    dragOrigin_.y = y_;
                    dragging_ = true;
                    dragged_ = false;
                    SetCapture(hwnd_);
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            // ★ 客户区原点在「内容 - 留白」处（外阴影要留白）→ 命中判定用内容坐标
            int mx = GET_X_LPARAM(lp) - shadowInset(), my = GET_Y_LPARAM(lp) - shadowInset();
            if (dragging_) {
                ReleaseCapture();
                dragging_ = false;
                if (dragged_) rebuildGlassOffscreen();  // 位置变了，玻璃底要重抓
                dragged_ = false;
                dragging_ = false;
                return 0;
            }
            std::function<void()> cb = pendingCb_;
            std::string key = pendingKey_;
            pendingCb_ = nullptr;
            pendingKey_.clear();
            if (!cb) return 0;
            const Hit* h = hitAt(mx, my);
            // 抬起时指针还在同一个控件上才算点击（和系统按钮一致）
            if (h && h->key == key) cb();
            return 0;
        }

        case WM_KEYDOWN:
            if (recording_) {
                recordKey(wp);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                close();
                return 0;
            }
            return 0;

        case WM_TIMER:
            if (wp == 1) {
                KillTimer(hwnd_, 1);
                errorTimer_ = 0;
                if (hotkeyError_) {
                    hotkeyError_ = false;
                    redraw();
                }
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (errorTimer_) KillTimer(hwnd_, 1);
            dragging_ = false;
            pendingCb_ = nullptr;
            hits_.clear();
            glass_.release();   // 真正归还内存（clear 只清零）
            frame_.release();
            hwnd_ = nullptr;
            logf("settings: 已关闭");
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 玻璃底 ──────────────────────────────────────

int SettingsWindow::shadowInset() const {
    return px(14);
}

void SettingsWindow::buildGlass() {
    glass_.create(winW_, winH_);
    const int radius = px(14);
    // ★ 与所有面板统一：纯透明底 —— 不抓屏、不模糊、不铺任何色，
    //   只保留一圈极细的立体边框（见 glass.cpp buildFlatPanel）。
    hasGlass_ = true;
    glass_.buildFlatPanel(cfg_ ? cfg_->panelOpacity : 100, radius, dark_);
    glass_.cutRoundCorners(radius);
    // avg_ 只用于判断文字对比度，这里给一个中性值即可
    avg_ = dark_ ? rgb(27, 28, 32) : rgb(242, 243, 245);

    // ★ 外阴影：与悬停面板同一套 —— 画布比内容大出四周留白，
    //   影子只画在内容矩形之外，内容坐标不动。
    // ⚠ 影子只跟尺寸/深浅有关，跟透明度无关 —— 拖透明度滑块时别重算它：
    //   那是一整张逐像素距离场，白算会让拖动发涩。
    const int s = shadowInset();
    if (shadow_.width() != winW_ + s * 2 || shadow_.height() != winH_ + s * 2) {
        shadow_.create(winW_ + s * 2, winH_ + s * 2);
        shadow_.clear();
        RECT content = {s, s, s + winW_, s + winH_};
        shadow_.applyOuterShadow(content, radius, dark_, px(2));
    }
}

void SettingsWindow::rebuildGlassOffscreen() {
    if (!hasGlass_ || !hwnd_) return;
    // 直接抓会把自己拍进去，所以先让开
    // ★ 不再"挪到屏幕外再挪回来"。
    //
    //   那趟搬运是当年"抓屏做玻璃底"留下的（抓之前要躲开自己）。
    //   现在的 buildGlass() 只画纯透明底 + 边框 + 雾化，**完全不抓屏** ——
    //   搬运纯属多余，而用户看到的就是
    //   "拖动放下的瞬间面板被关掉又打开一下"。
    buildGlass();
    redraw();
}

void SettingsWindow::pushFrame() {
    HDC screen = GetDC(nullptr);
    if (!screen) return;

    // ★★ 兜底（修"设置打开后整窗空白"）★★
    //   内存优化让 close() 里 release() 了这些画布（尺寸归零）。
    //   下面原本直接 outer_ = shadow_；shadow_ 若是 0x0，outer_ 就是 0x0，
    //   最后 UpdateLayeredWindow 提交 0 尺寸 —— 窗口看得见、内容全空，
    //   观感就是"设置打不开"。这里缺哪张补哪张。
    const int s0 = shadowInset();
    if (glass_.width() != winW_ || glass_.height() != winH_) {
        buildGlass();                      // 同时会把 shadow_ 按尺寸重建
    }
    if (shadow_.width() != winW_ + s0 * 2 || shadow_.height() != winH_ + s0 * 2) {
        buildGlass();
    }
    if (frame_.width() != winW_ || frame_.height() != winH_) {
        frame_.create(winW_, winH_);
    }
    if (glass_.width() != winW_ || shadow_.width() != winW_ + s0 * 2 ||
        frame_.width() != winW_) {
        // 实在补不齐就**不要提交** —— 提交 0 尺寸会把窗口弄成空白，
        // 保持上一帧反而更好。
        logf("settings: pushFrame 兜底失败 glass=%dx%d shadow=%dx%d frame=%dx%d (win=%dx%d)",
             glass_.width(), glass_.height(), shadow_.width(), shadow_.height(),
             frame_.width(), frame_.height(), winW_, winH_);
        ReleaseDC(nullptr, screen);
        return;
    }

    // ★ 让整块设置窗都能收鼠标：分层窗口的命中测试**只看 alpha**，
    //   而设置窗主体是纯透明的（buildFlatPanel 不铺底）——
    //   不抬 alpha，面板里点一下会直接穿透到背后的窗口
    //   （用户："设置跟岛台的出现问题了"）。
    RECT contentRc = {0, 0, winW_, winH_};
    frame_.ensureHitTestableArea(contentRc, px(14),
                                 hitTestAlphaFor(cfg_ ? cfg_->panelOpacity : 100));

    // ★ 内容 + 外阴影合成到"带留白"的画布再整幅提交；
    //   落点向左上各让出留白（dst = 内容位置 - 留白）。
    const int s = shadowInset();
    outer_ = shadow_;
    outer_.blendOver(frame_, s, s);

    HBITMAP bmp = outer_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT dst = {x_ - s, y_ - s};
        SIZE sz = {outer_.width(), outer_.height()};
        POINT src = {0, 0};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        // ★ 全局面板透明度：滑块 0-100 作用在这一帧的整体 alpha 上
        // ★ 不再按「面板透明度」缩放整帧 alpha：滑块现在只把**背景**加雾（见 glass.cpp buildFlatPanel），否则连文字/图标/边框一起变透明。
        const BOOL ulw = UpdateLayeredWindow(hwnd_, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        // 只在提交异常时打一行（正常时别刷屏，之前每帧一行太吵）
        if (!ulw) {
            logf("settings: ULW 提交失败 frame=%dx%d glass=%dx%d shadow=%dx%d outer=%dx%d sz=%dx%d",
                 frame_.width(), frame_.height(), glass_.width(), glass_.height(),
                 shadow_.width(), shadow_.height(), outer_.width(), outer_.height(),
                 sz.cx, sz.cy);
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

// ── 配色（对照 _palette） ───────────────────────

SettingsWindow::Palette SettingsWindow::palette() const {
    Palette c;
    uint32_t avg = avg_ ? avg_ : (dark_ ? rgb(27, 28, 32) : rgb(242, 243, 245));
    const uint32_t W = 0xFFFFFFFFu, B = 0xFF000000u;

    uint32_t mainCol = 0, dimCol = 0;
    if (hasGlass_) {
        readableText(avg, dark_, mainCol, dimCol);
        c.text = mainCol;
        c.dim = dimCol;
    } else {
        c.text = dark_ ? rgb(242, 243, 245) : rgb(27, 28, 31);
        c.dim = dark_ ? rgb(154, 160, 168) : rgb(90, 93, 99);
    }

    if (dark_) {
        c.hair = blendColor(avg, W, 0.18) | 0xFF000000u;
        c.rowHover = blendColor(avg, W, 0.07) | 0xFF000000u;
        c.trackOff = blendColor(avg, W, 0.22) | 0xFF000000u;
        c.trackOn = 0xFFF7F7FAu;
        c.knobOn = blendColor(avg, B, 0.35) | 0xFF000000u;
        c.knobOff = 0xFFF2F2F5u;
        c.segBg = blendColor(avg, W, 0.12) | 0xFF000000u;
        c.segOn = 0xFFF4F4F7u;
        c.segOnText = 0xFF1A1B1Eu;
        c.btnBg = blendColor(avg, W, 0.15) | 0xFF000000u;
        c.btnBgHot = blendColor(avg, W, 0.26) | 0xFF000000u;
        c.previewBg = blendColor(avg, B, 0.16) | 0xFF000000u;
        c.closeHot = blendColor(avg, W, 0.16) | 0xFF000000u;
    } else {
        c.hair = blendColor(avg, B, 0.14) | 0xFF000000u;
        c.rowHover = blendColor(avg, B, 0.05) | 0xFF000000u;
        c.trackOff = blendColor(avg, B, 0.16) | 0xFF000000u;
        c.trackOn = 0xFF1B1C1Fu;
        c.knobOn = 0xFFFFFFFFu;
        c.knobOff = 0xFFFFFFFFu;
        c.segBg = blendColor(avg, B, 0.07) | 0xFF000000u;
        c.segOn = 0xFFFFFFFFu;
        c.segOnText = 0xFF17181Bu;
        c.btnBg = blendColor(avg, B, 0.08) | 0xFF000000u;
        c.btnBgHot = blendColor(avg, B, 0.15) | 0xFF000000u;
        c.previewBg = blendColor(avg, B, 0.06) | 0xFF000000u;
        c.closeHot = blendColor(avg, B, 0.10) | 0xFF000000u;
    }
    // 半透明色带 alpha 才有效，这里统一把 alpha 补回不透明色对应的值
    return c;
}

// ── 基础绘制 ────────────────────────────────────

void SettingsWindow::textLeft(int x, int cy, const std::wstring& t, float sz, uint32_t col,
                              const wchar_t* fam, bool bold) {
    int hh = (int)(sz * 1.7f) + 2;
    RECT rc = {x, cy - hh, x + 5000, cy + hh};
    frame_.drawText(t.c_str(), rc, sz, col, 0, bold, fam);
}

void SettingsWindow::textCenter(int cx, int cy, const std::wstring& t, float sz, uint32_t col,
                                const wchar_t* fam, bool bold) {
    int hh = (int)(sz * 1.7f) + 2;
    RECT rc = {cx - 3000, cy - hh, cx + 3000, cy + hh};
    frame_.drawText(t.c_str(), rc, sz, col, 1, bold, fam);
}

void SettingsWindow::textRight(int rx, int cy, const std::wstring& t, float sz, uint32_t col) {
    int hh = (int)(sz * 1.7f) + 2;
    RECT rc = {rx - 5000, cy - hh, rx, cy + hh};
    frame_.drawText(t.c_str(), rc, sz, col, 2, false, kUI);
}

void SettingsWindow::hline(int x1, int y, int x2, uint32_t col) {
    frame_.fillRect(x1, y, x2 - x1, 1, col);
}

void SettingsWindow::hit(int x1, int y1, int x2, int y2, const std::string& key,
                         std::function<void()> cb) {
    Hit h;
    h.rc.left = x1;
    h.rc.top = y1;
    h.rc.right = x2;
    h.rc.bottom = y2;
    h.key = key;
    h.cb = std::move(cb);
    hits_.push_back(std::move(h));
}

bool SettingsWindow::isHot(const std::string& key) const {
    if (hoverKey_.empty()) return false;
    if (hoverKey_ == key) return true;
    return hoverKey_.size() > key.size() && hoverKey_.compare(0, key.size(), key) == 0 &&
           hoverKey_[key.size()] == ':';
}

const SettingsWindow::Hit* SettingsWindow::hitAt(int x, int y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (x >= it->rc.left && x <= it->rc.right && y >= it->rc.top && y <= it->rc.bottom)
            return &(*it);
    }
    return nullptr;
}

// ── 控件 ────────────────────────────────────────

void SettingsWindow::toggle(int xRight, int cy, bool value, const std::string& key,
                            std::function<void(bool)> onChange) {
    Palette c = palette();
    int tw = px(42), th = px(24);
    int x2 = xRight, x1 = x2 - tw;
    int y1 = cy - th / 2;

    frame_.fillRoundRect(x1, y1, tw, th, th / 2, value ? c.trackOn : c.trackOff);

    int d = px(18);
    int knobY = cy - d / 2;
    int knobX = value ? (x2 - px(3) - d) : (x1 + px(3));
    frame_.fillRoundRect(knobX, knobY, d, d, d / 2, value ? c.knobOn : c.knobOff);

    hit(x1 - px(6), y1 - px(6), x2 + px(6), y1 + th + px(6), key,
        [onChange, value]() { onChange(!value); });
}

void SettingsWindow::segmented(
    int xRight, int cy, const std::vector<std::pair<std::wstring, std::string>>& opts,
    const std::string& value, const std::string& key,
    std::function<void(const std::string&)> onChange) {
    Palette c = palette();
    int h = px(26), padX = px(14);

    std::vector<int> widths;
    int total = px(4);
    for (const auto& o : opts) {
        int w = std::max(px(44), Surface::measureText(o.first.c_str(), (float)pt(9), kUI) + padX * 2);
        widths.push_back(w);
        total += w;
    }

    int x1 = xRight - total;
    int y1 = cy - h / 2;
    frame_.fillRoundRect(x1, y1, total, h, h / 2, c.segBg);

    int x = x1 + px(2);
    for (size_t i = 0; i < opts.size(); ++i) {
        bool sel = (opts[i].second == value);
        if (sel) {
            frame_.fillRoundRect(x, y1 + px(2), widths[i], h - px(4), (h - px(4)) / 2, c.segOn);
        }
        textCenter(x + widths[i] / 2, cy, opts[i].first, (float)pt(9), sel ? c.segOnText : c.dim);
        std::string v = opts[i].second;
        hit(x, y1, x + widths[i], y1 + h, key + ":" + v, [onChange, v]() { onChange(v); });
        x += widths[i];
    }
}

void SettingsWindow::stepper(int xRight, int cy, int value, const std::string& key,
                             std::function<void(int)> onChange, int lo, int hi, int step, bool showNum) {
    Palette c = palette();
    int d = px(24), gap = px(8), numW = showNum ? px(34) : 0;
    int total = d * 2 + numW + gap * 2;
    int x1 = xRight - total;
    int y1 = cy - d / 2;

    for (int i = 0; i < 2; ++i) {
        int delta = (i == 0) ? -step : step;
        const wchar_t* glyph = (i == 0) ? L"−" : L"+";
        const char* sub = (i == 0) ? "dec" : "inc";
        int bx = x1 + (i == 0 ? 0 : d + numW + gap * 2);
        bool hot = isHot(key + ":" + sub);
        bool enabled = (value + delta) >= lo && (value + delta) <= hi;

        frame_.fillRoundRect(bx, y1, d, d, d / 2, (hot && enabled) ? c.btnBgHot : c.btnBg);
        textCenter(bx + d / 2, cy, glyph, (float)pt(11), enabled ? c.text : c.dim, kGlyph);
        if (enabled) {
            int nv = std::max(lo, std::min(hi, value + delta));
            hit(bx, y1, bx + d, y1 + d, key + ":" + sub, [onChange, nv]() { onChange(nv); });
        }
    }

    if (showNum) {
        textCenter(x1 + d + gap + numW / 2, cy, std::to_wstring(value), (float)pt(10), c.text);
    }
}

// ★ 面板透明度调节条（0-100）：轨道 + 已选 + 圆钮，整条切成 20 段可点（每段 5%）
void SettingsWindow::slider(int xRight, int cy, int value, const std::string& key,
                            std::function<void(int)> onChange) {
    Palette c = palette();
    const int w = px(190), h = px(6), knob = px(7);
    const int x1 = xRight - w, y1 = cy - h / 2;
    int v = value; if (v < 0) v = 0; if (v > 100) v = 100;

    frame_.fillRoundRect(x1, y1, w, h, h / 2, c.btnBg);
    int fillW = w * v / 100;
    if (fillW > 0) frame_.fillRoundRect(x1, y1, fillW, h, h / 2, c.text);
    int kx = x1 + fillW;
    frame_.fillRoundRect(kx - knob, cy - knob, knob * 2, knob * 2, knob, c.text);

    const int segs = 20;
    for (int i = 0; i < segs; ++i) {
        int sx1 = x1 + w * i / segs;
        int sx2 = x1 + w * (i + 1) / segs;
        int nv = (i * 100) / segs + (100 / segs) / 2;
        if (nv < 0) nv = 0;
        if (nv > 100) nv = 100;
        hit(sx1, cy - knob - px(2), sx2, cy + knob + px(2), key + ":seg",
            [onChange, nv]() { onChange(nv); });
    }
}

int SettingsWindow::button(int xRight, int cy, const std::wstring& text, const std::string& key,
                           std::function<void()> cb, int width) {
    Palette c = palette();
    int h = px(26);
    int w = width ? width : (Surface::measureText(text.c_str(), (float)pt(9), kUI) + px(24));
    int x1 = xRight - w;
    int y1 = cy - h / 2;
    frame_.fillRoundRect(x1, y1, w, h, h / 2, isHot(key) ? c.btnBgHot : c.btnBg);
    textCenter(x1 + w / 2, cy, text, (float)pt(9), c.text);
    hit(x1, y1, x1 + w, y1 + h, key, std::move(cb));
    return x1 - px(8);
}

void SettingsWindow::hotkeyPill(int xRight, int cy) {
    Palette c = palette();
    int h = px(26);

    std::wstring text;
    int w = 0;
    uint32_t fill = c.btnBg, col = c.text;

    if (recording_) {
        text = L"按下快捷键…";
        w = px(112);
        fill = c.btnBgHot;
        col = c.text;
    } else if (hotkeyError_) {
        text = L"已被占用";
        w = px(92);
        fill = c.btnBg;
        col = 0xFFE5645Bu;
    } else {
        text = toWide(cfg_->searchHotkey);
        w = std::max(px(104), Surface::measureText(text.c_str(), (float)pt(9), kUI) + px(26));
    }

    int x1 = xRight - w;
    int y1 = cy - h / 2;
    if (!recording_ && isHot("hotkey")) fill = c.btnBgHot;
    frame_.fillRoundRect(x1, y1, w, h, h / 2, fill);
    textCenter(x1 + w / 2, cy, text, (float)pt(9), col);
    hit(x1, y1, x1 + w, y1 + h, "hotkey", [this]() { startRecord(); });
}

// ── 行 / 分组 ───────────────────────────────────

int SettingsWindow::section(int x, int y, const std::wstring& title) {
    Palette c = palette();
    textLeft(x, y + px(8), title, (float)pt(9), c.dim);
    hline(x, y + px(24), x + colW_, c.hair);
    return y + px(32);
}

int SettingsWindow::row(int x, int y, const std::wstring& label, const std::string& key,
                        std::function<void(int, int)> drawControl) {
    Palette c = palette();
    int cy = y + rowH_ / 2;
    if (!key.empty() && isHot(key)) {
        frame_.fillRoundRect(x - px(8), y + px(1), colW_ + px(16), rowH_ - px(2), px(8),
                             c.rowHover);
    }
    textLeft(x, cy, label, (float)pt(10), c.text);
    drawControl(x + colW_, cy);
    return y + rowH_;
}

// ── 内容 ────────────────────────────────────────

void SettingsWindow::drawHeader() {
    Palette c = palette();
    textLeft(pad_, px(26), L"设置", (float)pt(12), c.text, kUI, true);
    textLeft(pad_ + px(52), px(27), version_, (float)pt(9), c.dim);

    int d = px(26);
    int x2 = winW_ - pad_ + px(4);
    int x1 = x2 - d;
    int y1 = px(26) - d / 2;
    bool hot = isHot("close");
    if (hot) frame_.fillRoundRect(x1, y1, d, d, d / 2, c.closeHot);
    textCenter((x1 + x2) / 2, px(26), L"✕", (float)pt(11), hot ? c.text : c.dim, kGlyph);
    hit(x1 - px(4), y1 - px(4), x2 + px(4), y1 + d + px(4), "close", [this]() { close(); });

    hline(pad_, headH_ - px(2), winW_ - pad_, c.hair);
}

void SettingsWindow::drawFooter() {
    Palette c = palette();
    int y = winH_ - px(26);
    textLeft(pad_, y, L"设置自动保存 · Esc 关闭", (float)pt(9), c.dim);
    button(winW_ - pad_ + px(4), y, L"退出程序", "quit", [this]() {
        if (onQuit) onQuit();
    });
}

void SettingsWindow::drawPreview(int x, int y) {
    Palette c = palette();
    int size = cfg_->shortcutFontSize;
    int h = px(46);

    frame_.fillRoundRect(x - px(8), y + px(2), colW_ + px(16), h - px(2), px(8), c.previewBg);

    int cy = y + px(2) + h / 2;
    int icon = px(std::max(14.0, size * 1.8));
    int ix = x + px(2);
    frame_.fillRoundRect(ix, cy - icon / 2, icon, icon, px(4), c.btnBgHot);
    textCenter(ix + icon / 2, cy, L"例", (float)pt(std::max(7, size - 3)), c.dim);

    int tx = ix + icon + px(9);
    int avail = (x + colW_) - tx - px(6);
    std::wstring fitted = Surface::fitText(L"示例程序", (float)pt(size), avail, kUI);
    textLeft(tx, cy, fitted, (float)pt(size), c.text);
}

void SettingsWindow::drawSearchTips(int x, int y) {
    Palette c = palette();
    textLeft(x, y, L"搜索框操作", (float)pt(9), c.dim);
    const wchar_t* tips[] = {
        L"拖动左上角放大镜 → 移动窗口（位置会记住）",
        L"拖动四边四角 → 调整大小（会记住）",
        L"↑ ↓ 选择，Enter 打开",
        L"Ctrl+Enter 打开所在位置，Esc 关闭",
        L"打开文件对话框时自动贴到下方",
    };
    int ty = y + px(20);
    for (const wchar_t* t : tips) {
        textLeft(x, ty, std::wstring(L"· ") + t, (float)pt(9), c.dim);
        ty += px(19);
    }
}

void SettingsWindow::drawTaskbarNotes(int x, int y) {
    Palette c = palette();
    TaskbarService& svc = taskbarService();

    // 第一行：实际状态（已透明 · 几个任务栏 · 实测色差），不是配置值
    std::wstring status = std::wstring(L"状态：") + svc.StatusText();
    textLeft(x, y, Surface::fitText(status.c_str(), (float)pt(9), colW_, kUI), (float)pt(9), c.dim);

    // 第二行：有兼容冲突就报冲突（更要紧），否则说清透明对系统的影响
    std::vector<std::wstring> notes = svc.CompatNotes();
    std::wstring line;
    uint32_t col = c.dim;
    if (!notes.empty()) {
        line = L"⚠ " + notes[0];
        col = 0xFFE5A24Bu;   // 暖橙：提示但不报警
    } else if (!svc.transparent()) {
        line = L"关 = 跟随系统，立刻还原成原来的主题，不留副作用";
    } else {
        // 如实告知代价：这个机制会一并改变开始菜单/通知中心的明暗
        line = L"已透明：背景透出壁纸，图标更清晰。系统 UI 转为亮色（应用主题不变）";
    }
    textLeft(x, y + px(17), Surface::fitText(line.c_str(), (float)pt(9), colW_, kUI), (float)pt(9),
             col);
}

// 把 g_wallTabId 解析成当前标签表里的下标；找不到就用“当前标签”
int SettingsWindow::wallTabIndex() const {
    for (size_t i = 0; i < cfg_->tabs.size(); ++i)
        if (cfg_->tabs[i].id == g_wallTabId) return (int)i;
    return currentTabIndex();
}

void SettingsWindow::drawWallpaperRow(int xRight, int cy) {
    int idx = wallTabIndex();
    bool has = idx >= 0 && !cfg_->tabs[(size_t)idx].wallpaper.empty();
    int left = xRight;
    if (has) left = button(xRight, cy, L"清除", "wall_clear", [this]() { clearWallpaper(); });
    button(left, cy, L"选择图片…", "wall_pick", [this]() { pickWallpaper(); });
}

void SettingsWindow::drawSizeRow(int xRight, int cy) {
    Palette c = palette();
    bool custom = cfg_->searchSizeCustom || cfg_->searchPosCustom;
    std::wstring txt = std::to_wstring(cfg_->searchW) + L" × " + std::to_wstring(cfg_->searchH);
    if (custom) {
        int left = button(xRight, cy, L"恢复默认", "size_reset", [this]() { resetSearchSize(); });
        textRight(left, cy, txt, (float)pt(9), c.dim);
    } else {
        textRight(xRight - px(6), cy, txt, (float)pt(9), c.dim);
    }
}

void SettingsWindow::redraw() {
    if (!hwnd_ || winW_ <= 0) return;
    if (frame_.width() != winW_ || frame_.height() != winH_) frame_.create(winW_, winH_);
    // ★ 画布可能在 close() 里被 release() 掉了（内存优化）—— 这里补一次重建，
    //   否则重新打开时 glass_ 是 0x0，分层窗口提交出去就是"看得见窗口、
    //   看不见内容"，观感就是"设置打不开"。（用户实测就是这个。）
    if (glass_.width() != winW_ || glass_.height() != winH_) buildGlass();
    frame_ = glass_;
    hits_.clear();

    drawHeader();

    int top = headH_ + px(10);
    int lx = pad_;
    int rx = pad_ + colW_ + colGap_;

    // ── 左栏：标签栏 ──
    int y = section(lx, top, L"标签栏");
    y = row(lx, y, L"展开方式", "hover_mode", [this](int x, int cy) {
        segmented(x, cy, {{L"悬停", "1"}, {L"点击", "0"}}, cfg_->hoverMode ? "1" : "0", "hover_mode",
                  [this](const std::string& v) { setBool("hover_mode", v == "1"); });
    });
    y = row(lx, y, L"自动隐藏", "auto_hide", [this](int x, int cy) {
        toggle(x, cy, cfg_->autoHide, "auto_hide",
               [this](bool v) { setBool("auto_hide", v); });
    });

    // ── 左栏：外观 ──
    y += px(14);
    y = section(lx, y, L"外观");
    y = row(lx, y, L"液态玻璃", "glass_mode", [this](int x, int cy) {
        toggle(x, cy, cfg_->glassMode, "glass_mode",
               [this](bool v) { setBool("glass_mode", v); });
    });
    y = row(lx, y, L"壁纸模式", "wallpaper_mode", [this](int x, int cy) {
        toggle(x, cy, cfg_->wallpaperMode, "wallpaper_mode",
               [this](bool v) { setBool("wallpaper_mode", v); });
    });

    // ★ 先选“给哪个标签加壁纸”。
    //   选项每次重绘都从当前标签表重建 —— 标签增删后自动跟着变。
    if (!cfg_->tabs.empty()) {
        std::vector<std::pair<std::wstring, std::string>> wopts;
        for (size_t wi = 0; wi < cfg_->tabs.size(); ++wi)
            wopts.push_back({cfg_->tabs[wi].name, std::to_string(wi)});
        const int curSel = wallTabIndex();
        y = row(lx, y, L"壁纸适用标签", "wall_tab", [this, wopts, curSel](int x, int cy) {
            segmented(x, cy, wopts, std::to_string(curSel < 0 ? 0 : curSel), "wall_tab",
                      [this](const std::string& v) {
                          const int wi = atoi(v.c_str());
                          if (wi >= 0 && wi < (int)cfg_->tabs.size())
                              g_wallTabId = cfg_->tabs[(size_t)wi].id;   // 存 id，不存下标
                          logf("settings: 壁纸适用标签 -> %s", g_wallTabId.c_str());
                          redraw();
                      });
        });
    }

    // ★ 面板透明度（所有面板共用一个值：0 = 只看得见图标与文字，100 = 完全可见）
    y = row(lx, y, L"面板透明度", "panel_opacity", [this](int x, int cy) {
        slider(x, cy, cfg_->panelOpacity, "panel_opacity", [this](int v) {
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            cfg_->panelOpacity = v;
            logf("settings: 面板透明度 -> %d", v);
            saveConfig(*cfg_);
            if (onSettingChanged) onSettingChanged("panel_opacity");
            // ★ 立刻按新透明度重烘焙玻璃底再出帧。
            //   雾是烘焙在 glass_ 里的，只 redraw() 的话它还是旧浓度 ——
            //   用户："拖动条之后的瞬间就要把雾化效果刷新出来"。
            buildGlass();
            redraw();
        });
    });

    int tabIdx = wallTabIndex();
    std::wstring tabName = tabIdx >= 0 ? cfg_->tabs[(size_t)tabIdx].name : L"—";
    y = row(lx, y, L"壁纸（" + tabName + L"）", "", [this](int x, int cy) {
        drawWallpaperRow(x, cy);
    });
    Palette c = palette();
    textLeft(lx, y + px(6) + pt(9) / 2, L"壁纸只作用于快捷方式面板，标签栏保持玻璃外观",
             (float)pt(9), c.dim);

    // ── 左栏：搜索 ──
    y += px(40);
    y = section(lx, y, L"搜索");
    y = row(lx, y, L"启用搜索", "search_enabled", [this](int x, int cy) {
        toggle(x, cy, cfg_->searchEnabled, "search_enabled",
               [this](bool v) { setBool("search_enabled", v); });
    });
    y = row(lx, y, L"呼出快捷键", "hotkey", [this](int x, int cy) { hotkeyPill(x, cy); });
    // ★ 「搜索目录」与「索引范围」两个控件已按用户要求删除：
    //   搜索固定为**全盘**、目录不可更改。范围在 config 载入时被强制成 "all"
    //   （见 config.cpp），这样搜索行为唯一，也不再有"范围选错所以搜不到"的坑。
    y = row(lx, y, L"搜索框大小", "size", [this](int x, int cy) { drawSizeRow(x, cy); });
    y = row(lx, y, L"跟随资源管理器", "search_follow_explorer", [this](int x, int cy) {
        toggle(x, cy, cfg_->searchFollow, "search_follow_explorer",
               [this](bool v) { setBool("search_follow_explorer", v); });
    });
    y = row(lx, y, L"文件对话框弹出", "search_dialog_auto", [this](int x, int cy) {
        toggle(x, cy, cfg_->searchDialogAuto, "search_dialog_auto",
               [this](bool v) { setBool("search_dialog_auto", v); });
    });

    // ── 右栏：快捷方式 ──
    int y2 = section(rx, top, L"快捷方式");
    y2 = row(rx, y2, L"文字大小", "font", [this](int x, int cy) {
        stepper(x, cy, cfg_->shortcutFontSize, "font",
                [this](int v) { setInt("shortcut_font_size", v); }, kFontMin, kFontMax);
    });
    drawPreview(rx, y2);
    y2 += px(46) + px(6);

    // ── 右栏：系统 ──
    y2 += px(14);
    y2 = section(rx, y2, L"系统");
    y2 = row(rx, y2, L"音频波动条", "audio_bar", [this](int x, int cy) {
        toggle(x, cy, cfg_->audioBar, "audio_bar",
               [this](bool v) { setBool("audio_bar", v); });
    });
               y2 = row(rx, y2, L"音频条横向长度", "audio_bar_w", [this](int x, int cy) {
                   stepper(x, cy, cfg_->audioBarW, "audio_bar_w",
                           [this](int v) { setInt("audio_bar_w", v); }, 1, 4000, 50, false);
               });
               y2 = row(rx, y2, L"音频条纵向长度", "audio_bar_h", [this](int x, int cy) {
                   stepper(x, cy, cfg_->audioBarH, "audio_bar_h",
                           [this](int v) { setInt("audio_bar_h", v); }, 1, 4000, 50, false);
               });
               y2 = row(rx, y2, L"音频条色调", "audio_tone", [this](int x, int cy) {
                   // ⚠ 选项值必须是**数字**：segmented 把 "key:value" 交给派发器，
                   //   派发器按整数解析 value（hover_mode 用的就是 "1"/"0"）。
                   //   这里原来写 "black"/"yellow" 会被解析成 0，于是点哪个都变黑。
                   const int toneIdx = (cfg_->audioTone == "black")  ? 0
                                     : (cfg_->audioTone == "white")  ? 1
                                     : (cfg_->audioTone == "blue")   ? 2
                                     : (cfg_->audioTone == "red")    ? 3 : 4;
                   segmented(x, cy,
                             {{L"黑", "0"}, {L"白", "1"}, {L"蓝", "2"},
                              {L"红", "3"}, {L"黄", "4"}},
                             std::to_string(toneIdx), "audio_tone",
                             [this](const std::string& v) {
                                 setInt("audio_tone", std::stoi(v));
                             });
               });
    y2 = row(rx, y2, L"开机自启动", "autostart", [this](int x, int cy) {
        toggle(x, cy, cacheAutostart_, "autostart",
               [this](bool v) { setBool("autostart", v); });
    });

    // ── 右栏：任务栏 ──
    // 开 = 任务栏透明（背景透出壁纸，图标保持清晰）
    // 关 = 跟随系统，并把系统主题还原成打开前的样子。
    //
    // 机制说明：实测 DWM / accent / blur / backdrop 对 Win11 任务栏全部无效，
    // 唯一有效路径是切换系统明暗主题（亮色主题下任务栏不再被重度压暗）。
    // 详见 hr/island/taskbar.h 与 src/tb/xaml/ 下的说明。
    y2 += px(14);
    y2 = section(rx, y2, L"任务栏");
    y2 = row(rx, y2, L"透明", "taskbar_transparent", [this](int x, int cy) {
        toggle(x, cy, cfg_->taskbarTransparent, "taskbar_transparent",
               [this](bool v) { setBool("taskbar_transparent", v); });
    });

    // 岛台：从标签栏最左边的按钮唤出的面板。
    // 开启后任务栏的「开始」和「搜索」会被隐藏，入口由岛台接管。
    y2 = row(rx, y2, L"岛台", "island_enabled", [this](int x, int cy) {
        toggle(x, cy, cfg_->islandEnabled, "island_enabled",
               [this](bool v) { setBool("island_enabled", v); });
    });

    // 托盘文字颜色。
    //
    // 透明后背景是壁纸，系统默认的白字在浅色壁纸上会看不清。
    // 「自动」按任务栏背后的实际亮度决定深浅；
    // 另两档是壁纸明暗反差很大时的手动兜底。
    y2 = row(rx, y2, L"文字颜色", "taskbar_text_color", [this](int x, int cy) {
        segmented(x, cy,
                  {{L"深色", "dark"}, {L"浅色", "light"}},
                  toUtf8(cfg_->taskbarTextColor), "taskbar_text_color",
                  [this](const std::string& v) {
                      cfg_->taskbarTextColor = toWide(v);
                      taskbarService().SetTextColor(cfg_->taskbarTextColor);
                      taskbarService().ApplyTextColor();
                      // 岛台的文字也跟着变 —— 两边用同一套深浅判断，
                      // 不同步的话会出现"任务栏黑字、岛台白字"的割裂。
                      islandSetTextMode(cfg_->taskbarTextColor);
                      notify("taskbar_text_color");
                  });
    });

    drawTaskbarNotes(rx, y2 + px(10));

    drawSearchTips(rx, y2 + px(62));
    drawFooter();

    pushFrame();
}

// ── 读写 ────────────────────────────────────────

bool SettingsWindow::getBool(const std::string& key) const {
    if (key == "hover_mode") return cfg_->hoverMode;
    if (key == "auto_hide") return cfg_->autoHide;
    if (key == "glass_mode") return cfg_->glassMode;
    if (key == "wallpaper_mode") return cfg_->wallpaperMode;
    if (key == "audio_bar") return cfg_->audioBar;
    if (key == "audio_bar_w") return cfg_->audioBarW;
    if (key == "audio_bar_h") return cfg_->audioBarH;
    if (key == "search_enabled") return cfg_->searchEnabled;
    if (key == "search_follow_explorer") return cfg_->searchFollow;
    if (key == "search_dialog_auto") return cfg_->searchDialogAuto;
    if (key == "autostart") return cacheAutostart_;
    if (key == "taskbar_transparent") return cfg_->taskbarTransparent;
    if (key == "island_enabled") return cfg_->islandEnabled;
    return false;
}

int SettingsWindow::getInt(const std::string& key) const {
    if (key == "shortcut_font_size") return cfg_->shortcutFontSize;
    return 0;
}

std::wstring SettingsWindow::getStr(const std::string& key) const {
    if (key == "search_hotkey") return toWide(cfg_->searchHotkey);
    return L"";
}

void SettingsWindow::notify(const std::string& key) {
    bool ok = true;
    if (onSettingChanged) ok = onSettingChanged(key);
    saveConfig(*cfg_);
    redraw();
    (void)ok;
}

void SettingsWindow::setBool(const std::string& key, bool v) {
    if (key == "hover_mode") cfg_->hoverMode = v;
    else if (key == "auto_hide") cfg_->autoHide = v;
    // main.py:552 `_set_glass_mode`：开玻璃就关壁纸。
    // 注意只互斥这两个布尔的取值，外观上仍是「或」（glassAppearanceOn），
    // 壁纸模式下标签栏照样是玻璃。
    else if (key == "glass_mode") {
        cfg_->glassMode = v;
        if (v) cfg_->wallpaperMode = false;
    // main.py:681 `_set_wallpaper_mode`：开壁纸就关玻璃
    } else if (key == "wallpaper_mode") {
        cfg_->wallpaperMode = v;
        if (v) cfg_->glassMode = false;
    }
    else if (key == "audio_bar") cfg_->audioBar = v;
    else if (key == "audio_bar_w") cfg_->audioBarW = v;
    else if (key == "audio_bar_h") cfg_->audioBarH = v;
    else if (key == "search_enabled") cfg_->searchEnabled = v;
    else if (key == "search_follow_explorer") cfg_->searchFollow = v;
    else if (key == "search_dialog_auto") cfg_->searchDialogAuto = v;
    else if (key == "autostart") {
        applyAutostart(v);
        cacheAutostart_ = autostartEnabled();
    } else if (key == "taskbar_transparent") {
        cfg_->taskbarTransparent = v;
        taskbarService().SetTextColor(cfg_->taskbarTextColor);
        // ★ 统一状态机：任何开关变化都调 applyTaskbarState()，
        //   它重新读两个开关的值，选对应的处理函数。
        islandApplyState();
    } else if (key == "island_enabled") {
        cfg_->islandEnabled = v;
        // ★ 统一状态机
        islandApplyState();
    }
    logf("settings: %s = %s", key.c_str(), v ? "开" : "关");
    notify(key);
}

void SettingsWindow::setInt(const std::string& key, int v) {
    if (key == "shortcut_font_size") {
        v = std::max(kFontMin, std::min(kFontMax, v));
        cfg_->shortcutFontSize = v;
        logf("settings: 快捷方式文字大小 = %d", v);
    }
    else if (key == "audio_bar_w") {
        cfg_->audioBarW = std::max(1, std::min(4000, v));
        logf("settings: 音频条横向长度 = %d", cfg_->audioBarW);
        saveConfig(*cfg_);   // ★ 立即存盘，否则重启后又回到旧尺寸
    }
    else if (key == "audio_bar_h") {
        cfg_->audioBarH = std::max(1, std::min(4000, v));
        logf("settings: 音频条纵向长度 = %d", cfg_->audioBarH);
        saveConfig(*cfg_);
    }
    else if (key == "audio_tone") {
        // ★ 这里才是 setInt 的路径，赋值必须在这儿（上次误插进了 setBool，
        //   导致点哪个色调都不会改变 cfg_->audioTone，日志永远打印 black）。
        cfg_->audioTone = (v == 0) ? "black" : (v == 1) ? "white"
                        : (v == 2) ? "blue"  : (v == 3) ? "red" : "yellow";
        logf("settings: 音频条色调 = %s", cfg_->audioTone.c_str());
        saveConfig(*cfg_);   // 立即存盘，重启后保持
    }
    // 叠加 alpha 反而会让图标变糊（实测锐度 7.46 → 3.60）。
    notify(key);
}

bool SettingsWindow::setHotkey(const std::wstring& spec) {
    std::string old = cfg_->searchHotkey;
    cfg_->searchHotkey = toUtf8(spec);
    bool ok = true;
    if (onSettingChanged) ok = onSettingChanged("search_hotkey");
    if (!ok) cfg_->searchHotkey = old;
    logf("settings: 搜索快捷键 %s -> %s", toUtf8(spec).c_str(), ok ? "已生效" : "被占用");
    saveConfig(*cfg_);
    return ok;
}

void SettingsWindow::startRecord() {
    if (recording_) return;
    recording_ = true;
    hotkeyError_ = false;
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    redraw();
}

void SettingsWindow::recordKey(WPARAM vk) {
    if (vk == VK_ESCAPE) {
        recording_ = false;
        redraw();
        return;
    }
    // 只按住了修饰键 → 继续等
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return;

    // ★ 中文输入法激活时，系统送来的是 VK_PROCESSKEY(0xE5) —— 真实按键只有输入法知道。
    //   不处理的话会把 "0xE5" 当成一个快捷键记下来（一个永远不会触发的键），
    //   用户看到的就是"改了没反应/一直被占用"。
    //   ImmGetVirtualKey 就是把真实按键问回来的官方办法；动态取，不新增链接依赖。
    if (vk == VK_PROCESSKEY) {
        using ImmGetVirtualKeyFn = UINT(WINAPI*)(HWND);
        static ImmGetVirtualKeyFn getReal = []() -> ImmGetVirtualKeyFn {
            HMODULE imm = ::LoadLibraryW(L"imm32.dll");
            return imm ? (ImmGetVirtualKeyFn)::GetProcAddress(imm, "ImmGetVirtualKey") : nullptr;
        }();
        const UINT real = getReal ? getReal(hwnd_) : 0;
        if (real == 0 || real == VK_PROCESSKEY) {
            logf("settings: 快捷键录制：输入法吃掉了这个键，且问不回真实按键 → 忽略");
            return;
        }
        vk = real;
    }

    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        mods |= MOD_WIN;
    // 不带修饰键的组合会抢走正常打字，不收
    if (!mods) return;

    std::string spec = formatHotkey(mods, (UINT)vk);
    recording_ = false;
    hotkeyError_ = !setHotkey(toWide(spec));
    if (hotkeyError_) {
        errorTimer_ = SetTimer(hwnd_, 1, 2600, nullptr);
    }
    redraw();
}

int SettingsWindow::currentTabIndex() const {
    if (!cfg_ || cfg_->tabs.empty()) return -1;
    if (activeTab_ >= 0 && activeTab_ < (int)cfg_->tabs.size()) return activeTab_;
    return 0;
}

void SettingsWindow::refreshRegistryCache() {
    cacheAutostart_ = autostartEnabled();
}

// ── 壁纸 / 目录 / 尺寸 ──────────────────────────

void SettingsWindow::pickWallpaper() {
    int idx = wallTabIndex();
    if (idx < 0) return;

    // 文件框会盖在最上层窗口下面，先把置顶撤掉
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    std::wstring src;
    bool picked = pickImageFile(hwnd_, src);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (!picked) {
        redraw();
        return;
    }

    std::wstring ext;
    size_t dot = src.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = lowerOf(src.substr(dot));
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".bmp" &&
        ext != L".webp" && ext != L".gif") {
        logf("settings: 壁纸格式不支持 %s", toUtf8(ext).c_str());
        return;
    }

    Tab& tab = cfg_->tabs[(size_t)idx];
    std::wstring folder = configDir() + L"\\Wallpapers";
    ensureDir(folder);

    // 先清旧的（换格式后也不残留）
    std::wstring prefix = toWide(tab.id) + L"_";
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name.compare(0, prefix.size(), prefix) == 0) {
                DeleteFileW((folder + L"\\" + name).c_str());
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    std::wstring dst = folder + L"\\" + prefix + randomSuffix() + ext;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        logf("settings: 复制壁纸失败 %lu", (unsigned long)GetLastError());
        return;
    }
    tab.wallpaper = dst;
    logf("settings: 标签 %s 壁纸 -> %s", tab.id.c_str(), toUtf8(dst).c_str());
    notify("wallpaper");
}

void SettingsWindow::clearWallpaper() {
    int idx = wallTabIndex();
    if (idx < 0) return;
    Tab& tab = cfg_->tabs[(size_t)idx];
    std::wstring folder = configDir() + L"\\Wallpapers";
    std::wstring prefix = toWide(tab.id) + L"_";
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name.compare(0, prefix.size(), prefix) == 0) {
                DeleteFileW((folder + L"\\" + name).c_str());
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    tab.wallpaper.clear();
    logf("settings: 标签 %s 壁纸已清除", tab.id.c_str());
    notify("wallpaper");
}

void SettingsWindow::resetSearchSize() {
    cfg_->searchSizeCustom = false;
    cfg_->searchPosCustom = false;
    cfg_->searchW = 560;
    cfg_->searchH = 420;
    cfg_->searchX = -1;
    cfg_->searchY = -1;
    logf("settings: 搜索框尺寸/位置恢复默认");
    notify("search_size");
}

}  // namespace hr
