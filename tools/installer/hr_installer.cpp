// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr_installer.cpp — hotrainy 自解压安装程序（单文件 exe）
//
// 设计：
//   · 本 exe = 安装程序 stub + 末尾追加的载荷容器
//   · 载荷格式（小端）：
//       每个文件: [u32 名字字节数][名字(UTF-8)][u64 大小][数据...]
//       末尾 16 字节 footer: [u32 magic='HRF1'][u32 文件数][u64 载荷起始偏移]
//   · 双击 = 图形界面：选安装目录 → 解压 → 建快捷方式 → 可选开机自启 → 可选启动
//   · 命令行： /S 静默（不建快捷方式、不写自启、不启动）
//              /D=<目录> 指定安装目录（可与 /S 合用，便于自动化测试）
//
// 只依赖系统库（静态 CRT），免管理员：默认装到当前用户的
// %LOCALAPPDATA%\Programs\hotrainy。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

static const DWORD kMagic = 0x31524648;  // 'HRF1'

struct Entry {
    std::wstring name;
    unsigned long long size;
    unsigned long long offset;  // 载荷文件内偏移
};

static std::wstring exePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

static std::wstring dirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

static void ensureDir(const std::wstring& d) {
    ::CreateDirectoryW(d.c_str(), nullptr);
}

static bool readAt(HANDLE h, unsigned long long off, void* buf, DWORD n) {
    LARGE_INTEGER li{};
    li.QuadPart = (LONGLONG)off;
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ::ReadFile(h, buf, n, &got, nullptr) && got == n;
}

static int fail(const wchar_t* msg) {
    ::MessageBoxW(nullptr, msg, L"hotrainy 安装程序", MB_ICONERROR | MB_OK);
    return 1;
}

// 用系统对话框选目录，并把初始位置指到 def
static bool pickFolder(std::wstring& out, const std::wstring& def) {
    struct Ctx { const std::wstring* def; };
    Ctx ctx{&def};
    wchar_t picked[MAX_PATH * 2] = {};
    BROWSEINFOW bi{};
    bi.hwndOwner = nullptr;
    bi.lpszTitle = L"选择 hotrainy 的安装位置（会在这个文件夹里放程序、说明和卸载程序）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    bi.lpfn = [](HWND hwnd, UINT msg, LPARAM lp, LPARAM data) -> int {
        if (msg == BFFM_INITIALIZED) {
            Ctx* c = (Ctx*)data;
            ::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, (LPARAM)c->def->c_str());
        }
        return 0;
    };
    bi.lParam = (LPARAM)&ctx;
    LPITEMIDLIST idl = ::SHBrowseForFolderW(&bi);
    if (!idl) return false;
    bool ok = ::SHGetPathFromIDListW(idl, picked) != FALSE;
    ::CoTaskMemFree(idl);
    if (!ok || !picked[0]) return false;
    out = picked;
    return true;
}

static bool writeFileBytes(const std::wstring& path, HANDLE src, unsigned long long off,
                           unsigned long long size) {
    HANDLE out = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    std::vector<char> buf(1 << 16);
    unsigned long long left = size;
    while (left > 0) {
        DWORD chunk = (DWORD)((left > buf.size()) ? buf.size() : left);
        if (!readAt(src, off, buf.data(), chunk)) { ::CloseHandle(out); return false; }
        DWORD wrote = 0;
        if (!::WriteFile(out, buf.data(), chunk, &wrote, nullptr) || wrote != chunk) {
            ::CloseHandle(out);
            return false;
        }
        off += chunk;
        left -= chunk;
    }
    ::CloseHandle(out);
    return true;
}

static void makeShortcut(const std::wstring& lnk, const std::wstring& target,
                         const std::wstring& workdir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&sl)))
        return;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workdir.c_str());
    sl->SetDescription(L"hotrainy");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

static std::wstring envPath(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = ::GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf) : std::wstring();
}

