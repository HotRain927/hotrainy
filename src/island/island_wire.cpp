#include "hr/island/island_wire.h"

#include "hr/app/app_context.h"
#include "hr/island/island.h"
#include "hr/shared/config.h"
#include "hr/shared/common.h"

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
}

}  // namespace hr