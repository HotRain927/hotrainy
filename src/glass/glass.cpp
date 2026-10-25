// hr/glass.cpp — 对照 core/glass_render.py
#include "hr/glass/glass.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <objidl.h>
#include <gdiplus.h>

#include "hr/shared/common.h"
#include "hr/text/theme.h"

using namespace Gdiplus;

namespace hr {

namespace {

const int kDown = 4;  // 模糊前缩到 1/4

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

struct DibGuard {
    HBITMAP bmp = nullptr;
    HDC dc = nullptr;
    void* bits = nullptr;
    ~DibGuard() {
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
    }
    bool create(HDC ref, int w, int h) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) return false;
        dc = CreateCompatibleDC(ref);
        if (!dc) return false;
        SelectObject(dc, bmp);
        return true;
    }
};

// ── 可分离盒式模糊（跑三遍近似高斯，sigma ≈ r） ──
void boxBlurH(std::vector<uint32_t>& px, int w, int h, int r) {
    if (r <= 0 || w <= 1) return;
    std::vector<uint32_t> tmp(px.size());
    int div = r * 2 + 1;
    for (int y = 0; y < h; ++y) {
        uint32_t* row = &px[(size_t)y * w];
        uint32_t* out = &tmp[(size_t)y * w];
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; ++i) {
            uint32_t c = row[clampi(i, 0, w - 1)];
            sa += (c >> 24) & 0xFF;
            sr += (c >> 16) & 0xFF;
            sg += (c >> 8) & 0xFF;
            sb += c & 0xFF;
        }
        for (int x = 0; x < w; ++x) {
            out[x] = ((uint32_t)(sa / div) << 24) | ((uint32_t)(sr / div) << 16) |
                     ((uint32_t)(sg / div) << 8) | (uint32_t)(sb / div);
            uint32_t ca = row[clampi(x + r + 1, 0, w - 1)];
            uint32_t cs = row[clampi(x - r, 0, w - 1)];
            sa += (int)((ca >> 24) & 0xFF) - (int)((cs >> 24) & 0xFF);
            sr += (int)((ca >> 16) & 0xFF) - (int)((cs >> 16) & 0xFF);
            sg += (int)((ca >> 8) & 0xFF) - (int)((cs >> 8) & 0xFF);
            sb += (int)(ca & 0xFF) - (int)(cs & 0xFF);
        }
    }
    px.swap(tmp);
}

void boxBlurV(std::vector<uint32_t>& px, int w, int h, int r) {
    if (r <= 0 || h <= 1) return;
    std::vector<uint32_t> tmp(px.size());
    int div = r * 2 + 1;
    for (int x = 0; x < w; ++x) {
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; ++i) {
            uint32_t c = px[(size_t)clampi(i, 0, h - 1) * w + x];
            sa += (c >> 24) & 0xFF;
            sr += (c >> 16) & 0xFF;
            sg += (c >> 8) & 0xFF;
            sb += c & 0xFF;
        }
        for (int y = 0; y < h; ++y) {
            tmp[(size_t)y * w + x] = ((uint32_t)(sa / div) << 24) | ((uint32_t)(sr / div) << 16) |
                                     ((uint32_t)(sg / div) << 8) | (uint32_t)(sb / div);
            uint32_t ca = px[(size_t)clampi(y + r + 1, 0, h - 1) * w + x];
            uint32_t cs = px[(size_t)clampi(y - r, 0, h - 1) * w + x];
            sa += (int)((ca >> 24) & 0xFF) - (int)((cs >> 24) & 0xFF);
            sr += (int)((ca >> 16) & 0xFF) - (int)((cs >> 16) & 0xFF);
            sg += (int)((ca >> 8) & 0xFF) - (int)((cs >> 8) & 0xFF);
            sb += (int)(ca & 0xFF) - (int)(cs & 0xFF);
        }
    }
    px.swap(tmp);
}

void boxBlur(std::vector<uint32_t>& px, int w, int h, int r) {
    if (r <= 0) return;
    boxBlurH(px, w, h, r);
    boxBlurV(px, w, h, r);
    boxBlurH(px, w, h, r);
}

// glass_render.py:201 `small.filter(ImageFilter.GaussianBlur(radius / factor))`
// —— PIL 的 GaussianBlur 参数是标准差 σ，不是盒半径。
// 我们用 3 次盒滤波近似高斯，单次盒滤波（半径 r）的方差是 ((2r+1)²−1)/12，
// 跑 3 次后总方差 ×3，所以要反解出等效的 r：
//     3 · ((2r+1)²−1)/12 = σ²   →   r = (√(4σ²+1) − 1) / 2
// 直接拿 blur/kDown 当 r 用会让 σ 偏大约 28%，玻璃看着比 Python 更"糊"。
int sigmaToBoxRadius(double sigma) {
    if (sigma <= 0) return 0;
    double r = (std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0;
    int ri = (int)std::lround(r);
    return ri < 1 ? 1 : ri;
}

// 双线性放大（源很小、且已模糊，与双三次肉眼无差）
void upscaleBilinear(const std::vector<uint32_t>& src, int sw, int sh, std::vector<uint32_t>& dst,
                     int dw, int dh) {
    dst.assign((size_t)dw * dh, 0);
    const float kx = (float)sw / (float)dw;
    const float ky = (float)sh / (float)dh;
    for (int y = 0; y < dh; ++y) {
        float fy = (y + 0.5f) * ky - 0.5f;
        if (fy < 0) fy = 0;
        int y0 = (int)fy;
        if (y0 > sh - 1) y0 = sh - 1;
        int y1 = std::min(y0 + 1, sh - 1);
        float wy = fy - (float)y0;
        if (wy < 0) wy = 0;
        const uint32_t* r0 = &src[(size_t)y0 * sw];
        const uint32_t* r1 = &src[(size_t)y1 * sw];
        uint32_t* out = &dst[(size_t)y * dw];
        for (int x = 0; x < dw; ++x) {
            float fx = (x + 0.5f) * kx - 0.5f;
            if (fx < 0) fx = 0;
            int x0 = (int)fx;
            if (x0 > sw - 1) x0 = sw - 1;
            int x1 = std::min(x0 + 1, sw - 1);
            float wx = fx - (float)x0;
            if (wx < 0) wx = 0;
            uint32_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1];
            int v[3];
            for (int ch = 0; ch < 3; ++ch) {
                int s = 16 - ch * 8;
                double a = ((c00 >> s) & 0xFF) * (1 - wx) + ((c01 >> s) & 0xFF) * wx;
                double b = ((c10 >> s) & 0xFF) * (1 - wx) + ((c11 >> s) & 0xFF) * wx;
                v[ch] = clampi((int)(a * (1 - wy) + b * wy), 0, 255);
            }
            out[x] = 0xFF000000u | ((uint32_t)v[0] << 16) | ((uint32_t)v[1] << 8) | (uint32_t)v[2];
        }
    }
}

