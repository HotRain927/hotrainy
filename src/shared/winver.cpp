// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// This file is part of hotrainy: noncommercial use only; see LICENSE.
//
// 系统版本与能力层的实现。见头文件里的说明。

#include "hr/shared/winver.h"

#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW（解析 --safe）

#include <cstdio>
#include <cstring>

#include "hr/shared/common.h"

namespace hr {
namespace {

// ── 真实的系统版本 ───────────────────────────────────
// ⚠ 绝不能用 GetVersionEx：受 manifest 影响，未声明 Win10 兼容性时会
//   谎报 6.2（Win8）。RtlGetVersion 直接返回内核的真实版本。
WinVersion probe() {
    WinVersion v;

    // 动态取 ntdll!RtlGetVersion —— 这样不必链接 ntdll 的导入库
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RtlGetVersionFn fn = nullptr;
    if (HMODULE nt = ::GetModuleHandleW(L"ntdll.dll")) {
        fn = (RtlGetVersionFn)::GetProcAddress(nt, "RtlGetVersion");
    }
    if (fn) {
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (fn(&vi) == 0 /* STATUS_SUCCESS */) {
            v.major = vi.dwMajorVersion;
            v.minor = vi.dwMinorVersion;
            v.build = vi.dwBuildNumber;
        }
    }
    // 兜底（正常永远走不到）
    if (!v.valid()) {
        v.major = 10;
        v.build = 19045;
        logf("winver: RtlGetVersion 不可用，退回假设 Win10 19045");
    }

    // 注册表补 DisplayVersion（22H2 / 24H2…）与 UBR（修订号）
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[128] = {};
        DWORD len = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (::RegQueryValueExW(k, L"DisplayVersion", nullptr, &type,
                               (LPBYTE)buf, &len) == ERROR_SUCCESS) {
            v.display = buf;
        }
        DWORD ubr = 0, sz = sizeof(ubr), t2 = 0;
        if (::RegQueryValueExW(k, L"UBR", nullptr, &t2, (LPBYTE)&ubr,
                               &sz) == ERROR_SUCCESS) {
            v.ubr = ubr;
        }
        ::RegCloseKey(k);
    }
    return v;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                          nullptr, nullptr);
    return s;
}

bool g_safe = false;
bool g_parsed = false;

void parseCommandLine() {
    if (g_parsed) return;
    g_parsed = true;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (::_wcsicmp(argv[i], L"--safe") == 0 ||
            ::_wcsicmp(argv[i], L"-safe") == 0 ||
            ::_wcsicmp(argv[i], L"/safe") == 0) {
            g_safe = true;
        }
    }
    ::LocalFree(argv);
}

}  // namespace

std::string WinVersion::text() const {
    char b[96];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u", major, minor, build, ubr);
    std::string s = b;
    if (!display.empty()) {
        s += " (";
        s += narrow(display);
        s += ")";
    }
    return s;
}

std::string WinVersion::friendly() const {
    const char* name = isWin11() ? "Windows 11" : (isWin10() ? "Windows 10" : "Windows(未知)");
    char b[160];
    std::snprintf(b, sizeof(b), "%s %s (build %u.%u)", name,
                  display.empty() ? "" : narrow(display).c_str(), build, ubr);
    return b;
}

const WinVersion& winVersion() {
    static WinVersion v = probe();
    return v;
}

const char* methodName(TbMethod m) {
    switch (m) {
        case TbMethod::XamlInject:  return "XamlInject(注入 explorer 改 XAML 任务栏)";
        case TbMethod::Accent:      return "Accent(SetWindowCompositionAttribute 亚克力)";
        case TbMethod::Registry:    return "Registry(TaskbarAcrylicOpacity)";
        case TbMethod::DwmBackdrop: return "DwmBackdrop(DWMWA_SYSTEMBACKDROP_TYPE)";
        default:                    return "None";
    }
}

bool methodSupported(TbMethod m) {
    const WinVersion& v = winVersion();
    switch (m) {
        // 注入改 XAML 任务栏：只有 Win11（XAML 任务栏存在）才谈得上
        case TbMethod::XamlInject:
            return v.isWin11();
        // accent 亚克力：Win10 全系 —— Win11 21H2 尚可，22H2 起大面积失效
        case TbMethod::Accent:
            return v.build > 0 && v.build < 22621;
        // 注册表 TaskbarAcrylicOpacity：Win11 22H2 起才有这一项
        case TbMethod::Registry:
            return v.atLeast(22621);
        // DWM 背板：Win11 22H2 起
        case TbMethod::DwmBackdrop:
            return v.atLeast(22621);
        default:
            return false;
    }
}

std::vector<TbMethod> taskbarMethods() {
    const WinVersion& v = winVersion();
    std::vector<TbMethod> out;
    if (v.isWin10()) {
        // ★ Win10：accent 亚克力是主路径，实测"最稳定的一代，几乎无坑"，
        //   而且**完全不需要注入 explorer**
        out.push_back(TbMethod::Accent);
    } else {
        // Win11：先试原有的 XAML 注入通道；不行再走注册表 / DWM 背板
        out.push_back(TbMethod::XamlInject);
        out.push_back(TbMethod::Registry);
        out.push_back(TbMethod::DwmBackdrop);
    }
    out.push_back(TbMethod::None);
    return out;
}

bool safeMode() {
    const WinVersion& v = winVersion();  // 确保已探测（内部会解析命令行）
    (void)v;
    parseCommandLine();
    return g_safe;
}

void logCapabilities() {
    const WinVersion& v = winVersion();
    logf("winver: %s", v.friendly().c_str());
    logf("winver: 判定 -> Win10=%d Win11=%d 安全模式=%d",
         (int)v.isWin10(), (int)v.isWin11(), (int)safeMode());

    auto ms = taskbarMethods();
    std::string chain;
    for (size_t i = 0; i < ms.size(); ++i) {
        if (i) chain += " -> ";
        chain += methodName(ms[i]);
    }
    logf("winver: 任务栏透明候选方案: %s", chain.c_str());

    for (auto m : ms) {
        if (m == TbMethod::None) break;
        logf("winver:   %-46s 理论上可用=%d", methodName(m),
             (int)methodSupported(m));
    }
}

void initVersionLayer() {
    parseCommandLine();
    logCapabilities();
    if (safeMode()) {
        logf("winver: ★ 安全模式已启用（--safe）：本次不注入 explorer、"
             "不做任何系统级改动");
    }
}

}  // namespace hr