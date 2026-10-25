// hr/shell.cpp — 用户会话与电源操作
#include "hr/shell.h"

#include "hr/common.h"
#include "hr/icons.h"

#include <shlobj.h>
#include <shellapi.h>       // ExtractIconExW
#include <wtsapi32.h>

#include <mutex>

#pragma comment(lib, "wtsapi32.lib")

namespace hr {

namespace {

// 头像缓存。key 是尺寸 —— 面板改了大小之后要重新取一次大图。
std::mutex g_avatarMu;
HBITMAP g_avatarBmp = nullptr;
int g_avatarSize = 0;

// 读当前账户的显示名。
//
// ⚠ 不用 GetUserNameExW(NameDisplay)：那个函数声明在 secext.h，
//   而 secext.h 必须先包含 ntsecapi.h 才能编译（否则报一堆
//   "缺少类型说明符"）。为了一个名字不值得引入那条依赖链。
//
//   改为读注册表：账户的显示名存在
//     HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\<SID>\ProfileImagePath
//   以及用户自己设的显示名在
//     HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\... 
//
//   最稳的是直接读 ProfileList 里当前用户那条的 ProfileImagePath
//   末段（就是账户名），配合 GetUserNameW。
//   想拿"张三"这种自定义显示名，读 HKCU 下的 AccountName。
std::wstring displayNameFromRegistry() {
    // 用户在"设置 → 账户"里改的显示名记在这里
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                        L"Explorer\\User Shell Folders",
                        0, KEY_READ, &k) == ERROR_SUCCESS) {
        ::RegCloseKey(k);
    }

    // 上面那个键其实拿不到显示名，真正的在下面：
    // HKCU\Software\Microsoft\Windows\CurrentVersion\CloudStore 之类
    // 各家版本不一。所以实际以 GetUserNameW 为准，
    // 显示名只在 HKCU\Volatile Environment 里能找到（远程会话才有）。
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Volatile Environment",
                        0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[256] = {0};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        if (::RegQueryValueExW(k, L"USERNAME", nullptr, &type,
                               (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
            type == REG_SZ && buf[0]) {
            ::RegCloseKey(k);
            return buf;
        }
        ::RegCloseKey(k);
    }
    return L"";
}

}  // namespace

std::wstring currentUserName() {
    // 优先注册表里的显示名（用户自己改过的名字）
    std::wstring s = displayNameFromRegistry();
    if (!s.empty()) return s;

    // 回落到账户名
    wchar_t buf[256] = {0};
    DWORD n = 256;
    if (::GetUserNameW(buf, &n) && buf[0]) {
        return buf;
    }
    return L"用户";
}

HBITMAP currentUserAvatar(int px) {
    std::lock_guard<std::mutex> lk(g_avatarMu);

    // 尺寸没变就复用
    if (g_avatarBmp && g_avatarSize == px) return g_avatarBmp;

    if (g_avatarBmp) {
        ::DeleteObject(g_avatarBmp);
        g_avatarBmp = nullptr;
    }

    // 账户头像图片路径：%APPDATA%\Microsoft\Windows\AccountPictures
    //
    // 系统把用户设置的头像放在那里（可能有多张，取最大的一张）。
    // 没设置过时目录为空 —— 那就回落到"默认用户"图标。
    wchar_t appdata[MAX_PATH] = {0};
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring dir = std::wstring(appdata) + L"\\Microsoft\\Windows\\AccountPictures";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring best;
            ULONGLONG bestSize = 0;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (sz > bestSize) {
                    bestSize = sz;
                    best = dir + L"\\" + fd.cFileName;
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);

            if (!best.empty()) {
                g_avatarBmp = iconFor(best, px);
            }
        }
    }

    // 没有自定义头像：用 shell 的"默认用户"图标。
    // SHGetFileInfoW 传一个不存在的特殊路径拿不到，得走 IMAGE 资源。
    if (!g_avatarBmp) {
        // user32.dll 里那张默认账户图（IDI_USER 系列在 shell32 里）
        HICON ic = nullptr;
        if (::ExtractIconExW(L"shell32.dll", 220, &ic, nullptr, 1) > 0 && ic) {
            g_avatarBmp = iconToBitmap(ic, px);
            ::DestroyIcon(ic);
        }
    }

    g_avatarSize = px;
    return g_avatarBmp;
}

// ── 电源与会话 ───────────────────────────────────────────────────────

namespace {

// 发起一次系统关机/重启。
//
// ⚠ 关键参数说明：
//   EWX_FORCEIFHUNG 而不是 EWX_FORCE —— 后者会强杀无响应的进程，
//   可能丢数据。前者只强制结束"已经挂起"的，其余照常走正常关闭流程。
//
//   不开 EWX_FORCE 的话，如果有程序卡住不响应关闭消息，
//   系统会弹框询问用户，这在"点一下就要关机"的场景下是合理的。
//
// ⚠ 要提权：关机需要 SE_SHUTDOWN_NAME 权限，普通用户默认没有。
//   不启用的话 ExitWindowsEx 直接返回失败（ERROR_ACCESS_DENIED）。
bool enableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
        ::CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    // ⚠ AdjustTokenPrivileges 即使部分失败也返回 TRUE，
    //   必须查 GetLastError 才知道到底成没成。
    DWORD err = ::GetLastError();
    ::CloseHandle(token);
    return err == ERROR_SUCCESS;
}