// glass_render.py:204 `small.resize((w, h), Image.BICUBIC)`
// —— Python 放大用的是双三次（Catmull-Rom），不是双线性。
// Catmull-Rom 核（a = -0.5）：
//   W(0)=1, W(|d|∈[0,1)) = 1.5|d|³ − 2.5|d|² + 1, W(|d|∈[1,2)) = −0.5|d|³ + 2.5|d|² − 4|d| + 2
void upscaleBicubic(const std::vector<uint32_t>& src, int sw, int sh, std::vector<uint32_t>& dst,
                    int dw, int dh) {
    dst.assign((size_t)dw * dh, 0);
    auto pick = [&](int ix, int iy) -> const uint32_t& {
        return src[(size_t)clampi(iy, 0, sh - 1) * sw + clampi(ix, 0, sw - 1)];
    };
    auto kernel = [](double d) -> double {
        double a = d < 0 ? -d : d;
        if (a < 1.0) return 1.5 * a * a * a - 2.5 * a * a + 1.0;
        if (a < 2.0) return -0.5 * a * a * a + 2.5 * a * a - 4.0 * a + 2.0;
        return 0.0;
    };
    const double kx = (double)sw / (double)dw;
    const double ky = (double)sh / (double)dh;
    for (int y = 0; y < dh; ++y) {
        double fy = (y + 0.5) * ky - 0.5;
        int iy = (int)std::floor(fy);
        double wy[4];
        for (int m = -1; m <= 2; ++m) wy[m + 1] = kernel(fy - (iy + m));
        uint32_t* out = &dst[(size_t)y * dw];
        for (int x = 0; x < dw; ++x) {
            double fx = (x + 0.5) * kx - 0.5;
            int ix = (int)std::floor(fx);
            double wx[4];
            for (int m = -1; m <= 2; ++m) wx[m + 1] = kernel(fx - (ix + m));
            double acc[3] = {0, 0, 0};
            for (int m = -1; m <= 2; ++m) {
                if (wy[m + 1] == 0.0) continue;
                for (int n = -1; n <= 2; ++n) {
                    if (wx[n + 1] == 0.0) continue;
                    double wgt = wy[m + 1] * wx[n + 1];
                    const uint32_t& c = pick(ix + n, iy + m);
                    acc[0] += ((c >> 16) & 0xFF) * wgt;
                    acc[1] += ((c >> 8) & 0xFF) * wgt;
                    acc[2] += (c & 0xFF) * wgt;
                }
            }
            out[x] = 0xFF000000u | ((uint32_t)clamp255((int)std::lround(acc[0])) << 16) |
                     ((uint32_t)clamp255((int)std::lround(acc[1])) << 8) |
                     clamp255((int)std::lround(acc[2]));
        }
    }
}

// frost：饱和度 / 亮度调整
// 对照 glass_render.py:202-203 `ImageEnhance.Color(...).enhance(1.7)` + `ImageEnhance.Brightness(...)`
void adjustSatBright(std::vector<uint32_t>& px, double sat, double bright) {
    for (auto& c : px) {
        double r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        // PIL 的 ImageEnhance.Color 内部用 image.convert('L') 求灰度，
        // 而 convert('L') 是 Rec.601：L = 0.299R + 0.587G + 0.114B。
        // 不能用 Rec.709（0.2126/0.7152/0.0722），否则红/绿的饱和度调整结果对不上。
        double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        r = (luma + (r - luma) * sat) * bright;
        g = (luma + (g - luma) * sat) * bright;
        b = (luma + (b - luma) * sat) * bright;
        c = 0xFF000000u | ((uint32_t)clamp255((int)std::lround(r)) << 16) |
            ((uint32_t)clamp255((int)std::lround(g)) << 8) | clamp255((int)std::lround(b));
    }
}

// 强制深色：把亮度压到 cap 以内（超过的按比例缩回，保留色相）。
// 用户要求玻璃**永远是深色**——亮壁纸/亮窗口经过脚下时不能把玻璃烘浅。
// 软件窗口（深）在下面时的观感就是基准：整体亮度落在 40~70 区间。
static void clampLumaCap(std::vector<uint32_t>& px, double cap) {
    for (auto& c : px) {
        double r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        double luma = 0.299 * r + 0.587 * g + 0.114 * b;
        if (luma > cap) {
            double k = cap / luma;
            r *= k; g *= k; b *= k;
            c = 0xFF000000u | ((uint32_t)clamp255((int)std::lround(r)) << 16) |
                ((uint32_t)clamp255((int)std::lround(g)) << 8) | clamp255((int)std::lround(b));
        }
    }
}

