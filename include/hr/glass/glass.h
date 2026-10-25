// hr/glass/glass.h — 玻璃渲染管线（对照 core/glass_render.py）
//
// 流程：抓屏 → 缩到 1/4 模糊(frost) → 叠竖向色调渐变 → 顶部镜面高光
//      → 圆角遮罩 → 描边 / 分隔线 / 胶囊高亮
// 与 Python 版一致：脏区域只烘焙一次，鼠标划过时只重贴胶囊。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hr {

// 抓下来的一块背景（未模糊，缩到 1/4）
struct Backdrop {
    std::vector<uint32_t> px;  // 0xAARRGGBB
    int sw = 0;
    int sh = 0;
    uint64_t sig = 0;          // 24×3 采样指纹
    bool valid() const { return sw > 0 && sh > 0 && px.size() == (size_t)sw * sh; }
    uint32_t average() const;
};

// 命中用 alpha 下限：按当前「面板透明度」算，保证乘完 SourceConstantAlpha 之后
// 仍然 >= 1（否则分层窗口的命中测试会把那一片当成全透明，鼠标直接穿透到背后窗口）。
// 它自己很小 —— 合成到屏幕上约 1/255，肉眼看不出来。
inline uint8_t hitTestAlphaFor(int opacityPercent) {
    int p = opacityPercent < 0 ? 0 : (opacityPercent > 100 ? 100 : opacityPercent);
    int c = 255 * p / 100;              // 就是 SourceConstantAlpha
    if (c < 1) c = 1;
    int a = (255 + c - 1) / c;          // ceil(255/c) → 合成后 >= 1
    if (a < 2) a = 2;
    if (a > 32) a = 32;                 // 上限：再低也救不回来，而且别真的看得见
    return (uint8_t)a;
}

class Surface {
public:
    bool create(int w, int h);
    void clear();
    int width() const { return w_; }
    int height() const { return h_; }
    const std::vector<uint32_t>& pixels() const { return px_; }
    uint32_t average() const;

    // ── 抓屏 ──
    // offsetY：抓窗口下方 offsetY 处的一条（避面抓到自己）
    // includeLayered=false 时不带 CAPTUREBLT —— 分层窗口会被排除在抓屏之外。
    // 岛台面板自己是分层窗口，拖动中想抓"纯背景"就靠这个（不用藏窗口，
    // 藏窗口会把系统的拖动循环搞中止）。
    static bool capture(const RECT& rc, int offsetY, Backdrop& out,
                        bool includeLayered = true);

    // ── 烘焙 ──
    // 标签栏底：frost(blur) + 竖向色调渐变；radius=0 时不加高光
    bool buildBar(const Backdrop& bd, bool dark, int blur = 14);
    // 悬停面板底：frost(blur=30) + 圆角 + 1px 描边
    bool buildPanel(const Backdrop& bd, bool dark, int radius, int blur = 30);

    // ★ 纯色平铺底：按百分比铺一层中性深色（0 = 什么都不铺，保持全透明），
    //   不抓屏、不模糊、不按背景明暗自适应。所有面板共用同一个百分比。
    void buildFlatPanel(int opacityPercent, int radius, bool dark);

    // ★ 柔和外阴影：只画【内容矩形之外】那一圈羽化暗环，不动画布里的任何内容。
    //
    //   调用约定：this 必须是一张 (内容 + 四周留白) 的画布，contentRect 是
    //   "内容"在这张画布里的位置（例如把 793x400 的内容放进 835x430 的画布，
    //   就是 {21, 9, 814, 409}）。调用方之后用 blendOver() 把内容贴到同一位置。
    //
    //   为什么是"只画外圈"而不是盒式模糊铺满：
    //     · 铺满会把面板主体一起压暗 —— 而主体要求是纯透明的；
    //     · 模糊还会在画布边缘留下没衰减完的 alpha，边上出现一条硬切线。
    //   这里按"到内容圆角矩形的有符号距离"做二次衰减，并且强制在画布边缘归零。
    //
    //   offsetY：影子整体下移几像素（光源在上方，影子偏低才自然）。
    void applyOuterShadow(const RECT& contentRect, int radius, bool dark, int offsetY = 0);

