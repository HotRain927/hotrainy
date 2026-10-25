// hr/common.cpp
#include "hr/shared/common.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>

namespace hr {

namespace {

std::wstring g_logPath;
CRITICAL_SECTION g_lock;
bool g_lockReady = false;

std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::wstring();
    return std::wstring(buf, n);
}

std::wstring join(const std::wstring& a, const wchar_t* b) {
    if (a.empty()) return std::wstring(b);
    std::wstring r = a;
    if (r.back() != L'\\') r += L'\\';
    r += b;
    return r;
}

}  // namespace

std::wstring exePath() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return n ? std::wstring(buf, n) : std::wstring();
}

std::wstring exeDir() {
    std::wstring p = exePath();
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

std::wstring configDir() {
    std::wstring base = envVar(L"APPDATA");
    if (base.empty()) {
        wchar_t buf[MAX_PATH] = {0};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) base = buf;
    }
    return join(base, L"hotrainy");
}

std::wstring configPath() { return join(configDir(), L"config.json"); }

std::wstring iconCacheDir() { return join(configDir(), L"Icons"); }

std::wstring logPath() { return join(configDir(), L"cpp.log"); }

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool ensureDir(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 2) ensureDir(path.substr(0, pos));
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool readFileUtf8(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.empty() ? nullptr : &out[0], (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        out.clear();
        return false;
    }
    out.resize(read);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool writeFileAtomic(const std::wstring& path, const std::string& data) {
    ensureDir(configDir());
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void logInit() {
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    ensureDir(configDir());
    g_logPath = logPath();
}

void logf(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    char body[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1400];
    int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n", st.wYear,
                     st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    if (g_lockReady) EnterCriticalSection(&g_lock);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line, (DWORD)n, &written, nullptr);
        CloseHandle(h);
    }
    if (g_lockReady) LeaveCriticalSection(&g_lock);

    std::wstring w = toWide(line);
    OutputDebugStringW(w.c_str());
}

void logw(const wchar_t* fmt, ...) {
    wchar_t wbuf[1000];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(wbuf, 1000, fmt, ap);
    va_end(ap);
    logf("%s", toUtf8(wbuf).c_str());
}

void enablePerMonitorDpi() {
    typedef BOOL(WINAPI * PFN_SetProcessDpiAwarenessContext)(HANDLE);
    static bool done = false;
    if (done) return;
    done = true;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
        PFN_SetProcessDpiAwarenessContext fn =
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(user32,
                                                              "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) {  // PER_MONITOR_AWARE_V2
            logf("dpi: Per-Monitor V2");
            return;
        }
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT(WINAPI * PFN_SetProcessDpiAwareness)(int);
        PFN_SetProcessDpiAwareness fn2 =
            (PFN_SetProcessDpiAwareness)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn2 && SUCCEEDED(fn2(2))) {
            logf("dpi: Per-Monitor");
            return;
        }
    }
    SetProcessDPIAware();
    logf("dpi: System（降级）");
}

namespace {
double g_scale = 1.0;
bool g_scaleReady = false;
}  // namespace

double dpiScale() {
    if (g_scaleReady) return g_scale;
    g_scaleReady = true;
    int dpi = 96;
    typedef UINT(WINAPI * PFN_GetDpiForSystem)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    PFN_GetDpiForSystem fn =
        user32 ? (PFN_GetDpiForSystem)GetProcAddress(user32, "GetDpiForSystem") : nullptr;
    if (fn) {
        dpi = (int)fn();
    } else {
        HDC dc = GetDC(nullptr);
        if (dc) {
            dpi = GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(nullptr, dc);
        }
    }
    if (dpi <= 0) dpi = 96;
    g_scale = (double)dpi / 96.0;
    if (g_scale < 1.0) g_scale = 1.0;
    logf("dpi: 系统 %d (scale %.3f)", dpi, g_scale);
    return g_scale;
}

int px(double value) { return (int)lround(value * dpiScale()); }

int pt(double points) { return (int)lround(points * dpiScale() * 96.0 / 72.0); }

int dpiForWindow(HWND hwnd) {
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (PFN_GetDpiForWindow)GetProcAddress(user32, "GetDpiForWindow");
    }
    if (fn && hwnd) return (int)fn(hwnd);
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi;
}

// ── 系统选择框 ──

bool pickFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter,
                    std::wstring& out) {
    wchar_t buf[1024] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

bool pickFolderDialog(HWND owner, const wchar_t* title, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg)))) {
        logf("folder: 创建选择框失败");
        return false;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
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

std::string shortId() {
    unsigned a = (unsigned)(GetTickCount64() & 0xFFFFFFFFu) ^ ((unsigned)rand() * 2654435761u);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", a);
    return std::string(buf, 8);
}

}  // namespace hr
