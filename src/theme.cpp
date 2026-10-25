// hr/theme.cpp — 对照 core/theme_colors.py + glass_render.palette()
#include "hr/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace hr {

namespace {

uint32_t parseHex(const char* h) {
    if (!h) return 0xFF000000;
    if (*h == '#') ++h;
    unsigned v = 0;
    for (int i = 0; i < 6 && h[i]; ++i) {
        char c = h[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    }
    return 0xFF000000u | (v & 0x00FFFFFFu);
}

// 主题：一个色相按 10~25% 混进近黑/近白底
uint32_t mixHex(const char* accent, const char* base, double t) {
    return blendColor(parseHex(accent), parseHex(base), t);
}

Theme darkThemeFrom(const char* accent) {
    Theme t;
    t.light = false;
    t.bg = mixHex(accent, "#0B0B0D", 0.86);
    t.hover = mixHex(accent, "#15151A", 0.84);
    t.active = mixHex(accent, "#1F1F26", 0.80);
    t.panelBg = mixHex(accent, "#08080A", 0.90);
    t.text = 0xFFFFFFFF;
    t.textDim = mixHex(accent, "#B4B8C0", 0.78);
    t.accent = parseHex(accent);
    return t;
}

Theme lightThemeFrom(const char* accent) {
    Theme t;
    t.light = true;
    t.bg = mixHex(accent, "#FFFFFF", 0.90);
    t.hover = mixHex(accent, "#FFFFFF", 0.84);
    t.active = mixHex(accent, "#FFFFFF", 0.76);
    t.panelBg = mixHex(accent, "#FFFFFF", 0.95);
    t.text = parseHex("#14161A");
    t.textDim = mixHex(accent, "#5A5F68", 0.72);
    t.accent = parseHex(accent);
    return t;
}

std::vector<std::string> g_names;
std::map<std::string, Theme> g_themes;

void buildThemes() {
    if (!g_themes.empty()) return;

    Theme def;
    def.bg = parseHex("#101114");
    def.hover = parseHex("#181A1E");
    def.active = parseHex("#1F2228");
    def.panelBg = parseHex("#0C0D0F");
    def.text = 0xFFFFFFFF;
    def.textDim = parseHex("#9AA0A8");
    def.accent = parseHex("#4C8DFF");
    g_themes["默认"] = def;

    struct Hue {
        const char* name;
        const char* dark;
        const char* light;
    };
    static const Hue hues[] = {
        {"红", "#FF5F57", "#E03A30"}, {"橙", "#FF9F0A", "#C9720A"},
        {"黄", "#FFD60A", "#A88600"}, {"绿", "#30D158", "#1E9E43"},
        {"青", "#40C8E0", "#1296AF"}, {"蓝", "#4C8DFF", "#1F6FEB"},
        {"紫", "#BF5AF2", "#8E3FD1"},
    };
    for (const auto& h : hues) {
        g_themes[std::string("深") + h.name] = darkThemeFrom(h.dark);
        g_themes[std::string("浅") + h.name] = lightThemeFrom(h.light);
    }

    Theme black;
    black.bg = parseHex("#0C0C0E");
    black.hover = parseHex("#141416");
    black.active = parseHex("#1B1B1E");
    black.panelBg = parseHex("#09090B");
    black.text = 0xFFFFFFFF;
    black.textDim = parseHex("#96969E");
    black.accent = parseHex("#8E8E96");
    g_themes["黑"] = black;

    Theme white;
    white.light = true;
    white.bg = parseHex("#FBFBFC");
    white.hover = parseHex("#F1F2F4");
    white.active = parseHex("#E8EAEE");
    white.panelBg = parseHex("#FFFFFF");
    white.text = parseHex("#14161A");
    white.textDim = parseHex("#666A72");
    white.accent = parseHex("#3A3C40");
    g_themes["白"] = white;

    g_names = {"默认", "深红", "浅红", "深橙", "浅橙", "深黄", "浅黄", "深绿",
               "浅绿", "深青", "浅青", "深蓝", "浅蓝", "深紫", "浅紫", "黑", "白"};
}

GlassPalette g_darkPal;
GlassPalette g_lightPal;
bool g_palInit = false;

void buildPalettes() {
    if (g_palInit) return;
    g_darkPal.tint = 0x0008080A;
    // 用户要求整体固定深色调、并提高透明度：把深色调色层的覆盖度
    // 从 Python 的 0.52/0.60 降到 0.32/0.38，背景透出更多（更贴合脚下）。
    g_darkPal.aTop = 0.32;
    g_darkPal.aBot = 0.38;
    g_darkPal.text = 0xFFF5F5F7;
    g_darkPal.textDim = 0xFFC2C4CC;
    g_darkPal.capsule = 0x1AFFFFFF;        // 白 26
    g_darkPal.capsuleStrong = 0x30FFFFFF;  // 白 48
    g_darkPal.border = 0x34FFFFFF;         // 白 52
    g_darkPal.hairline = 0x54FFFFFF;       // 白 84
    g_darkPal.specular = 56;
    g_darkPal.rowHover = 0x18FFFFFF;       // 白 24

    g_lightPal.tint = 0x00FFFFFF;
    // 与 Python palette(False) 完全一致
    g_lightPal.aTop = 0.58;
    g_lightPal.aBot = 0.48;
    g_lightPal.text = 0xFF17181B;
    g_lightPal.textDim = 0xFF4E5158;
    g_lightPal.capsule = 0x78FFFFFF;        // 白 120
    g_lightPal.capsuleStrong = 0xC3FFFFFF;  // 白 195
    g_lightPal.border = 0xBEFFFFFF;         // 白 190
    g_lightPal.hairline = 0x18000000;       // 黑 24
    g_lightPal.specular = 110;
    g_lightPal.rowHover = 0x96FFFFFF;       // 白 150
    g_palInit = true;
}

}  // namespace

uint32_t blendColor(uint32_t c1, uint32_t c2, double t) {
    double a = aOf(c1) + (aOf(c2) - (double)aOf(c1)) * t;
    double r = rOf(c1) + (rOf(c2) - (double)rOf(c1)) * t;
    double g = gOf(c1) + (gOf(c2) - (double)gOf(c1)) * t;
    double b = bOf(c1) + (bOf(c2) - (double)bOf(c1)) * t;
    auto cl = [](double v) {
        int i = (int)std::lround(v);
        return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    return argb(cl(a), cl(r), cl(g), cl(b));
}

uint32_t withAlpha(uint32_t color, uint8_t a) { return (color & 0x00FFFFFFu) | ((uint32_t)a << 24); }

double luminance(uint32_t c) {
    return 0.2126 * rOf(c) + 0.7152 * gOf(c) + 0.0722 * bOf(c);
}

void readableText(uint32_t avg, bool dark, uint32_t& mainOut, uint32_t& dimOut) {
    // 永远深色玻璃：文字色固定为浅色，不再按脚下画面的平均亮度翻黑。
    // 之前亮背景会把 contrast 拉爆 → 翻成黑字，软件移开后整条就"不好了"。
    (void)avg;
    const GlassPalette& p = glassPalette(dark);
    mainOut = p.text;
    dimOut = p.textDim;
}

const GlassPalette& glassPalette(bool dark) {
    buildPalettes();
    return dark ? g_darkPal : g_lightPal;
}

const std::vector<std::string>& themeNames() {
    buildThemes();
    return g_names;
}

const Theme& themeByName(const std::string& name) {
    buildThemes();
    auto it = g_themes.find(name);
    if (it == g_themes.end()) return g_themes["默认"];
    return it->second;
}

bool themeIsLight(const std::string& name) {
    const Theme& t = themeByName(name);
    return luminance(t.bg) > 140.0;
}

std::string toHex(uint32_t c) {
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", rOf(c), gOf(c), bOf(c));
    return std::string(buf);
}

}  // namespace hr