// 竖向渐变覆盖层：alpha 从 aTop 线性到 aBot（tint 为纯色）
void overlayVGrad(std::vector<uint32_t>& px, int w, int h, uint32_t tint, double aTop,
                  double aBot) {
    int tr = rOf(tint), tg = gOf(tint), tb = bOf(tint);
    for (int y = 0; y < h; ++y) {
        double t = (h > 1) ? (double)y / (double)(h - 1) : 0.0;
        double a = aTop + (aBot - aTop) * t;
        uint32_t* row = &px[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            uint32_t c = row[x];
            row[x] = ((uint32_t)clamp255((int)std::lround(rOf(c) + (tr - (double)rOf(c)) * a))
                      << 16) |
                     ((uint32_t)clamp255((int)std::lround(gOf(c) + (tg - (double)gOf(c)) * a))
                      << 8) |
                     clamp255((int)std::lround(bOf(c) + (tb - (double)bOf(c)) * a)) | 0xFF000000u;
        }
    }
}

// 圆角遮罩：cover ∈ [0,1] 的覆盖率（角部距离场抗锯齿）
inline double cornerCover(int x, int y, int w, int h, int r) {
    if (r <= 0) return 1.0;
    bool nearCorner = (x < r || x >= w - r) && (y < r || y >= h - r);
    if (!nearCorner) return 1.0;
    double cx = (x < r) ? r : w - r - 1;
    double cy = (y < r) ? r : h - r - 1;
    double dx = x - cx, dy = y - cy;
    double dist = std::sqrt(dx * dx + dy * dy);
    double cover = r - dist;
    return cover < 0 ? 0.0 : (cover > 1 ? 1.0 : cover);
}

}  // namespace

// ── 指纹 / 平均色 ──

uint64_t backdropSig(const std::vector<uint32_t>& px, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    uint64_t sig = 1469598103934665603ull;
    for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 24; ++gx) {
            int x = std::min(w - 1, (gx * 2 + 1) * w / 48);
            int y = std::min(h - 1, (gy * 2 + 1) * h / 6);
            uint32_t c = px[(size_t)y * w + x];
            for (int s = 16; s >= 0; s -= 8) {
                sig ^= (uint64_t)(((c >> s) & 0xFF) / 8);
                sig *= 1099511628211ull;
            }
        }
    }
    return sig;
}

uint32_t Backdrop::average() const {
    if (!valid()) return 0xFF18181C;
    double r = 0, g = 0, b = 0;
    for (uint32_t c : px) {
        r += rOf(c);
        g += gOf(c);
        b += bOf(c);
    }
    double n = (double)px.size();
    return rgb((uint8_t)std::lround(r / n), (uint8_t)std::lround(g / n),
               (uint8_t)std::lround(b / n));
}

uint32_t Surface::average() const {
    if (px_.empty()) return 0xFF18181C;
    double r = 0, g = 0, b = 0;
    for (uint32_t c : px_) {
        r += rOf(c);
        g += gOf(c);
        b += bOf(c);
    }
    double n = (double)px_.size();
    return rgb((uint8_t)std::lround(r / n), (uint8_t)std::lround(g / n),
               (uint8_t)std::lround(b / n));
}

// ── Surface ──

bool Surface::create(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    w_ = w;
    h_ = h;
    px_.assign((size_t)w * h, 0);
    return true;
}

void Surface::clear() { std::fill(px_.begin(), px_.end(), 0u); }

bool Surface::capture(const RECT& rc, int offsetY, Backdrop& out, bool includeLayered) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    int sw = std::max(1, w / kDown);
    int sh = std::max(1, h / kDown);

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    // glass_render.py:103 `_grab_win32` 是 BitBlt 1:1 抓完整分辨率，
    // 再由 frost 里 `img.resize((fw,fh), Image.BILINEAR)` 缩到 1/4。
    // 这里照做：先用 BitBlt 1:1 抓原图，再用双线性缩到 1/4。
    // 不能用 StretchBlt + HALFTONE 一步到位：HALFTONE 会抖动，
    // 颜色和模糊质感都和 Python 的 BILINEAR 明显不同（这正是「玻璃没 py 好」的根因）。
    DibGuard full;
    bool ok = full.create(screen, w, h);
    if (ok) {
        // Python 用了 SRCCOPY | CAPTUREBLT，CAPTUREBLT 能拍到分层窗口
        // CAPTUREBLT(0x40000000) 会把**分层窗口**也拍进来；
        // 拖动抓背景时不要它 —— 岛台面板自己就是分层窗口，少了它面板就不会入镜。
        BitBlt(full.dc, 0, 0, w, h, screen, rc.left, rc.top + offsetY,
               SRCCOPY | (includeLayered ? 0x40000000u : 0u));
    }
    ReleaseDC(nullptr, screen);
    if (!ok) return false;

    // GDI 的 DIB 是 BGRA，转成 0xAARRGGBB 的完整分辨率缓冲
    std::vector<uint32_t> fullpx((size_t)w * h);
    const uint8_t* s = (const uint8_t*)full.bits;
    for (int i = 0; i < w * h; ++i) {
        fullpx[i] = 0xFF000000u | ((uint32_t)s[i * 4 + 2] << 16) |
                    ((uint32_t)s[i * 4 + 1] << 8) | (uint32_t)s[i * 4 + 0];
    }

    // 双线性缩小到 1/4（与 PIL BILINEAR 同一坐标映射，缩小也适用）
    out.sw = sw;
    out.sh = sh;
    upscaleBilinear(fullpx, w, h, out.px, sw, sh);
    out.sig = backdropSig(out.px, sw, sh);
    return true;
}

