// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// tools/tap_apply.cpp - inject, attach, then actually set transparency.
//
// The chain now works end to end:
//   GlassBoot.dll -> InitializeXamlDiagnosticsEx -> S_OK
//   -> DllGetClassObject(CLSID_TAPSite) -> TAPSite
//   -> SetSite(IXamlDiagnostics) -> AdviseVisualTreeChange
//   -> BackgroundFill located
//
// This tool finishes the job: it keeps the hook alive, waits for the visual
// tree to be attached, then asks the service (running inside explorer) to
// make the taskbar background fully transparent, and measures the result.
#include <windows.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static const wchar_t* kBootDll = L"GlassBoot.dll";
static const wchar_t* kFeatureDll = L"GlassTAP.dll";
static const wchar_t* kReadyEvent = L"GlassTAP_Ready_v1";

typedef LRESULT(CALLBACK* PFN_HookProc)(int, WPARAM, LPARAM);

static std::wstring exeDir() {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    return p;
}

static DWORD explorerPid() {
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

static bool hasModule(DWORD pid, const wchar_t* name) {
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

struct Measure { double bg; double sharp; };

static Measure measureTaskbar(HWND tray) {
    Measure m{ -1, 0 };
    RECT r{};
    if (!::GetWindowRect(tray, &r)) return m;

    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return m;

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> px((size_t)w * h * 4);
    ::GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);

    double s = 0; int n = 0;
    for (int y = h / 3; y < h * 2 / 3; y += 2)
        for (int x = 10; x < 220 && x < w; x += 2) {
            s += px[((size_t)y * w + x) * 4 + 2]; ++n;
        }
    m.bg = n ? s / n : -1;

    double e = 0; int en = 0;
    int x0 = w * 35 / 100, x1 = w * 66 / 100;
    for (int y = h / 4; y < h * 3 / 4; ++y)
        for (int x = x0; x < x1 - 1; ++x) {
            e += std::fabs((double)px[((size_t)y * w + x) * 4 + 2] -
                           (double)px[((size_t)y * w + x + 1) * 4 + 2]);
            ++en;
        }
    m.sharp = en ? e / en : 0;
    return m;
}

// Read a double out of the DLL's log so we know the in-explorer steps ran.
static void tailTapLog() {
    wchar_t la[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
    std::wstring p = std::wstring(la) + L"\\hotrainy\\tap.log";

    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { printf("  (tap.log not found)\n"); return; }

    char buf[16384] = {};
    DWORD rd = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    ::CloseHandle(h);

    // print the last few lines
    std::string all(buf);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < all.size()) {
        size_t nl = all.find('\n', start);
        if (nl == std::string::npos) break;
        lines.push_back(all.substr(start, nl - start));
        start = nl + 1;
    }
    size_t from = lines.size() > 14 ? lines.size() - 14 : 0;
    for (size_t i = from; i < lines.size(); ++i) {
        printf("  %s\n", lines[i].c_str());
    }
}

int main(int argc, char** argv) {
    int holdSeconds = (argc > 1) ? atoi(argv[1]) : 25;

    printf("===== tap_apply =====\n\n");

    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) { printf("no taskbar\n"); return 1; }
    DWORD pid = 0;
    DWORD tid = ::GetWindowThreadProcessId(tray, &pid);
    printf("explorer pid=%lu tid=%lu\n", pid, tid);

    Measure before = measureTaskbar(tray);
    printf("BEFORE: background=%.1f sharpness=%.2f\n\n", before.bg, before.sharp);

    std::wstring dll = exeDir() + kBootDll;
    HMODULE mod = ::LoadLibraryExW(dll.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                       LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) { printf("LoadLibraryEx failed err=%lu\n", ::GetLastError()); return 1; }

    auto hookProc = (PFN_HookProc)(void*)::GetProcAddress(mod, "GlassBoot_HookProc");
    if (!hookProc) { printf("no GlassBoot_HookProc export\n"); return 1; }

    HANDLE ev = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kReadyEvent);
    if (ev) { ::ResetEvent(ev); ::CloseHandle(ev); }

    HHOOK hook = ::SetWindowsHookExW(WH_CALLWNDPROC, hookProc, mod, tid);
    if (!hook) { printf("SetWindowsHookEx failed err=%lu\n", ::GetLastError()); return 1; }
    printf("hook=%p (owner: DLL) - keeping it for the whole run\n\n", (void*)hook);

    // Wait for the feature layer and for SetSite to complete.
    for (int i = 0; i < holdSeconds; ++i) {
        ::PostThreadMessageW(tid, WM_NULL, 0, 0);
        ::SendMessageTimeoutW(tray, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 300, nullptr);

        if (i % 5 == 0) {
            Measure m = measureTaskbar(tray);
            printf("[%2ds] boot=%-3s feature=%-3s  bg=%.1f sharp=%.2f\n", i,
                   hasModule(pid, kBootDll) ? "y" : "n",
                   hasModule(pid, kFeatureDll) ? "y" : "n",
                   m.bg, m.sharp);
        }
        ::Sleep(1000);
    }

    Measure after = measureTaskbar(tray);
    printf("\nAFTER : background=%.1f sharpness=%.2f\n", after.bg, after.sharp);
    printf("delta background = %+.1f\n", after.bg - before.bg);

    printf("\n--- tap.log tail ---\n");
    tailTapLog();

    ::UnhookWindowsHookEx(hook);
    printf("\nhook released\n");
    return 0;
}