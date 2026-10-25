// hr/common.h — 基础设施（路径 / 编码 / 日志 / DPI）
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <string>

namespace hr {

// ── 路径 ──
std::wstring exePath();
std::wstring exeDir();
std::wstring configDir();                 // %APPDATA%\hotrainy
std::wstring configPath();                // config.json（与 Python 版同一个文件）
std::wstring logPath();
std::wstring iconCacheDir();

// ── 编码 ──
std::string toUtf8(const std::wstring& w);
std::wstring toWide(const std::string& s);

// ── 文件 ──
bool ensureDir(const std::wstring& path);
bool fileExists(const std::wstring& path);
bool readFileUtf8(const std::wstring& path, std::string& out);
bool writeFileAtomic(const std::wstring& path, const std::string& data);

// ── 系统选择框（壁纸 / 搜索目录 / 快捷方式目标） ──
// filter 形如 L"图片文件\0*.png;*.jpg\0所有文件\0*.*\0\0"
bool pickFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter, std::wstring& out);
bool pickFolderDialog(HWND owner, const wchar_t* title, std::wstring& out);
// 8 位十六进制随机 id（对照 Python 的 uuid4()[:8]）
std::string shortId();

// ── 日志 ──
void logInit();
void logf(const char* fmt, ...);
void logw(const wchar_t* fmt, ...);

// ── DPI ──
// 声明 Per-Monitor V2 并记下系统缩放比
void enablePerMonitorDpi();
double dpiScale();
// 设计稿像素 → 物理像素（对照 core/dpi.py: px()）
int px(double value);
// 字号：Tk 的 size=N 是磅值，转成像素 = N * dpi / 72
int pt(double points);
int dpiForWindow(HWND hwnd);

}  // namespace hr