bool Surface::buildBar(const Backdrop& bd, bool dark, int blur) {
    if (w_ <= 0 || h_ <= 0) return false;
    const GlassPalette& p = glassPalette(dark);

    std::vector<uint32_t> buf;
    if (bd.valid()) {
        buf = bd.px;
        int r = sigmaToBoxRadius((double)blur / (double)kDown);
        boxBlur(buf, bd.sw, bd.sh, r);
        adjustSatBright(buf, 1.7, 1.0);   // 固定深色，不提亮（去掉 Python 的 1.12）
        clampLumaCap(buf, 72.0);          // 永远深色：亮背景也不许把玻璃烘浅
        upscaleBicubic(buf, bd.sw, bd.sh, px_, w_, h_);
    } else {
        px_.assign((size_t)w_ * h_, dark ? rgb(18, 18, 20) : rgb(242, 243, 246));
    }

    overlayVGrad(px_, w_, h_, p.tint, p.aTop, p.aBot);
    for (auto& c : px_) c |= 0xFF000000u;
    return true;
}

bool Surface::buildPanel(const Backdrop& bd, bool dark, int radius, int blur) {
    if (w_ <= 0 || h_ <= 0) return false;
    const GlassPalette& p = glassPalette(dark);

    std::vector<uint32_t> buf;
    if (bd.valid()) {
        buf = bd.px;
        int r = sigmaToBoxRadius((double)blur / (double)kDown);
        boxBlur(buf, bd.sw, bd.sh, r);
        adjustSatBright(buf, 1.7, 1.0);   // 固定深色，不提亮（去掉 Python 的 1.12）
        clampLumaCap(buf, 72.0);          // 永远深色：亮背景也不许把玻璃烘浅
        upscaleBicubic(buf, bd.sw, bd.sh, px_, w_, h_);
    } else {
        px_.assign((size_t)w_ * h_, dark ? rgb(18, 18, 20) : rgb(242, 243, 246));
    }

    overlayVGrad(px_, w_, h_, p.tint, p.aTop, p.aBot);

    // 顶部极窄镜面高光：只铺一条，不铺满上半部
    if (radius > 0 && p.specular > 0) {
        int sh = std::max(2, std::min(12, (int)(h_ * 0.06)));
        sh = std::min(sh, h_);
        for (int y = 0; y < sh; ++y) {
            double t = (double)y / (double)std::max(1, sh - 1);
            int a = (int)std::lround(p.specular * (1.0 - t));
            uint32_t* row = &px_[(size_t)y * w_];
            for (int x = 0; x < w_; ++x) {
                row[x] = blendColor(row[x] | 0xFF000000u, 0xFFFFFFFF, a / 255.0);
            }
        }
    }

    for (auto& c : px_) c |= 0xFF000000u;
    addBorder(radius, dark);
    cutRoundCorners(radius);
    return true;
}

// 玻璃卡片：比面板多压一层薄雾（设置窗口面积大、文字多，压不住会发飘）。
// 不含描边和圆角切边——调用方要先算平均色，再描边、再切角。
bool Surface::buildCard(const Backdrop& bd, bool dark, int radius, int blur, int veil) {
    if (w_ <= 0 || h_ <= 0) return false;
    const GlassPalette& p = glassPalette(dark);

    if (bd.valid()) {
        std::vector<uint32_t> buf = bd.px;
        int r = sigmaToBoxRadius((double)blur / (double)kDown);
        boxBlur(buf, bd.sw, bd.sh, r);
        adjustSatBright(buf, 1.7, 1.0);   // 固定深色，不提亮（去掉 Python 的 1.12）
        clampLumaCap(buf, 72.0);          // 永远深色：亮背景也不许把玻璃烘浅
        upscaleBicubic(buf, bd.sw, bd.sh, px_, w_, h_);
    } else {
        px_.assign((size_t)w_ * h_, dark ? rgb(18, 18, 20) : rgb(242, 243, 246));
    }

    overlayVGrad(px_, w_, h_, p.tint, p.aTop, p.aBot);

    if (radius > 0 && p.specular > 0) {
        int sh = std::max(2, std::min(12, (int)(h_ * 0.06)));
        sh = std::min(sh, h_);
        for (int y = 0; y < sh; ++y) {
            double t = (double)y / (double)std::max(1, sh - 1);
            int a = (int)std::lround(p.specular * (1.0 - t));
            uint32_t* row = &px_[(size_t)y * w_];
            for (int x = 0; x < w_; ++x) {
                row[x] = blendColor(row[x] | 0xFF000000u, 0xFFFFFFFF, a / 255.0);
            }
        }
    }

    for (auto& c : px_) c |= 0xFF000000u;

    // 薄雾：深色压黑、浅色提白，加在描边之前，所以圆角外不会出紫边
    if (veil > 0) {
        uint32_t fog = dark ? 0xFF000000u : 0xFFFFFFFFu;
        double t = std::min(255, veil) / 255.0;
        for (auto& c : px_) c = blendColor(c | 0xFF000000u, fog, t) | 0xFF000000u;
    }
    return true;
}

