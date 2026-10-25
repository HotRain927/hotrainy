#include "hr/island/island_wire.h"

#include "hr/app/app_context.h"
#include "hr/island/island.h"
#include "hr/shared/config.h"

namespace hr {

void islandWire() {
    // 岛台里改动（拖动分类/排序等）后立刻落盘，避免异常退出丢改动
    island().onConfigDirty = []() { saveConfig(cfg()); };
}

}  // namespace hr