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
    void buildFlatPanel(int opacityPercent, int radius, bool dark,
                        const Backdrop* behind = nullptr);   // behind 非空时做边缘折射

    // ★ 整幅内容向内缩 margin 像素（圆角矩形随之内缩），腾出的外圈画柔和投影。
    //   面板窗口尺寸不变 —— 视觉上面板略微变小、并「浮」在桌面上。
    void applyOuterShadow(int margin, int radius, bool dark);
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
