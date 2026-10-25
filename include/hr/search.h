// hr/search.h — 搜索窗（对照 ui/search_window.py + core/search_service.py）
#pragma once

#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "hr/config.h"
#include "hr/glass.h"

namespace hr {

// 搜索输入框子类过程（定义在 search.cpp，create() 里要取地址 -> 先声明）
LRESULT CALLBACK searchEditProc(HWND, UINT, WPARAM, LPARAM);

// 文件对话框检测（对照 search_window.py 的 file_dialog_bounds / _is_file_dialog）：
// 前台或按 z 序找最上面的那个 Windows 文件对话框（#32770 + SHELLDLL_DefView
// 或 DirectUIHWND+Edit），返回其可见矩形与窗口句柄。
bool fileDialogBounds(RECT& out, HWND& hwnd);

struct SearchHit {
    std::wstring name;
    std::wstring path;
    std::wstring parent;
    int kind = 2;   // 0=目录 1=程序 2=文件
};

// 查一次关键词，返回排好序的命中项。
//
// 给岛台面板用：它要在自己面板里直接列结果（用户要求
// "把搜索框复制一份固定在岛台上"），所以需要一个不改动
// 搜索窗状态的查询入口。
//
// ⚠ 和搜索窗走的是同一套匹配/排序（search.cpp 里的 runQuery），
//   两处结果必须一致。
std::vector<SearchHit> queryHits(const std::wstring& q, int maxHits);

class SearchWindow {
public:
    bool create(HINSTANCE hInst, Config* cfg, bool dark);
    void destroy();

    void show(bool activate = true);
    void hide();
    void toggle();
    bool visible() const { return visible_; }
    void setDark(bool dark) { dark_ = dark; }

    // 自动弹出（文件对话框）用：改贴到另一个窗口；查询当前吸附对象 / 是否手动脱离
    void attachTo(HWND hwnd, bool dialog);
    HWND attached() const { return attached_; }
    bool userDetached() const { return userDetached_; }
    // 自动弹出后被用户关掉 → 通知主程序别再为同一个对话框弹回来
    std::function<void(HWND)> onAutoClosed;

    // 设置里改了索引范围/目录/开关后调用
    void reloadIndex();
    bool indexReady() const;
    int indexCount() const;

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    void layoutEntry();
    void buildGlass();
    void rebuildGlassOffscreen();
    void redraw();
    void pushFrame();

    void drawHeader();
    void drawResults();
    void drawScrollbar(int top, int avail, int rows, int n);
    void drawShapeIcon(const SearchHit& hit, int x, int y, int size, uint32_t tile);
    void drawFooter();

    uint32_t bandAvg(int y1, int y2) const;
    uint32_t tint(int y1, int y2, double amount, bool towardWhite) const;

    void place(bool autoOnly);
    void fitBelow(int t, int b, int sh, int& y, int& h) const;
    bool targetBounds(RECT& out, HWND& hwnd, bool& isDialog) const;

    void runSearch();
    void moveSel(int delta);
    void pageSel(int dir);
    void ensureVisible();
    void clampSelToView();
    void scrollBy(int rows);
    void openSelected();
    void revealSelected();
    void open(const SearchHit& hit);
    void reveal(const SearchHit& hit);

    int rowAt(int y) const;
    const wchar_t* zoneAt(int x, int y) const;
    void sbPress(int y);
    void sbDrag(int yRoot);
    void syncEntryColors();
    void followTarget();

    void persistSize();
    void persistPos();

    friend LRESULT CALLBACK searchEditProc(HWND, UINT, WPARAM, LPARAM);

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    WNDPROC editOldProc_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    Config* cfg_ = nullptr;

    Surface glass_;
    Surface frame_;
    uint32_t avg_ = 0xFF1A1B1F;
    bool dark_ = true;
    bool hasGlass_ = false;

    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool visible_ = false;
    bool solidOnly_ = false;      // 拖动/缩放中：只画实底

    std::vector<SearchHit> hits_;
    int sel_ = 0;
    std::wstring query_;
    std::wstring hint_;
    int hotRow_ = -1;
    int scroll_ = 0;
    int rowsVis_ = 1;
    int maxHits_ = 800;

    // 几何
    int top_ = 0, rowH_ = 0;

    // 交互
    bool dragging_ = false, dragMoved_ = false;
    POINT dragOffset_ = {0, 0};
    const wchar_t* resizeZone_ = nullptr;
    bool resizing_ = false, resizeMoved_ = false;
    int rStartX_ = 0, rStartY_ = 0, rStartW_ = 0, rStartH_ = 0;
    POINT rMouse_ = {0, 0};
    bool sbDragging_ = false;
    int sbStartY_ = 0, sbStartScroll_ = 0;
    int sbTrack_ = 0, sbThumb_ = 0, sbTravel_ = 0, sbMaxScroll_ = 1;

    // 位置/尺寸记忆
    bool customSize_ = false;
    bool posCustom_ = false;

    // 跟随
    HWND attached_ = nullptr;
    bool attachedDialog_ = false;
    bool autoMode_ = false;
    bool userDetached_ = false;   // 用户手动挪/缩放过 → 别再自动吸回去

    bool searchTimer_ = false;
    UINT_PTR tickTimer_ = 0;
};

}  // namespace hr