// ★ 全局面板透明度：只铺一层纯色，不做任何模糊/抓屏/明暗自适应。
//   opacityPercent = 0 时不铺（面板完全透出背后内容，只剩图标与文字）。
// ★ 全局面板透明度 + 恒定细边框。
//   opacityPercent = 0 时不铺底色（完全透出背后内容，只剩图标与文字）；
//   无论底色铺不铺，都画一圈极细的"液态"边框 —— 面板透明了，边框还在。
void Surface::buildFlatPanel(int opacityPercent, int radius, bool dark) {
    if (width() <= 0 || height() <= 0) return;
    int p = opacityPercent;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    (void)p;   // 主体保持纯透明（不铺任何色底），浓淡由窗口级 alpha 负责

    // ═══════════════════════════════════════════════════════════════════════
    //  液态玻璃边框（思路参考同类"玻璃面板"工具的做法：把边沿当作有厚度的
    //  小斜面来打光，而不是画一条线。这里用 CPU 描边模拟，不依赖任何 shader。）
    //
    //  ① 微斜面：5 圈同心圆角描边，从外到内亮度按方向光渐变 → 厚度感
    //  ② 方向光：左上受光偏亮、右下背光偏暗
    //  ③ 色散暗示：最外圈极淡冷色、最内圈极淡暖色（很克制，3~6/255）
    //  ④ 内侧光晕：斜面再往里一圈极淡高光，让"玻璃厚度"过渡自然
    // ═══════════════════════════════════════════════════════════════════════
    const int rings = 5;
    // 受光/背光的基准色（深色主题下用亮边，浅色主题下用暗边）
    const uint32_t litR = dark ? 0xFFu : 0x00u, litG = dark ? 0xFFu : 0x00u, litB = dark ? 0xFFu : 0x00u;

    auto ring = [&](int i) {
        // i = 0 最外圈（接触桌面，最暗）→ i 越大越靠内（越亮，模拟斜面迎光）
        int a;
        if (i == 0)      a = dark ? 0x2Au : 0x26u;   // 接触阴影
        else if (i == 1) a = dark ? 0x3Cu : 0x33u;
        else if (i == 2) a = dark ? 0x50u : 0x40u;
        else if (i == 3) a = dark ? 0x66u : 0x4Cu;   // 受光面
        else             a = dark ? 0x3Cu : 0x2Eu;   // 内侧收一点，避免"发白"
        uint32_t col = ((uint32_t)a << 24) | (litR << 16) | (litG << 8) | litB;
        int x = i, y = i;
        int w = w_ - 2 * i, h = h_ - 2 * i;
        int r = radius - i; if (r < 1) r = 1;
        if (w > 2 && h > 2) strokeRoundRect(x, y, w, h, r, col, 1.0f);
    };
    for (int i = 0; i < rings; ++i) ring(i);

    // ③ 色散暗示：外冷内暖（各 1px，极淡）
    strokeRoundRect(0, 0, w_, h_, radius, 0x0A60A0FFu, 1.0f);          // 最外：极淡冷
    {
        int ri = rings - 1;
        int w2 = w_ - 2 * ri, h2 = h_ - 2 * ri;
        int r2 = radius - ri; if (r2 < 1) r2 = 1;
        if (w2 > 2 && h2 > 2) strokeRoundRect(ri, ri, w2, h2, r2, 0x0AFFB060u, 1.0f);   // 内圈：极淡暖
    }

    // ④ 内侧光晕：斜面再向内一圈的柔光，模拟玻璃内部的厚度过渡
    {
        int gi = rings;
        int w3 = w_ - 2 * gi, h3 = h_ - 2 * gi;
        int r3 = radius - gi; if (r3 < 1) r3 = 1;
        uint32_t glow = dark ? 0x14FFFFFFu : 0x10000000u;
        if (w3 > 2 && h3 > 2) strokeRoundRect(gi, gi, w3, h3, r3, glow, 1.0f);
    }

    // ⑤ 左上角单独补一道更亮的高光（方向光的最强点，强化"迎光"）
    {
        uint32_t spec = dark ? 0x5AFFFFFFu : 0x44000000u;
        int len = w_ / 3;
        if (len > 4) {
            // 用两段短描边近似左上圆角处的高光
            strokeRoundRect(0, 0, w_, h_, radius, spec, 1.0f);
        }
    }
}

bool Surface::buildWallpaperPanel(const std::wstring& path, bool dark, int radius) {
    if (w_ <= 0 || h_ <= 0) return false;
    bool loaded = false;
    {
        Bitmap src(path.c_str());
        if (src.GetLastStatus() == Ok && src.GetWidth() > 0 && src.GetHeight() > 0) {
            // cover：等比放大到铺满，再居中裁切
            double ratio = std::max((double)w_ / src.GetWidth(), (double)h_ / src.GetHeight());
            int nw = std::max(w_, (int)(src.GetWidth() * ratio + 0.5));
            int nh = std::max(h_, (int)(src.GetHeight() * ratio + 0.5));
            Bitmap sized(nw, nh, PixelFormat32bppARGB);
            Graphics g(&sized);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.DrawImage(&src, Rect(0, 0, nw, nh), 0, 0, src.GetWidth(), src.GetHeight(),
                        UnitPixel);
            int left = (nw - w_) / 2, top = (nh - h_) / 2;

            Bitmap dst(w_, h_, w_ * 4, PixelFormat32bppARGB, (BYTE*)px_.data());
            Graphics g2(&dst);
            g2.SetCompositingMode(CompositingModeSourceCopy);
            g2.DrawImage(&sized, Rect(0, 0, w_, h_), left, top, w_, h_, UnitPixel);
            loaded = true;
        }
    }
    if (!loaded) {
        px_.assign((size_t)w_ * h_, rgb(26, 26, 30));
    }

    // 整体压一层薄暗
    for (auto& c : px_) c = blendColor(c | 0xFF000000u, 0xFF000000u, 0.18);

    // 左侧加重（文字都在左边）
    const double hold = 0.30, fade = 0.92, alpha = 118;
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            double t = (w_ > 1) ? (double)x / (double)(w_ - 1) : 0.0;
            double a;
            if (t <= hold) a = alpha;
            else if (t >= fade) a = 0;
            else a = alpha * (1.0 - (t - hold) / (fade - hold));
            if (a <= 0) continue;
            px_[(size_t)y * w_ + x] =
                blendColor(px_[(size_t)y * w_ + x] | 0xFF000000u, 0xFF000000u, a / 255.0);
        }
    }

    const GlassPalette& p = glassPalette(dark);
    if (radius > 0 && p.specular > 0) {
        int sh = std::max(2, std::min(12, (int)(h_ * 0.06)));
        sh = std::min(sh, h_);
        for (int y = 0; y < sh; ++y) {
            double t = (double)y / (double)std::max(1, sh - 1);
            int a = (int)std::lround(p.specular * 0.55 * (1.0 - t));
            for (int x = 0; x < w_; ++x) {
                px_[(size_t)y * w_ + x] =
                    blendColor(px_[(size_t)y * w_ + x] | 0xFF000000u, 0xFFFFFFFF, a / 255.0);
            }
        }
    }
    for (auto& c : px_) c |= 0xFF000000u;
    addBorder(radius, dark);
    cutRoundCorners(radius);
    return true;
}

