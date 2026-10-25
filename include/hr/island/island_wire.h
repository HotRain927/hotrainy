#pragma once

namespace hr {

// 岛台模块的接线（由 app/main.cpp 调用一次）。
// 逐步把原先堆在 main.cpp 的岛台相关回调搬到这里，见 docs/代码结构.md「main.cpp 瘦身方案」。
void islandWire();

}  // namespace hr