// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/inject_diag.cpp — 注入链路逐步诊断
//
// 把注入的每一步单独打出来，定位到底卡在哪：
//   1. 找 explorer 的窗口线程
//   2. LoadLibrary 我们的 DLL
//   3. SetWindowsHookEx
//   4. 发消息触发钩子
//   5. 检查 DLL 是否进了 explorer（读 explorer 的模块列表）
//   6. 等就绪事件
#include "hr/common.h"

#include <shlobj.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace hr;

namespace {

std::wstring dllPath() {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    return p + L"GlassBoot.dll";
}

// 列出某进程已加载的模块
bool hasModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (::Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) { found = true; break; }
        } while (::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap);
    return found;
}

DWORD explorerPid() {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return pid;
}

LRESULT CALLBACK hookProc(int code, WPARAM w, LPARAM l) {
    return ::CallNextHookEx(nullptr, code, w, l);
}

}  // namespace

int main() {
    logInit();
    printf("===== 注入链路诊断 =====\n\n");

    // [1] 找任务栏与线程
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    printf("[1] Shell_TrayWnd = 0x%p\n", (void*)tray);
    if (!tray) { printf("    ❌ 没有任务栏\n"); return 1; }

    DWORD pid = 0;
    DWORD tid = ::GetWindowThreadProcessId(tray, &pid);
    printf("    explorer PID = %lu, 主线程 TID = %lu\n", pid, tid);

    DWORD expPid = explorerPid();
    printf("    进程快照里的 explorer PID = %lu  %s\n", expPid,
           (expPid == pid) ? "(一致)" : "(不一致!)");

    // [2] 加载 DLL
    std::wstring path = dllPath();
    printf("\n[2] DLL 路径 = %ls\n", path.c_str());
    bool exists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    printf("    文件存在: %s\n", exists ? "是" : "否 ❌");
    if (!exists) return 1;

    HMODULE mod = ::LoadLibraryExW(path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                       LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    printf("    LoadLibraryEx = %p  %s\n", (void*)mod,
           mod ? "成功" : ("失败 err=" + std::to_string(::GetLastError())).c_str());
    if (!mod) return 1;

    // [3] 注入前检查 explorer 是否已有模块
    printf("\n[3] 注入前 explorer 是否已加载 GlassTAP.dll: %s\n",
           hasModule(pid, L"GlassBoot.dll") ? "是" : "否");

    // [4] 装钩子
    printf("\n[4] SetWindowsHookEx(WH_CALLWNDPROC) 到 TID %lu ...\n", tid);
    HHOOK hook = ::SetWindowsHookExW(WH_CALLWNDPROC, hookProc, mod, tid);
    printf("    hook = %p  err=%lu\n", (void*)hook, hook ? 0 : ::GetLastError());
    if (!hook) return 1;

    // [5] 触发钩子：给该线程发消息
    printf("\n[5] 触发钩子（发消息给 explorer 线程）...\n");
    ::PostThreadMessageW(tid, WM_NULL, 0, 0);
    // 同时给任务栏发消息，确保消息循环被唤醒
    ::SendMessageTimeoutW(tray, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);

    for (int i = 0; i < 10; ++i) {
        ::Sleep(500);
        bool has = hasModule(pid, L"GlassBoot.dll");
        printf("    %2d 秒后: explorer 加载 GlassTAP = %s\n", (i + 1) / 2, has ? "✅ 是" : "否");
        if (has) break;
    }

    bool injected = hasModule(pid, L"GlassBoot.dll");
    printf("\n[6] 注入结果: %s\n", injected ? "✅ DLL 已进入 explorer" : "❌ DLL 未进入");

    if (injected) {
        printf("\n[7] 等待就绪事件（20 秒）...\n");
        for (int i = 0; i < 40; ++i) {
            HANDLE ev = ::OpenEventW(SYNCHRONIZE, FALSE, L"GlassTAP_Ready_v1");
            if (ev) {
                DWORD r = ::WaitForSingleObject(ev, 500);
                ::CloseHandle(ev);
                if (r == WAIT_OBJECT_0) { printf("    ✅ 事件已置位（第 %d 次检查）\n", i + 1); break; }
            }
            ::Sleep(500);
        }
    }

    printf("\n=== 检查 tap.log ===\n");
    wchar_t appdata[MAX_PATH] = {};
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata))) {
        std::wstring log = std::wstring(appdata) + L"\\hotrainy\\tap.log";
        printf("  路径: %ls\n", log.c_str());
        HANDLE h = ::CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[4096] = {};
            DWORD rd = 0;
            ::ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
            ::CloseHandle(h);
            printf("  内容:\n%s\n", buf);
        } else {
            printf("  ❌ 日志不存在（DLL 的 DllMain 可能没跑到或提前 return FALSE）\n");
        }
    }

    if (hook) ::UnhookWindowsHookEx(hook);
    return injected ? 0 : 2;
}