void Surface::addHairline(bool dark) {
    if (h_ <= 0) return;
    uint32_t color = glassPalette(dark).hairline;
    double a = aOf(color) / 255.0;
    uint32_t* row = &px_[(size_t)(h_ - 1) * w_];
    for (int x = 0; x < w_; ++x) {
        // 必须是"叠在不透明底上"，alpha 要强制 255，
        // 否则 blendColor 会把 alpha 一起拉低，这一行会半透明发暗
        row[x] = blendColor(row[x] | 0xFF000000u, color, a) | 0xFF000000u;
    }
}

void Surface::addBorder(int radius, bool dark) {
    if (w_ <= 0 || h_ <= 0) return;
    uint32_t color = glassPalette(dark).border;
    double a = aOf(color) / 255.0;

    // 1px 内描边：用圆角距离场找出 |dist - (r-0.5)| <= 0.75 的像素
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            double d;
            bool edgeX = (x == 0 || x == w_ - 1);
            bool edgeY = (y == 0 || y == h_ - 1);
            bool nearCorner = radius > 0 && (x < radius || x >= w_ - radius) &&
                              (y < radius || y >= h_ - radius);
            if (nearCorner) {
                double cx = (x < radius) ? radius : w_ - radius - 1;
                double cy = (y < radius) ? radius : h_ - radius - 1;
                double dx = x - cx, dy = y - cy;
                d = std::fabs(std::sqrt(dx * dx + dy * dy) - (radius - 0.5));
                if (d > 0.9) continue;
            } else if (!edgeX && !edgeY) {
                continue;
            }
            px_[(size_t)y * w_ + x] =
                blendColor(px_[(size_t)y * w_ + x] | 0xFF000000u, color, a) | 0xFF000000u;
        }
    }
}

void Surface::cutRoundCorners(int radius) {
    if (radius <= 0) return;
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            double cover = cornerCover(x, y, w_, h_, radius);
            if (cover >= 1.0) continue;
            uint32_t c = px_[(size_t)y * w_ + x];
            uint32_t a = (uint32_t)std::lround(aOf(c) * cover);
            px_[(size_t)y * w_ + x] = (a << 24) | (c & 0x00FFFFFFu);
        }
    }
}
// 把整张图的 alpha 乘上一个系数，做出真正的半透明。
//
// ⚠ 为什么需要它：
//   buildPanel / buildBar 最后有一句
//       for (auto& c : px_) c |= 0xFF000000u;
//   把底色强制成完全不透明 —— 那是**不透明的深色板**，
//   只是靠压暗壁纸来"像玻璃"，背后其实什么都看不见。
//
//   要真透出桌面，只能在这里统一降 alpha。
//
// 实现要点：
//   · 只改 alpha，不动 RGB。RGB 是已经烘焙好的玻璃色，
//     动了会偏色。
//   · 夹到 0.05~1.0。0 会让窗口完全消失（而且分层窗口下
//     alpha=0 连鼠标都不收），留一点下限更安全。
void Surface::setGlassOpacity(double alpha) {
    if (alpha < 0.05) alpha = 0.05;
    if (alpha > 1.0) alpha = 1.0;

    for (auto& c : px_) {
        const uint32_t a = (c >> 24) & 0xFF;
        uint32_t na = (uint32_t)std::lround(a * alpha);
        if (na > 255) na = 255;
        c = (na << 24) | (c & 0x00FFFFFFu);
    }
}

// 把最外圈像素的 alpha 抬到至少 1。
//
// ⚠ 分层窗口的命中测试规则：alpha=0 的像素不接收鼠标。
//   圆角外的像素被 cutRoundCorners 切成 0，于是那片区域
//   鼠标直接穿透 —— 拖右下角改大小会完全没反应。
//
//   （实测：拖右边中部能拖，拖右下角不能。差别就在角上 alpha 为 0。）
//
// 抬到 1 就够了：1/255 的不透明度肉眼不可见，
// 但对系统来说那里"有窗口"，命中测试会正常走到 WM_NCHITTEST。
//
// 只处理最外一圈。内部不动 —— 否则整个圆角区会浮出一层方形轮廓。
void Surface::ensureHitTestableEdges() {
    if (w_ <= 2 || h_ <= 2) return;

    auto bump = [this](int x, int y) {
        uint32_t c = px_[(size_t)y * w_ + x];
        if ((c >> 24) == 0) {
            px_[(size_t)y * w_ + x] = (1u << 24) | (c & 0x00FFFFFFu);
        }
    };

    for (int x = 0; x < w_; ++x) { bump(x, 0); bump(x, h_ - 1); }
    for (int y = 0; y < h_; ++y) { bump(0, y); bump(w_ - 1, y); }
}

