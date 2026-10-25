// hr/tb/xaml/taplog.cpp — DLL 侧独立日志
//
// TAP DLL 运行在 explorer 进程里，不能依赖主程序的 common.cpp：
//   · 会拖进 GetOpenFileNameW 等无关依赖
//   · 主程序的日志路径逻辑在注入环境里不一定可用
//
// 这里实现一个自包含的 logger，写到 %LOCALAPPDATA%\hotrainy\tap.log。
// 日志对排查注入问题至关重要（注入失败时没有界面可看）。
#include "hr/tb/xaml/taplog.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace hr {
namespace tb {
namespace xaml {

namespace {

std::mutex g_mu;
std::wstring g_path;
bool g_ready = false;

std::wstring computePath() {
    wchar_t buf[MAX_PATH] = {};
    std::wstring dir;
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
        dir = buf;
    }
    if (dir.empty()) return L"";
    dir += L"\\hotrainy";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\tap.log";
}

}  // namespace

void tapLogInit() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ready) return;
    g_path = computePath();
    g_ready = !g_path.empty();
}

void tapLogf(const char* fmt, ...) {
    if (!g_ready) return;

    char buf[1200];
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "[%02d:%02d:%02d.%03d][pid=%lu] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        (unsigned long)::GetCurrentProcessId());

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - (size_t)n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) n = (int)sizeof(buf) - 2; else n += m;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\n';
    buf[n] = '\0';

    std::lock_guard<std::mutex> lk(g_mu);
    HANDLE h = ::CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, buf, (DWORD)n, &wr, nullptr);
    ::CloseHandle(h);
}

// 宽字符版本。
//
// 存在的理由：调用方常要打路径、类型名这类宽字符串。
// 而 tapLogf 是窄字符 printf —— 传 %ls 进去会让参数解析错位、
// 破坏调用栈（实测日志里出现大段乱码，且后续调用行为异常）。
//
// 这里先把宽串格式化到宽缓冲，再整体转成 UTF-8 写文件。
void tapLogw(const wchar_t* fmt, ...) {
    if (!g_ready) return;
    if (!fmt) return;

    wchar_t wbuf[1200] = {0};
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    int n = _snwprintf_s(wbuf, 1200, _TRUNCATE,
                         L"[%02d:%02d:%02d.%03d][pid=%lu] ",
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                         (unsigned long)::GetCurrentProcessId());

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnwprintf_s(wbuf + n, 1200 - (size_t)n, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) n = 1198; else n += m;
    if (n > 1197) n = 1197;
    wbuf[n++] = L'\n';
    wbuf[n] = L'\0';

    // 转 UTF-8
    int need = ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return;
    std::string utf8((size_t)need - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, &utf8[0], need, nullptr, nullptr);

    std::lock_guard<std::mutex> lk(g_mu);
    HANDLE h = ::CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wr, nullptr);
    ::CloseHandle(h);
}

}  // namespace xaml
}  // namespace tb
}  // namespace hr