int wmain() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    bool silent = false;
    std::wstring dst;

    for (int i = 1; i < argc && argv; ++i) {
        std::wstring a = argv[i];
        if (a == L"/S" || a == L"/s") silent = true;
        else if (a.size() > 3 && (a.compare(0, 3, L"/D=") == 0 || a.compare(0, 3, L"/d=") == 0)) {
            dst = a.substr(3);
            if (dst.size() >= 2 && dst.front() == L'"' && dst.back() == L'"')
                dst = dst.substr(1, dst.size() - 2);
        }
    }
    if (argv) ::LocalFree(argv);

    // ── 读自己的尾部 footer，定位载荷 ──
    std::wstring self = exePath();
    HANDLE h = ::CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(L"无法打开安装程序自身。");
    LARGE_INTEGER fsz{};
    ::GetFileSizeEx(h, &fsz);
    if (fsz.QuadPart < 16) { ::CloseHandle(h); return fail(L"安装程序文件不完整。"); }

    unsigned char footer[16] = {};
    if (!readAt(h, (unsigned long long)fsz.QuadPart - 16, footer, 16)) {
        ::CloseHandle(h);
        return fail(L"读取安装数据失败。");
    }
    DWORD magic = 0, count = 0;
    unsigned long long payloadOff = 0;
    ::memcpy(&magic, footer, 4);
    ::memcpy(&count, footer + 4, 4);
    ::memcpy(&payloadOff, footer + 8, 8);
    if (magic != kMagic || count == 0 || payloadOff >= (unsigned long long)fsz.QuadPart) {
        ::CloseHandle(h);
        return fail(L"安装数据校验失败（这个 exe 可能被截断或改动过）。");
    }

    // ── 解析目录表 ──
    std::vector<Entry> files;
    unsigned long long p = payloadOff;
    for (DWORD i = 0; i < count; ++i) {
        unsigned int nameLen = 0;
        if (!readAt(h, p, &nameLen, 4) || nameLen == 0 || nameLen > 512) {
            ::CloseHandle(h);
            return fail(L"安装数据损坏。");
        }
        p += 4;
        std::string nm(nameLen, 0);
        if (!readAt(h, p, &nm[0], nameLen)) { ::CloseHandle(h); return fail(L"安装数据损坏。"); }
        p += nameLen;
        unsigned long long size = 0;
        if (!readAt(h, p, &size, 8)) { ::CloseHandle(h); return fail(L"安装数据损坏。"); }
        p += 8;
        Entry e;
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, nm.c_str(), (int)nm.size(), nullptr, 0);
        e.name.assign((size_t)wlen, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, nm.c_str(), (int)nm.size(), &e.name[0], wlen);
        e.size = size;
        e.offset = p;
        files.push_back(e);
        p += size;
    }

    // ── 选安装目录 ──
    if (dst.empty()) {
        std::wstring def = envPath(L"LOCALAPPDATA");
        def += L"\\Programs\\hotrainy";
        if (silent) dst = def;
        else {
            std::wstring sel;
            if (!pickFolder(sel, def)) { ::CloseHandle(h); return 0; }  // 用户取消
            dst = sel;
            if (dst.size() > 1 && (dst.back() == L'\\' || dst.back() == L'/')) dst.pop_back();
            std::wstring q = L"确认安装到下面这个文件夹吗？\n\n" + dst +
                             L"\n\n里面会放入 hotrainy.exe、两个 DLL、使用说明和卸载程序。";
            if (::MessageBoxW(nullptr, q.c_str(), L"hotrainy 安装程序",
                              MB_ICONQUESTION | MB_OKCANCEL) != IDOK) {
                ::CloseHandle(h);
                return 0;
            }
        }
    }

    ensureDir(dst);
    // 目录不存在且创建失败 → 报错
    if (::GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ::CloseHandle(h);
        return fail(L"无法创建安装目录，请换一个位置（或检查权限）。");
    }

    std::wstring exeTarget = dst + L"\\hotrainy.exe";
    int written = 0;
    for (const Entry& e : files) {
        std::wstring out = dst + L"\\" + e.name;
        if (!writeFileBytes(out, h, e.offset, e.size)) {
            ::CloseHandle(h);
            return fail((L"写入文件失败：\n" + out).c_str());
        }
        ++written;
    }
    ::CloseHandle(h);

    // ── 快捷方式 / 自启 / 启动（静默模式全部跳过） ──
    bool madeShortcuts = false;
    if (!silent) {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring desk = envPath(L"USERPROFILE") + L"\\Desktop\\hotrainy.lnk";
        std::wstring menu = envPath(L"APPDATA") +
                            L"\\Microsoft\\Windows\\Start Menu\\Programs\\hotrainy.lnk";
        makeShortcut(desk, exeTarget, dst);
        makeShortcut(menu, exeTarget, dst);
        ::CoUninitialize();
        madeShortcuts = true;

        // 开机自启：安装程序**不碰**注册表（不写、不删）。
        // 用户想开机启动，就在程序里：托盘右键 → 设置 → 开机自启动。

        std::wstring done = L"安装完成！\n\n位置：" + dst +
                            L"\n\n桌面和开始菜单已经放了快捷方式。\n"
                            L"启动后右键托盘图标 → 设置，可以打开「透明」「岛台」「音频条」。\n\n"
                            L"想卸载：运行该文件夹里的 uninstall.cmd。\n\n（安装程序不会改动开机自启；要开机启动请在设置里打开它。）\n\n现在启动 hotrainy 吗？";
        if (::MessageBoxW(nullptr, done.c_str(), L"hotrainy 安装程序",
                          MB_ICONINFORMATION | MB_YESNO) == IDYES) {
            ::ShellExecuteW(nullptr, L"open", exeTarget.c_str(), nullptr, dst.c_str(),
                            SW_SHOWNORMAL);
        }
    } else {
        wchar_t msg[512] = {};
        ::swprintf(msg, 512, L"静默安装完成：%s（%d 个文件）", dst.c_str(), written);
        ::OutputDebugStringW(msg);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    return wmain();
}