    // 把 src 以 source-over 叠到本画布的 (dx, dy) 处（直通 alpha，不做 premultiply）。
    void blendOver(const Surface& src, int dx, int dy);
    // 玻璃卡片（设置窗口）：frost + 渐变 + 高光 + 一层薄雾，**不含**描边与圆角切边
    // veil：叠在色调之上的一层雾的 alpha（深色压黑 46 / 浅色提白 40），0 表示不叠
    bool buildCard(const Backdrop& bd, bool dark, int radius, int blur, int veil);
    // 壁纸面板底：cover 缩放裁切 + 压暗 + 左侧加重 + 描边
    bool buildWallpaperPanel(const std::wstring& imagePath, bool dark, int radius);

    // ── 叠加 ──
    void addHairline(bool dark);
    void addBorder(int radius, bool dark);
    void drawCapsule(int x1, int y1, int x2, int y2, int radius, uint32_t color);
    void strokeCircle(int cx, int cy, int radius, uint32_t color, float width);
    void drawLine(int x1, int y1, int x2, int y2, uint32_t color, float width);
    void fillRoundRect(int x, int y, int w, int h, int r, uint32_t color);
    void fillRect(int x, int y, int w, int h, uint32_t color);
    void strokeRoundRect(int x, int y, int w, int h, int r, uint32_t color, float width);

    // ★ 面板同款"玻璃边"：外圈 1px 暗轮廓 + 内侧 1px 亮高光，四角都是圆角。
    //
    //   全界面同一套边 —— 面板、标签栏的每个标签、右侧每个小图标、
    //   面板里悬停的那条快捷方式，都走这里，所以观感天然一致。
    //   (x,y,w,h) 是【外框】：轮廓画在第 1 圈、高光画在第 2 圈。
    //   radius 给小时更像矩形（标签/图标用），给大时就是面板那种圆角。
    //   strength：整体亮度系数。1.0 = 常态；>1 让这一圈的边"亮起来"（悬停/激活）。
    void strokeGlassEdge(int x, int y, int w, int h, int radius, bool dark, double strength = 1.0);

    void cutRoundCorners(int radius);      // 圆角外置为全透明

    // 把最外圈像素的 alpha 抬到至少 1。
    //
    // ⚠ 为什么需要它：分层窗口里 alpha=0 的像素**不接收鼠标**，
    //   系统的命中测试直接穿透过去。
    //
    //   而圆角是靠 cutRoundCorners 把角上 alpha 切成 0 做的，
    //   于是"拖右下角改大小"收不到 WM_NCHITTEST —— 边框拖不动。
    //
    //   抬到 1（1/255 不透明度）肉眼看不出来，
    //   但系统就认那里有窗口了。只处理最外圈，
    //   内部维持原样，否则圆角区会浮出一层方形轮廓。
    void ensureHitTestableEdges();

    // ★ 把圆角矩形 rc 内部的 alpha 抬到至少 1（只动 alpha，RGB 保持 0 → 肉眼不可见）。
    //
    // ⚠ 为什么需要：分层窗口的命中测试**只看 alpha**。面板主体是纯透明的
    //   （alpha=0），于是整块面板对鼠标是"空的" —— 鼠标消息直接穿透到背后的窗口，
    //   表现就是"面板里的快捷方式点不到"，而且悬停高亮也不会亮
    //   （因为连 WM_MOUSEMOVE 都收不到）。
    //   实测证据：面板开着时 WindowFromPoint(面板内部) 返回的是背后的浏览器窗口，
    //   只有边框和阴影那几圈（alpha>0）才返回 HotrainyPanel。
    //   抬到 1（1/255 ≈ 0.4% 不透明度）肉眼看不出来，但窗口就开始收鼠标了。
    //   floorAlpha：抬到的 alpha 下限，默认 1（1/255，看不见）。
    //   ⚠ 但提交时会按「面板透明度」把整帧 alpha 再乘一遍（SourceConstantAlpha），
    //     透明度压低时 alpha=1 会被四舍五入成 0 —— 分层窗口立刻又不收鼠标了
    //     （表现：面板里点一下，点穿到背后的窗口）。所以调用方要用
    //     hitTestAlphaFor(panelOpacity) 传一个"缩完仍 >= 1"的值。
    void ensureHitTestableArea(const RECT& rc, int radius, uint8_t floorAlpha = 1);