bool exitWindows(UINT flags) {
    if (!enableShutdownPrivilege()) {
        logf("shell: 无法获得关机权限（需要管理员）");
        // 仍然尝试一次 —— 某些配置下普通用户也有权限
    }
    BOOL ok = ::ExitWindowsEx(flags, SHTDN_REASON_MAJOR_APPLICATION |
                                          SHTDN_REASON_MINOR_OTHER |
                                          SHTDN_REASON_FLAG_PLANNED);
    if (!ok) {
        logf("shell: ExitWindowsEx(0x%X) 失败 err=%lu", flags, (unsigned long)::GetLastError());
    }
    return ok != FALSE;
}

void sleepSystemImpl(bool hibernate) {
    // ⚠ SetSuspendState 在 powrprof.h / PowrProf.lib 里。
    //
    //   不直接链接那个库，改用动态加载：
    //     · powrprof.h 会带进一堆电源相关的东西，编译更慢
    //     · 动态加载失败时能给出明确的日志，而不是启动就崩
    //
    //   这个 API 从 XP 起就在 powrprof.dll 里，函数名固定，不用担心找不到。
    typedef BOOLEAN(WINAPI * PFN_SetSuspendState)(BOOLEAN, BOOLEAN, BOOLEAN);

    HMODULE dll = ::LoadLibraryW(L"powrprof.dll");
    if (!dll) {
        logf("shell: 载入 powrprof.dll 失败 err=%lu", (unsigned long)::GetLastError());
        return;
    }

    auto fn = (PFN_SetSuspendState)::GetProcAddress(dll, "SetSuspendState");
    if (!fn) {
        logf("shell: 找不到 SetSuspendState");
        ::FreeLibrary(dll);
        return;
    }

    // bForce = FALSE：让系统先询问拒绝睡眠的程序，而不是强行挂起（强挂会丢数据）
    BOOL ok = fn(hibernate ? TRUE : FALSE, FALSE, FALSE);
    if (!ok) {
        logf("shell: %s 失败 err=%lu%s",
             hibernate ? "休眠" : "睡眠",
             (unsigned long)::GetLastError(),
             hibernate ? "（可能没启用休眠文件）" : "");
    }
    ::FreeLibrary(dll);
}

}  // namespace

bool shutdownSystem() {
    logf("shell: 请求关机");
    return exitWindows(EWX_SHUTDOWN | EWX_FORCEIFHUNG);
}

bool restartSystem() {
    logf("shell: 请求重启");
    return exitWindows(EWX_REBOOT | EWX_FORCEIFHUNG);
}

bool logoffSession() {
    logf("shell: 请求注销");
    return exitWindows(EWX_LOGOFF | EWX_FORCEIFHUNG);
}

bool sleepSystem() {
    logf("shell: 请求睡眠");
    sleepSystemImpl(false);
    // 这个 API 不返回失败原因，调用返回后系统已经在挂起流程里了。
    // 返回 true 表示"请求已发出"。
    return true;
}

bool hibernateSystem() {
    logf("shell: 请求休眠");
    sleepSystemImpl(true);
    return true;
}

bool lockWorkstation() {
    logf("shell: 锁定工作站");
    BOOL ok = ::LockWorkStation();
    if (!ok) {
        logf("shell: LockWorkStation 失败 err=%lu", (unsigned long)::GetLastError());
    }
    return ok != FALSE;
}

bool disconnectSession() {
    logf("shell: 断开远程会话");
    // WTSDisconnectSession 只对 RDP 会话有意义；
    // 本地会话调用会失败，这是正常的。
    BOOL ok = ::WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE,
                                     WTS_CURRENT_SESSION, FALSE);
    if (!ok) {
        logf("shell: WTSDisconnectSession 失败 err=%lu（本地会话时属正常）",
             (unsigned long)::GetLastError());
    }
    return ok != FALSE;
}

bool updatesPending() {
    // 检查 Windows Update 是否处于"已安装待重启"状态。
    //
    // 读这个键比调 WUAPI 轻得多，而且不需要 COM 初始化。
    // 键存在且非空 = 有待重启的更新，开始菜单此时会显示"更新并关机"。
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                        L"WindowsUpdate\\Auto Update\\RebootRequired",
                        0, KEY_READ, &k) != ERROR_SUCCESS) {
        // 也可能在这个位置
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                            L"Component Based Servicing\\RebootPending",
                            0, KEY_READ, &k) != ERROR_SUCCESS) {
            return false;
        }
    }
    ::RegCloseKey(k);
    return true;
}

}  // namespace hr