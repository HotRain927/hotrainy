// hr/tagbar/tabs_menu_wire.cpp — 标签栏/面板的右键菜单（增删改）+ 托盘菜单。
//
// 结构重构第 2 片：这一整块原来堆在 main.cpp 里（约 300 行），
// 按"标签栏和它的面板的东西写一块"搬到这里。依赖的全局量走 app_context 的薄访问器。
//
// ⚠ 这里所有对话框回调都遵守同一条规矩（原来在 main.cpp 里踩过三次悬空引用）：
//   开模态对话框**之前只拷值、不拿 Tab&/Shortcut& 引用**，回来之后重新校验下标。
//   原因见各回调里的注释（askString/askYesNo 跑消息循环期间用户可能增删标签）。
#include "hr/tagbar/tabs_menu_wire.h"

#include <string>
#include <vector>
#include <shellapi.h>   // ShellExecuteW / AllowSetForegroundWindow
#include <windows.h>

#include "hr/app/app_context.h"
#include "hr/app/menu_ids.h"
#include "hr/island/taskbar.h"           // taskbarService()
#include "hr/settings/dialog.h"          // askString / askYesNo / showWarning
#include "hr/settings/settings_wire.h"   // openSettings()
#include "hr/shared/common.h"            // logf / toUtf8 / fileExists / shortId
#include "hr/shared/config.h"            // saveConfig / loadConfig
#include "hr/shared/icons.h"             // shellPath()
#include "hr/shared/menu.h"              // ContextMenuHooks

namespace hr {
namespace {

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

void saveAndRefresh() {
    saveConfig(cfg());
    bar().onConfigChanged();
    panel().onConfigChanged();
    // ★ 设置里改了任何外观相关的东西，岛台面板也要重抓背景。
    island().refreshAppearance();
    settings().setActiveTab(bar().active());
    if (bar().active() >= (int)cfg().tabs.size()) bar().setActive(-1);
}

}  // namespace

ContextMenuHooks makeHooks() {
    ContextMenuHooks h;
    HWND owner = bar().hwnd();

    h.onAdd = [](int tab, const std::wstring& path, const std::wstring& name) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        Tab& t = cfg().tabs[(size_t)tab];
        for (const auto& s : t.shortcuts) {
            if (lowerW(s.targetPath) == lowerW(path)) {
                showWarning(bar().hwnd(), L"重复", L"该目标路径已存在于当前标签中");
                return;
            }
        }
        for (const auto& s : t.shortcuts) {
            if (lowerW(s.name) == lowerW(name)) {
                showWarning(bar().hwnd(), L"重复", L"该名称已存在于当前标签中");
                return;
            }
        }
        if (t.shortcuts.size() >= 50) {
            showWarning(bar().hwnd(), L"上限", L"每个标签最多50个快捷方式");
            return;
        }
        Shortcut s;
        s.id = shortId();
        s.name = name;
        s.targetPath = path;
        t.shortcuts.push_back(s);
        logf("menu: 标签 %s 添加快捷方式 %s -> %s", t.id.c_str(), toUtf8(name).c_str(),
             toUtf8(path).c_str());
        saveAndRefresh();
    };

