// hr/text/island_theme.h — 岛台配色的单一来源
//
// 这个头文件单独拆出来，不依赖 glass.h / config.h 那些重头。
//
// 使用方有三处：
//   · src/island.cpp             面板文字
//   · src/islandbtn.cpp          任务栏上的岛台按钮图标
//   · src/tb/xaml/visualtree.cpp 任务栏托盘文字（GlassTAP.dll）
//
// 三者必须用同一套颜色 —— 各写各的就会出现
// "托盘黑字、面板白字、按钮又一个色"的割裂。
#pragma once

#include <cstdint>

namespace hr {

// ── 浅色方案（深色背景下用）──
//
// 不用纯白（0xFFFFFFFF）—— 纯白在深色玻璃上偏刺眼，
// 且和任务栏系统图标（本机实测约 0xF5）不搭。
constexpr uint32_t kIslandTextMainLight = 0xFFF5F5F7;
constexpr uint32_t kIslandTextDimLight  = 0xFFC2C4CC;

// ── 深色方案（浅色背景下用）──
constexpr uint32_t kIslandTextMainDark = 0xFF1A1A1C;
constexpr uint32_t kIslandTextDimDark  = 0xFF5A5A5E;

// 兼容旧代码：默认（浅色）主色
constexpr uint32_t kIslandTextMain = kIslandTextMainLight;
constexpr uint32_t kIslandTextDim  = kIslandTextDimLight;

// 按"用不用深色"取主色 —— 三个使用方都调这个，
// 保证拿到的值永远一致。
inline uint32_t islandTextMain(bool dark) {
    return dark ? kIslandTextMainDark : kIslandTextMainLight;
}

inline uint32_t islandTextDim(bool dark) {
    return dark ? kIslandTextDimDark : kIslandTextDimLight;
}

}  // namespace hr