    // ★ 给"小元素"用的柔和阴影：在任意圆角矩形**外围**画一圈羽化暗环。
    //
    //   和 applyOuterShadow 的区别：那个是画布级的（面板整幅提交时用，
    //   靠"到内容矩形的有符号距离"全画布衰减）；这个只处理一个小矩形，
    //   用于拖动残影这类"浮起来的格子"。两者衰减曲线一致，观感统一。
    //   reach = 往外羽化的像素数。
    void drawSoftShadow(int x, int y, int w, int h, int radius, bool dark, int reach);

    // ★ 按「面板透明度」给主体铺一层中性雾（0 = 不铺，保持纯透明）。
    //   buildFlatPanel = fillFog + 边框；标签栏只要雾、不要边框，
    //   所以单独抽出来（那条横贯全屏的黑线是用户要求删掉的）。
    void fillFog(int opacityPercent, int radius, bool dark);

    // 设置玻璃的不透明度（0.0 = 全透，1.0 = 完全不透明）。
    //
    // ⚠ 默认的 buildPanel / buildBar 会把底色强制成不透明（alpha=255），
    //   靠"把壁纸压暗"来模拟玻璃感 —— 那是**不透明的深色板**，
    //   背后什么都看不见。
    //
    //   要真正透出桌面，必须在烘焙完之后统一降 alpha。
    //   这个方法就是干这个：把整张图的 alpha 乘上一个系数。
    //
    //   0.0~1.0 之外的值会被夹住。调用后要重新提交到窗口才生效。
    void setGlassOpacity(double alpha);

    // 画文字。
    //
    // ⚠ 透明度直接由 color 的 alpha 控制（GDI+ 的 SolidBrush 支持），
    //   不需要额外的 opacity 参数。
    //   想让文字跟着面板透明，就传一个 alpha 低一些的颜色。
    // ★ 全局文字样式：从此所有 drawText 一律"白字黑边"（用户口径）。
    //
    //   forceWhite = true 时忽略调用方给的 RGB，只保留它原来的**主/次层次**
    //   （亮的主文字 → 更实的白；暗的次要文字 → 稍淡的白），再统一描一圈黑边。
    //   这样做的好处：不管底下是浅色壁纸还是深色窗口，字都读得清 ——
    //   纯深色字在深色底上糊、纯白字在浅色底上糊，只有"白字黑边"两边都成立。
    //
    //   描边 = 用黑色在 8 个方向各画一遍，最后把主色压上去（不需要 GDI+ 的路径描边）。
    //   outlineColor 的 alpha = 0 表示关闭描边。
    static void setGlobalTextStyle(uint32_t outlineColor, bool forceWhite);

    void drawText(const wchar_t* text, RECT rc, float sizePx, uint32_t color, int align,
                  bool bold, const wchar_t* family = L"Microsoft YaHei");
    // 画位图（图标）。原样画（不额外透明）。
    void drawBitmap(HBITMAP bmp, int x, int y, int size);

    // 画位图，并额外指定透明度。
    //
    // opacity 是源的额外透明系数（0~1）。
    // 传小于 1 的值会让图标也透出背后 —— 用在玻璃面板上，
    // 让图标和面板的半透明观感一致，而不是像实心贴纸糊在上面。
    void drawBitmapFaded(HBITMAP bmp, int x, int y, int size, double opacity);
    void drawIcon(HICON icon, int x, int y, int size);

    // 截断加省略号（按像素宽度），返回实际绘制用的文本
    static std::wstring fitText(const wchar_t* text, float sizePx, int availPx,
                                const wchar_t* family = L"Microsoft YaHei");
    // 量文字宽度（与 drawText 同一套字体，控件宽度据此算）
    static int measureText(const wchar_t* text, float sizePx,
                           const wchar_t* family = L"Microsoft YaHei", bool bold = false);

    HBITMAP createPremultipliedBitmap(HDC screenDc) const;

private:
    std::vector<uint32_t> px_;
    int w_ = 0;
    int h_ = 0;
};

// 24×3 采样指纹
uint64_t backdropSig(const std::vector<uint32_t>& px, int w, int h);

}  // namespace hr