void Surface::strokeCircle(int cx, int cy, int radius, uint32_t color, float width) {

    int a = aOf(color);

    if (a <= 0 || radius <= 0) return;
    double sa = a / 255.0;
    double half = width * 0.5;
    int x1 = std::max(0, cx - radius - (int)width - 1);
    int x2 = std::min(w_ - 1, cx + radius + (int)width + 1);
    int y1 = std::max(0, cy - radius - (int)width - 1);
    int y2 = std::min(h_ - 1, cy + radius + (int)width + 1);
    for (int y = y1; y <= y2; ++y) {
        for (int x = x1; x <= x2; ++x) {
            double dx = x - cx, dy = y - cy;
            double d = std::sqrt(dx * dx + dy * dy);
            double cover = 1.0 - (std::fabs(d - radius) - half + 0.5);
            if (cover <= 0) continue;
            if (cover > 1) cover = 1;
            uint32_t* p = &px_[(size_t)y * w_ + x];
            *p = blendColor(*p | 0xFF000000u, color, sa * cover) | 0xFF000000u;
        }
    }
}

void Surface::drawLine(int x1, int y1, int x2, int y2, uint32_t color, float width) {
    int a = aOf(color);
    if (a <= 0) return;
    double sa = a / 255.0;
    double half = std::max(0.5, width * 0.5);
    int lx = std::min(x1, x2) - (int)width - 1, rx = std::max(x1, x2) + (int)width + 1;
    int ty = std::min(y1, y2) - (int)width - 1, by = std::max(y1, y2) + (int)width + 1;
    lx = std::max(0, lx);
    ty = std::max(0, ty);
    rx = std::min(w_ - 1, rx);
    by = std::min(h_ - 1, by);

    double dx = x2 - x1, dy = y2 - y1;
    double len2 = dx * dx + dy * dy;
    if (len2 <= 0.0001) return;

    for (int y = ty; y <= by; ++y) {
        for (int x = lx; x <= rx; ++x) {
            double t = ((x - x1) * dx + (y - y1) * dy) / len2;
            t = std::max(0.0, std::min(1.0, t));
            double px = x1 + t * dx, py = y1 + t * dy;
            double d = std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
            double cover = 1.0 - (d - half + 0.5);
            if (cover <= 0) continue;
            if (cover > 1) cover = 1;
            uint32_t* p = &px_[(size_t)y * w_ + x];
            *p = blendColor(*p | 0xFF000000u, color, sa * cover) | 0xFF000000u;
        }
    }
}

void Surface::fillRect(int x, int y, int w, int h, uint32_t color) {
    int a = aOf(color);
    if (a <= 0) return;
    double sa = a / 255.0;
    for (int iy = 0; iy < h; ++iy) {
        int py = y + iy;
        if (py < 0 || py >= h_) continue;
        for (int ix = 0; ix < w; ++ix) {
            int pxx = x + ix;
            if (pxx < 0 || pxx >= w_) continue;
            px_[(size_t)py * w_ + pxx] =
                blendColor(px_[(size_t)py * w_ + pxx] | 0xFF000000u, color, sa) | 0xFF000000u;
        }
    }
}

void Surface::fillRoundRect(int x, int y, int w, int h, int r, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int a = aOf(color);
    if (a <= 0) return;
    double sa = a / 255.0;
    for (int iy = 0; iy < h; ++iy) {
        int py = y + iy;
        if (py < 0 || py >= h_) continue;
        for (int ix = 0; ix < w; ++ix) {
            int pxx = x + ix;
            if (pxx < 0 || pxx >= w_) continue;
            double cover = cornerCover(ix, iy, w, h, r);
            if (cover <= 0) continue;
            px_[(size_t)py * w_ + pxx] =
                blendColor(px_[(size_t)py * w_ + pxx] | 0xFF000000u, color, sa * cover) |
                0xFF000000u;
        }
    }
}

void Surface::drawCapsule(int x1, int y1, int x2, int y2, int radius, uint32_t color) {
    fillRoundRect(x1, y1, x2 - x1 + 1, y2 - y1 + 1, radius, color);
}

void Surface::strokeRoundRect(int x, int y, int w, int h, int r, uint32_t color, float width) {
    int a = aOf(color);
    if (a <= 0) return;
    int t = std::max(1, (int)std::lround(width));
    fillRoundRect(x, y, w, t, r, color);
    fillRoundRect(x, y + h - t, w, t, r, color);
    fillRoundRect(x, y, t, h, r, color);
    fillRoundRect(x + w - t, y, t, h, r, color);
}

void Surface::drawText(const wchar_t* text, RECT rc, float sizePx, uint32_t color, int align,
                       bool bold, const wchar_t* family) {
    if (!text || !*text) return;
    Bitmap bmp(w_, h_, w_ * 4, PixelFormat32bppARGB, (BYTE*)px_.data());
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetCompositingMode(CompositingModeSourceOver);

    FontFamily fam(family);
    FontFamily fallback(L"Segoe UI");
    const FontFamily* f = fam.IsAvailable() ? &fam : &fallback;
    Font font(f, sizePx, (bold ? FontStyleBold : FontStyleRegular), UnitPixel);
    SolidBrush brush(Color(aOf(color), rOf(color), gOf(color), bOf(color)));

    StringFormat fmt;
    fmt.SetAlignment(align == 1 ? StringAlignmentCenter
                                : (align == 2 ? StringAlignmentFar : StringAlignmentNear));
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);

    RectF rect((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
    g.DrawString(text, -1, &font, rect, &fmt, &brush);
}

std::wstring Surface::fitText(const wchar_t* text, float sizePx, int availPx,
                              const wchar_t* family) {
    if (!text || !*text || availPx <= 0) return text ? std::wstring(text) : std::wstring();
    HDC screen = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfHeight = -(LONG)std::lround(sizePx);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, family, 31);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(screen, font);

    std::wstring s(text);
    auto measure = [&](const std::wstring& t) {
        SIZE sz = {};
        GetTextExtentPoint32W(screen, t.c_str(), (int)t.size(), &sz);
        return (int)sz.cx;
    };
    if (measure(s) > availPx) {
        while (!s.empty() && measure(s + L"…") > availPx) s.pop_back();
        s += L"…";
    }
    SelectObject(screen, old);
    DeleteObject(font);
    ReleaseDC(nullptr, screen);
    return s;
}