    h.onDelete = [](int tab, int idx) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        Tab& t = cfg().tabs[(size_t)tab];
        if (idx < 0 || idx >= (int)t.shortcuts.size()) return;
        logf("menu: 删除快捷方式 %s", toUtf8(t.shortcuts[(size_t)idx].name).c_str());
        t.shortcuts.erase(t.shortcuts.begin() + idx);
        saveAndRefresh();
    };

    h.onEdit = [owner](int tab, int idx) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        if (idx < 0 || idx >= (int)cfg().tabs[(size_t)tab].shortcuts.size()) return;

        // ⚠ 先把要用的值**拷出来**，再开对话框。
        //
        //   不能在对话框之前拿 Tab& 引用 —— askString 是模态的，
        //   在它跑消息循环期间用户可以增删标签，cfg().tabs 一变，
        //   引用就悬空了，回来接着用就是访问野内存（0xc0000005）。
        std::wstring oldName = cfg().tabs[(size_t)tab].shortcuts[(size_t)idx].name;

        std::wstring name;
        if (!askString(owner, L"编辑快捷方式", L"名称:", oldName, name)) return;

        // 对话框返回后**重新校验**（标签可能已经被删了）
        if (tab < 0 || tab >= (int)cfg().tabs.size()) {
            logf("menu: 编辑取消 —— 标签已不存在");
            return;
        }
        if (idx < 0 || idx >= (int)cfg().tabs[(size_t)tab].shortcuts.size()) {
            logf("menu: 编辑取消 —— 快捷方式已不存在");
            return;
        }

        cfg().tabs[(size_t)tab].shortcuts[(size_t)idx].name = name;
        logf("menu: 快捷方式改名 -> %s", toUtf8(name).c_str());
        saveAndRefresh();
    };

    h.onMove = [owner](int tab, int idx) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        if (idx < 0 || idx >= (int)cfg().tabs[(size_t)tab].shortcuts.size()) return;

        // ⚠ 同 onEdit：对话框之前只拷值，不拿引用。
        //
        //   这里崩得比 onEdit 更频繁，因为用户要在对话框里输数字，
        //   停留时间更长 —— 期间发生配置改动（增删标签、
        //   甚至岛台那边加了个快捷方式）的概率大得多。
        //
        //   实测崩溃栈（cdb + PDB）：
        //     HoverPanel::redraw → std::vector<Shortcut>::empty → cfg()
        //   就是引用悬空之后，后续重绘读到了已释放的 vector。
        const int total = (int)cfg().tabs[(size_t)tab].shortcuts.size();
        if (total <= 1) return;
        const int current = idx + 1;
        const std::wstring curName = cfg().tabs[(size_t)tab].shortcuts[(size_t)idx].name;

        std::wstring prompt = L"「" + curName + L"」当前在第 " +
                              std::to_wstring(current) + L" 位\n请输入目标位置 (1 ~ " +
                              std::to_wstring(total) + L"):";
        std::wstring input;
        if (!askString(owner, L"调换序列", prompt.c_str(), std::to_wstring(current), input)) return;

        int pos = 0;
        try {
            pos = std::stoi(input);
        } catch (...) {
            showWarning(owner, L"无效输入", L"请输入一个有效的数字。");
            return;
        }

        // 对话框返回后重新校验 —— 标签和快捷方式都可能已经不在了
        if (tab < 0 || tab >= (int)cfg().tabs.size()) {
            logf("menu: 调序取消 —— 标签已不存在");
            return;
        }
        const int nowTotal = (int)cfg().tabs[(size_t)tab].shortcuts.size();
        if (idx < 0 || idx >= nowTotal) {
            logf("menu: 调序取消 —— 快捷方式已不存在");
            return;
        }
        // 用**当前**的长度校验，不用对话框之前那个 total
        if (pos < 1 || pos > nowTotal) {
            showWarning(owner, L"超出范围",
                        (L"请输入 1 到 " + std::to_wstring(nowTotal) + L" 之间的数字。").c_str());
            return;
        }

        const int newIdx = pos - 1;
        if (newIdx == idx) return;

        Tab& t = cfg().tabs[(size_t)tab];      // 这里才取引用，而且马上用
        Shortcut sc = t.shortcuts[(size_t)idx];
        t.shortcuts.erase(t.shortcuts.begin() + idx);
        t.shortcuts.insert(t.shortcuts.begin() + newIdx, sc);
        logf("menu: 快捷方式 %s 移到第 %d 位", toUtf8(sc.name).c_str(), pos);
        saveAndRefresh();
    };

    h.onOpenLocation = [](int tab, int idx) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        Tab& t = cfg().tabs[(size_t)tab];
        if (idx < 0 || idx >= (int)t.shortcuts.size()) return;
        std::wstring target = shellPath(t.shortcuts[(size_t)idx].targetPath);
        if (fileExists(target)) {
            std::wstring sel = L"/select,\"" + target + L"\"";
            ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
            ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
            logf("menu: 打开文件位置 %s", toUtf8(target).c_str());
        } else {
            showWarning(bar().hwnd(), L"文件不存在",
                        (L"目标文件不存在:\n" + target + L"\n\n可能已被移动或删除。").c_str());
        }
    };

    h.onRenameTab = [](int tab, const std::wstring& name) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        cfg().tabs[(size_t)tab].name = name;
        logf("menu: 标签改名 -> %s", toUtf8(name).c_str());
        saveAndRefresh();
    };

    h.onAddTab = [](int tab) {
        std::wstring name;
        if (!askString(bar().hwnd(), L"添加标签", L"请输入新标签名称:", L"", name)) return;
        if (name.empty()) return;
        Tab t;
        t.id = shortId();
        t.name = name;
        t.order = (int)cfg().tabs.size();
        cfg().tabs.push_back(t);
        logf("menu: 新增标签 %s", toUtf8(name).c_str());
        saveAndRefresh();
    };

    h.onDeleteTab = [](int tab) {
        if (tab < 0 || tab >= (int)cfg().tabs.size()) return;
        if (cfg().tabs.size() <= 1) {
            showWarning(bar().hwnd(), L"无法删除", L"至少保留一个标签。");
            return;
        }
        // ⚠ 对话框之前只拷值，不留引用。
        //
        //   踩过的坑（同类问题的第三处）：
        //     Tab& t = cfg().tabs[tab];
        //     ...askYesNo(模态)...
        //     logf(..., t.name);     ← 对话框之后再用 t，此时可能已悬空
        //
        //   这里 t.name 只用来写日志，看起来"不会崩"，
        //   但读已释放的 std::wstring 同样是未定义行为 ——
        //   长度字段可能已经是指向别处的垃圾值，照样会访问违规。
        const std::wstring tabName = cfg().tabs[(size_t)tab].name;
        const size_t scCount = cfg().tabs[(size_t)tab].shortcuts.size();

        std::wstring msg = L"确定删除标签「" + tabName + L"」吗？";
        if (scCount > 0) {
            msg += L"\n其中的 " + std::to_wstring(scCount) + L" 个快捷方式也会被删除。";
        }
        if (!askYesNo(bar().hwnd(), L"确认删除", msg.c_str())) return;

        // 对话框返回后重新校验 —— 标签数可能已经变了
        if (tab < 0 || tab >= (int)cfg().tabs.size()) {
            logf("menu: 删除标签取消 —— 标签已不存在");
            return;
        }

        logf("menu: 删除标签 %s", toUtf8(tabName).c_str());
        cfg().tabs.erase(cfg().tabs.begin() + tab);
        for (size_t i = 0; i < cfg().tabs.size(); ++i) cfg().tabs[i].order = (int)i;
        bar().setActive(-1);
        hidePanelWire();
        saveAndRefresh();
    };

    h.onMoveTabLeft = [](int tab) {
        if (tab <= 0 || tab >= (int)cfg().tabs.size()) return;
        std::swap(cfg().tabs[(size_t)tab], cfg().tabs[(size_t)tab - 1]);
        for (size_t i = 0; i < cfg().tabs.size(); ++i) cfg().tabs[i].order = (int)i;
        logf("menu: 标签左移 %d -> %d", tab, tab - 1);
        saveAndRefresh();
    };

    h.onMoveTabRight = [](int tab) {
        if (tab < 0 || tab >= (int)cfg().tabs.size() - 1) return;
        std::swap(cfg().tabs[(size_t)tab], cfg().tabs[(size_t)tab + 1]);
        for (size_t i = 0; i < cfg().tabs.size(); ++i) cfg().tabs[i].order = (int)i;
        logf("menu: 标签右移 %d -> %d", tab, tab + 1);
        saveAndRefresh();
    };

    h.onOpenSettings = []() { openSettings(); };
    h.onQuit = []() { PostMessageW(msgWnd(), WM_CLOSE, 0, 0); };
    return h;
}

void trayMenu(HWND owner) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE, bar().visible() ? L"隐藏标签栏" : L"显示标签栏");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"重新加载配置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    bar().setMenuOpen(true);
    int cmd = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner,
                                    nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    bar().setMenuOpen(false);

    switch (cmd) {
        case IDM_TOGGLE:
            if (bar().visible()) {
                // 手动隐藏：临时把 autoHide 当作"已隐藏"处理
                POINT p = {};
                GetCursorPos(&p);
                SetCursorPos(p.x, GetSystemMetrics(SM_CYSCREEN) - 1);
            }
            break;
        case IDM_SETTINGS:
            openSettings();
            break;
        case IDM_RELOAD: {
            std::vector<std::string> warns;
            loadConfig(cfg(), warns);
            bar().onConfigChanged();
            panel().setFontSize(cfg().shortcutFontSize);
            taskbarService().SetAlpha(cfg().taskbarAlpha);
            taskbarService().SetTransparent(cfg().taskbarTransparent);
            break;
        }
        case IDM_EXIT:
            PostMessageW(owner, WM_CLOSE, 0, 0);
            break;
        default:
            break;
    }
}

}  // namespace hr
