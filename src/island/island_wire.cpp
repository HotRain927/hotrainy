#include "hr/island/island_wire.h"

#include "hr/app/app_context.h"
#include "hr/island/island.h"
#include "hr/shared/config.h"
#include "hr/island/taskbar.h"    // taskbarService()
#include "hr/shared/common.h"
#include "hr/shared/icons.h"      // shellPath()
#include <shellapi.h>             // ShellExecuteW / AllowSetForegroundWindow
#include <string>
#include <vector>

namespace hr {

// ★ 任务栏岛台按钮被点击时的统一处理。
//   原先这段代码在 main.cpp 里写了两份（19 行 × 2，逻辑相同、注释断行不同），
//   现合并为唯一一份；两处安装点都引用本函数，改一处即两处生效。
void onIslandButtonClick() {
    logf("islandbtn: 被点击");

    // 记下时刻，让"点外检查"在接下来这段时间里让路 ——
    // 否则它会先收面板，然后 WM_ACTIVATE 又打开，
    // 净效果是"点了没反应"。
    iconClickTick() = ::GetTickCount();

    // 撤销挂起的"点外收起"：这次点击是冲我们的图标来的，
    // 由 toggle 处理，不该再被当成"点外面"。
    if (pendingCloseTick()) {
    logf("island: 撤销挂起的点外收起（这次点的是岛台图标）");
    pendingCloseTick() = 0;
    }

    RECT r{};
    if (islandBtn().alive()) ::GetWindowRect(islandBtn().hwnd(), &r);
    toggleIslandPanelWire(r);
}

void islandWire() {
    // 岛台里改动（拖动分类/排序等）后立刻落盘，避免异常退出丢改动
    island().onConfigDirty = []() { saveConfig(cfg()); };

    // 启动分类里的快捷方式（网格点击）。
    // 走和标签面板同一条启动路径，参数/工作目录处理才一致。
    island().onLaunchShortcut = [](int index) {
        const std::vector<Shortcut>* items = nullptr;
        if (cfg().islandCategory == "__all__") {
            items = &cfg().islandShortcuts;
        } else {
            for (const Tab& t : cfg().tabs) {
                if (t.id == cfg().islandCategory) { items = &t.shortcuts; break; }
            }
        }
        if (!items || index < 0 || index >= (int)items->size()) return;
        const Shortcut& s = items->at(index);
        // 与标签面板同一条启动路径：规范化路径 + 传 args
        std::wstring target = shellPath(s.targetPath);
        if (target.empty()) return;
        std::wstring args = s.args;
        ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
        HINSTANCE r = ::ShellExecuteW(nullptr, L"open", target.c_str(),
                                      args.empty() ? nullptr : args.c_str(),
                                      nullptr, SW_SHOWNORMAL);
        const INT_PTR code = (INT_PTR)r;
        logf("island: 启动 %s -> %lld", toUtf8(target).c_str(), (long long)code);

        // ⚠ 启动成功后收起面板。
        //
        //   用户点快捷方式 = 他要去用那个程序了，面板还挡在桌面上就碍事。
        //   只在成功时收：ShellExecute 返回值 > 32 才算成功
        //   （<= 32 是各种错误码，这是 Win32 的老约定）。
        //   失败时留着面板 —— 用户能立刻再点一次，不用重新打开面板。
        if (code > 32) {
            island().hide();
            bar().setIslandOpen(false);
            logf("island: 已收起面板（启动成功）");
        }
    };

    // /select, 前缀是 explorer 的参数，其余按普通路径打开
    island().onLaunchPath = [](const std::wstring& target) {
        if (target.rfind(L"/select,", 0) == 0) {
            std::wstring p = target.substr(8);
            std::wstring arg = L"/select,\"" + p + L"\"";
            ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
            ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(),
                            nullptr, SW_SHOWNORMAL);
        } else {
            ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
            ::ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
        }
    };
}

// 岛台/任务栏相关的设置键。
bool island_applySetting(const std::string& key, bool& handled) {
    handled = true;
    if (key == "taskbar_transparent") {
        taskbarService().SetTransparent(cfg().taskbarTransparent);
    } else if (key == "taskbar_alpha") {
        // 强度改动不需要重启引擎，SetAlpha 会唤醒工作线程重放
        taskbarService().SetAlpha(cfg().taskbarAlpha);
    } else {
        handled = false;
    }
    return true;
}


}  // namespace hr