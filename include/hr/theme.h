// hr/theme.h — 主题色（16 套）与玻璃调色板
// 对照 core/theme_colors.py 与 core/glass_render.py 的 palette()
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr {

// 全部颜色一律 0xAARRGGBB
struct Theme {
    bool light = false;
    uint32_t bg = 0xFF101114;
    uint32_t hover = 0xFF181A1E;
    uint32_t active = 0xFF1F2228;
    uint32_t panelBg = 0xFF0C0D0F;
    uint32_t text = 0xFFFFFFFF;
    uint32_t textDim = 0xFF9AA0A8;
    uint32_t accent = 0xFF4C8DFF;
};

// 玻璃调色板：整个界面只用其中一套，不在部件之间切换
struct GlassPalette {
    uint32_t tint = 0x0008080A;   // 只取 RGB 部分
    double aTop = 0.52;
    double aBot = 0.60;
    uint32_t text = 0xFFF5F5F7;
    uint32_t textDim = 0xFFC2C4CC;
    uint32_t capsule = 0x1AFFFFFF;
    uint32_t capsuleStrong = 0x30FFFFFF;
    uint32_t border = 0x34FFFFFF;
    uint32_t hairline = 0x54FFFFFF;
    int specular = 56;
    uint32_t rowHover = 0x18FFFFFF;
};

const GlassPalette& glassPalette(bool dark);

const std::vector<std::string>& themeNames();
const Theme& themeByName(const std::string& name);
bool themeIsLight(const std::string& name);

// ── 颜色工具 ──
inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
inline uint8_t aOf(uint32_t c) { return (uint8_t)(c >> 24); }
inline uint8_t rOf(uint32_t c) { return (uint8_t)(c >> 16); }
inline uint8_t gOf(uint32_t c) { return (uint8_t)(c >> 8); }
inline uint8_t bOf(uint32_t c) { return (uint8_t)c; }

uint32_t blendColor(uint32_t c1, uint32_t c2, double t);   // t=0 取 c1
uint32_t withAlpha(uint32_t color, uint8_t a);
// 感知亮度 0~255（用于判断深/浅）
double luminance(uint32_t c);
// 按玻璃成品平均色给出能读清的主/次文字色（永远深色玻璃 → 固定浅色文字）
void readableText(uint32_t surfaceAvg, bool dark, uint32_t& mainOut, uint32_t& dimOut);

std::string toHex(uint32_t c);

}  // namespace hr