int Surface::measureText(const wchar_t* text, float sizePx, const wchar_t* family, bool bold) {
    if (!text || !*text) return 0;
    HDC screen = GetDC(nullptr);
    LOGFONTW lf = {};
    lf.lfHeight = -(LONG)std::lround(sizePx);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy(lf.lfFaceName, family, 31);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(screen, font);
    SIZE sz = {};
    GetTextExtentPoint32W(screen, text, (int)wcslen(text), &sz);
    SelectObject(screen, old);
    DeleteObject(font);
    ReleaseDC(nullptr, screen);
    return (int)sz.cx;
}

void Surface::drawBitmap(HBITMAP bmp, int x, int y, int size) {
    drawBitmapFaded(bmp, x, y, size, 1.0);
}

void Surface::drawBitmapFaded(HBITMAP bmp, int x, int y, int size, double opacity) {
    // 不能直接交给 GDI+：HBITMAP 没有 alpha 概念，从 IShellItemImageFactory
    // 拿到的 32bpp 预乘位图一旦走 Bitmap(hbitmap) 就会丢掉透明通道，
    // 图标四周那圈透明区域会变成黑方块。
    // 这里自己 GetDIBits 把 BGRA 读出来，按 source-over 混合（预乘公式）。
    if (!bmp || size <= 0) return;
    BITMAP bm = {};
    if (!GetObjectW(bmp, sizeof(bm), &bm)) return;
    int sw = bm.bmWidth, sh = bm.bmHeight;
    if (sw <= 0 || sh <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh;   // 负数 = 自上而下，和我们的行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> src((size_t)sw * sh);
    HDC screen = GetDC(nullptr);
    if (!screen) return;
    int got = GetDIBits(screen, bmp, 0, (UINT)sh, src.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (got != sh) return;

    // 整张 alpha 全 0 的是老式不透明位图（DrawIconEx 出来的那种）
    uint8_t maxA = 0;
    for (uint32_t c : src) maxA = std::max<uint8_t>(maxA, (uint8_t)(c >> 24));

    for (int dy = 0; dy < size; ++dy) {
        int py = y + dy;
        if (py < 0 || py >= h_) continue;
        // 目标像素中心在源图里的位置
        double sy = (dy + 0.5) * sh / size - 0.5;
        int y0 = (int)std::floor(sy);
        double fy = sy - y0;
        int y1 = std::min(sh - 1, std::max(0, y0));
        y0 = std::min(sh - 1, std::max(0, y0));
        for (int dx = 0; dx < size; ++dx) {
            int px = x + dx;
            if (px < 0 || px >= w_) continue;
            double sx = (dx + 0.5) * sw / size - 0.5;
            int x0 = (int)std::floor(sx);
            double fx = sx - x0;
            int x1 = std::min(sw - 1, std::max(0, x0));
            x0 = std::min(sw - 1, std::max(0, x0));

            auto sample = [&](int qx, int qy) { return src[(size_t)qy * sw + qx]; };
            uint32_t c00 = sample(x0, y0), c10 = sample(x1, y0), c01 = sample(x0, y1),
                     c11 = sample(x1, y1);

            auto mix4 = [&](int shift) {
                double v = ((c00 >> shift & 0xFF) * (1 - fx) + (c10 >> shift & 0xFF) * fx) *
                               (1 - fy) +
                           ((c01 >> shift & 0xFF) * (1 - fx) + (c11 >> shift & 0xFF) * fx) * fy;
                return v;
            };
            double a = maxA ? mix4(24) : 255.0;
            double r = mix4(16), g = mix4(8), b = mix4(0);

            // ⚠ 源 alpha 乘上额外透明度 —— 这是"让图标也透明"的关键一步。
            //
            //   不乘的话，图标以完全不透明的姿态糊在玻璃上，
            //   和面板的半透明观感脱节，看起来像实心贴纸。
            a *= opacity;

            if (a <= 0.5) continue;

            uint32_t& dst = px_[(size_t)py * w_ + px];
            double inv = 1.0 - a / 255.0;
            double nr = r + rOf(dst) * inv;
            double ng = g + gOf(dst) * inv;
            double nb = b + bOf(dst) * inv;
            dst = (dst & 0xFF000000u) | ((uint32_t)std::min(255.0, nr) << 16) |
                  ((uint32_t)std::min(255.0, ng) << 8) | (uint32_t)std::min(255.0, nb);
        }
    }
}

void Surface::drawIcon(HICON icon, int x, int y, int size) {
    if (!icon || size <= 0) return;
    Bitmap src(icon);
    if (src.GetLastStatus() != Ok) return;
    Bitmap dst(w_, h_, w_ * 4, PixelFormat32bppARGB, (BYTE*)px_.data());
    Graphics g(&dst);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(&src, Rect(x, y, size, size), 0, 0, src.GetWidth(), src.GetHeight(), UnitPixel);
}

HBITMAP Surface::createPremultipliedBitmap(HDC screenDc) const {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w_;
    bi.bmiHeader.biHeight = -h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return nullptr;

    uint8_t* dst = (uint8_t*)bits;
    for (size_t i = 0; i < px_.size(); ++i) {
        uint32_t c = px_[i];
        uint32_t a = (c >> 24) & 0xFF;
        dst[i * 4 + 0] = (uint8_t)(bOf(c) * a / 255);
        dst[i * 4 + 1] = (uint8_t)(gOf(c) * a / 255);
        dst[i * 4 + 2] = (uint8_t)(rOf(c) * a / 255);
        dst[i * 4 + 3] = (uint8_t)a;
    }
    return bmp;
}

}  // namespace hr
