// hr/settings/settings.h — 玻璃设置窗口（对照 ui/settings_window.py）
//
// 720×700 设计稿，左右两栏：
//   左栏  标签栏（展开方式 / 自动隐藏）
//         外观  （液态玻璃 / 壁纸模式 / 当前标签的壁纸）
//         搜索  （启用 / 快捷键 / 目录 / 索引范围 / 大小 / 跟随 / 对话框）
//   右栏  快捷方式（文字大小 + 实时预览）
//         系统  （音频波动条 / 隐藏开始菜单全部应用 / 开机自启动）
//         任务栏（透明：开=透明，关=跟随系统）
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "hr/shared/config.h"
#include "hr/glass/glass.h"
#include "hr/text/theme.h"

namespace hr {

class SettingsWindow {
public:
    // 设置已写进 cfg_ 并落盘后通知外部应用；返回 false 表示没生效（只有快捷键被占用）
    std::function<bool(const std::string& key)> onSettingChanged;
    std::function<void()> onQuit;

    bool open(HINSTANCE hInst, Config* cfg, bool dark, const std::string& version);
    void close();
    bool isOpen() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    // 当前激活标签（壁纸那一行要显示它的名字）
    void setActiveTab(int idx) { activeTab_ = idx; }

private:
    struct Hit {
        RECT rc;
        std::string key;
        std::function<void()> cb;
    };

    struct Palette {
        uint32_t text = 0xFFF2F3F5;
        uint32_t dim = 0xFF9AA0A8;
        uint32_t hair = 0x2EFFFFFF;
        uint32_t rowHover = 0x12FFFFFF;
        uint32_t trackOff = 0x38FFFFFF;
        uint32_t trackOn = 0xFFF7F7FA;
        uint32_t knobOn = 0xFFA8A8AE;
        uint32_t knobOff = 0xFFF2F2F5;
        uint32_t segBg = 0x1FFFFFFF;
        uint32_t segOn = 0xFFF4F4F7;
        uint32_t segOnText = 0xFF1A1B1E;
        uint32_t btnBg = 0x26FFFFFF;
        uint32_t btnBgHot = 0x42FFFFFF;
        uint32_t previewBg = 0x28FFFFFF;
        uint32_t closeHot = 0x28FFFFFF;
    };

    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void createWindow(const std::string& version);
    void buildGlass();
    void rebuildGlassOffscreen();
    void redraw();
    void pushFrame();

    Palette palette() const;

    // ── 基础绘制 ──
    void textLeft(int x, int cy, const std::wstring& t, float sz, uint32_t col,
                  const wchar_t* fam = L"Microsoft YaHei", bool bold = false);
    void textCenter(int cx, int cy, const std::wstring& t, float sz, uint32_t col,
                    const wchar_t* fam = L"Microsoft YaHei", bool bold = false);
    void textRight(int rx, int cy, const std::wstring& t, float sz, uint32_t col);
    void hline(int x1, int y, int x2, uint32_t col);
    void hit(int x1, int y1, int x2, int y2, const std::string& key, std::function<void()> cb);
    bool isHot(const std::string& key) const;
    const Hit* hitAt(int x, int y) const;

    // ── 控件 ──
    void toggle(int xRight, int cy, bool value, const std::string& key,
                std::function<void(bool)> onChange);
    void segmented(int xRight, int cy, const std::vector<std::pair<std::wstring, std::string>>& opts,
                   const std::string& value, const std::string& key,
                   std::function<void(const std::string&)> onChange);
    void stepper(int xRight, int cy, int value, const std::string& key,
                 std::function<void(int)> onChange, int lo, int hi, int step = 1, bool showNum = true);
    // 返回按钮左边界（方便一行并排两个按钮）
    // 0-100 的滑块（调节条）：整条可点（20 段、每段 5%），
    // 目前是"点击定位"，拖动版随后升级。onChange 收到新的 0-100 值。
    void slider(int xRight, int cy, int value, const std::string& key,
                std::function<void(int)> onChange);

    int button(int xRight, int cy, const std::wstring& text, const std::string& key,
               std::function<void()> cb, int width = 0);
    void hotkeyPill(int xRight, int cy);

    // ── 内容 ──
    int section(int x, int y, const std::wstring& title);
    int row(int x, int y, const std::wstring& label, const std::string& key,
            std::function<void(int, int)> drawControl);
    void drawHeader();
    void drawFooter();
    void drawPreview(int x, int y);
    void drawSearchTips(int x, int y);
    void drawTaskbarNotes(int x, int y);
    void drawWallpaperRow(int xRight, int cy);
    void drawDirsRow(int xRight, int cy);
    void drawSizeRow(int xRight, int cy);

    // ── 读写 ──
    bool getBool(const std::string& key) const;
    int getInt(const std::string& key) const;
    std::wstring getStr(const std::string& key) const;
    void setBool(const std::string& key, bool v);
    void setInt(const std::string& key, int v);
    bool setHotkey(const std::wstring& spec);
    void notify(const std::string& key);

    void pickWallpaper();
    void clearWallpaper();
    void addDir();
    void clearDirs();
    void resetSearchSize();
    void startRecord();
    void recordKey(WPARAM vk);
    int currentTabIndex() const;
    // 壁纸适用标签（按 id 解析，标签增删后不会错位）
    int wallTabIndex() const;

    void refreshRegistryCache();

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    Config* cfg_ = nullptr;
    std::wstring version_;

    Surface glass_;
    Surface frame_;
    uint32_t avg_ = 0xFF1B1C20;
    bool hasGlass_ = false;
    bool dark_ = true;

    int x_ = 0, y_ = 0, winW_ = 0, winH_ = 0;
    int pad_ = 0, colGap_ = 0, rowH_ = 0, headH_ = 0, colW_ = 0;

    std::vector<Hit> hits_;
    std::string hoverKey_;
    std::function<void()> pendingCb_;
    std::string pendingKey_;
    bool dragging_ = false;
    bool dragged_ = false;
    POINT dragOrigin_ = {0, 0};
    POINT dragStart_ = {0, 0};

    bool recording_ = false;
    bool hotkeyError_ = false;
    UINT_PTR errorTimer_ = 0;

    bool cacheAutostart_ = false;
    int activeTab_ = 0;   // 由 main 在打开时告知，壁纸行按它取名
};

}  // namespace hr
