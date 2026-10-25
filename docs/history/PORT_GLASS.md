# Python 源码逐句对照结论（玻璃 / 壁纸 / 模糊）

> 依据：`core/glass_render.py`(523) `ui/tab_bar.py`(424) `ui/hover_panel.py`(806)
> `main.py`(1620) `core/dpi.py` `ui/glass_menu.py` `ui/glass_dialog.py`
> `ui/settings_window.py` `ui/search_window.py` `core/wallpaper_store.py` `models/*`

---

## 1. 玻璃模式与壁纸模式：**不是互斥的外观，互斥的只是两个布尔开关**

### 1.1 数据层（main.py:552 / 681）—— 开关互斥

```python
def _set_glass_mode(self, enabled):
    self.config.glass_mode = enabled
    if enabled:
        self.config.wallpaper_mode = False      # 开玻璃 → 关壁纸

def _set_wallpaper_mode(self, enabled):
    self.config.wallpaper_mode = enabled
    if enabled:
        self.config.glass_mode = False          # 开壁纸 → 关玻璃
```

### 1.2 外观层（main.py:488）—— **是「或」，不是互斥**

```python
def _glass_appearance_on(self) -> bool:
    """玻璃状态和壁纸状态是互斥的两个状态：
         · 玻璃状态：全部玻璃，没有任何壁纸；
         · 壁纸状态：标签栏和玻璃状态长得一模一样，只有快捷方式面板换成壁纸。
       所以只要任意一个开着，就按玻璃外观绘制。"""
    return bool(glass_mode or wallpaper_mode) and PIL_OK
```

**→ 壁纸模式下，标签栏仍然是完整的液态玻璃。** 这是本次最关键的修正点。

### 1.3 面板底（hover_panel.py:152 / 234 / 246）

```python
@property
def _use_wallpaper(self):            # 真要用壁纸：模式开 + 该标签有壁纸
    return bool(self.wallpaper_mode and self.wallpaper)

@property
def _glass_like(self):               # 悬浮圆角卡片外观：玻璃 OR (壁纸模式且有壁纸)
    return bool(self.glass or self._use_wallpaper) and PIL_OK

# _ensure_glass():
if self._use_wallpaper:
    img = glass_render.build_wallpaper_panel(w, h, self.wallpaper,
                                             radius=px(14), dark=dark)
else:
    img = glass_render.build_panel(w, h, backdrop, dark, radius=px(14))
```

**→ 壁纸模式开但该标签没壁纸时，面板回落成玻璃（前提是 glass 开着）。**
**→ 玻璃关 + 壁纸关 → 普通实色模式。**

### 1.4 标签栏对壁纸模式完全无感（tab_bar.py:104）

```python
def set_wallpaper_mode(self, enabled: bool):
    """壁纸模式只作用于悬停面板，标签栏本身仍与玻璃模式完全一致。
    保留这个接口只为让 main.py 的调用点不必改，内部不改变任何绘制。"""
    return
```

### 1.5 设置壁纸会自动切模式（main.py:722）

```python
tab.wallpaper = saved
if not self.config.wallpaper_mode:
    self.config.wallpaper_mode = True
    self.config.glass_mode = False       # ← 开壁纸就关玻璃（数据层互斥）
    self._refresh_visual_modes()
```

---

## 2. 模糊：`frost()`（glass_render.py:195）

```python
def frost(img, radius=14, factor=4, saturation=1.7, brightness=1.08):
    small = img.resize((w//4, h//4), Image.BILINEAR)          # 降 1/4，双线性
    small = small.filter(ImageFilter.GaussianBlur(radius/4))   # 半径=σ，在 1/4 尺度上
    small = ImageEnhance.Color(small).enhance(1.7)             # 饱和度 ×1.7
    small = ImageEnhance.Brightness(small).enhance(brightness) # 亮度：
    return small.resize((w, h), Image.BICUBIC)                 # 放大用双三次
```

调用点：`build_surface()` 第 260 行传的是 `brightness = 1.12 if dark else 1.0`
→ **浅色模式 brightness=1.0（不提亮），深色模式 1.12。** 没有其它提亮。

**等效 σ 换算（box blur 3 次近似高斯）**：
单数次 box blur 半径 r 的方差 σ² = ((2r+1)²−1)/12，跑 3 次后 σ_total = √3·σ_box。
要匹配目标 σ：
```
r = (√(4σ² + 1) − 1) / 2
```
- 标签栏 blur=14 → σ=3.5 → r ≈ 3.0（原来用 round(14/4)=4，σ 偏大约 28%）
- 面板 blur=30 → σ=7.5 → r ≈ 7.0（原来用 8）

各部件 blur：标签栏 14、面板 30、菜单 26、弹窗 30、搜索窗 26、设置窗 30。

---

## 3. 合成：`build_surface()`（glass_render.py:246）

```python
p = palette(dark)
base = backdrop or 纯色(18,18,20)/(242,243,246)
out = alpha_composite(
    frost(base, radius=blur, brightness=1.12 if dark else 1.0),
    _vgrad((w,h), tint+(255*a_top,), tint+(255*a_bot,))     # 竖向色调渐变
)
if radius > 0 and p["specular"] > 0:                         # 标签栏 radius=0 → 无高光
    sh = max(2, min(12, int(h*0.06)))
    out.alpha_composite(_vgrad((w,sh), (255,255,255,specular), (255,255,255,0)), (0,0))
```

调色板（已与 C++ 一致）：

| | 深色 | 浅色 |
|---|---|---|
| tint | (8,8,10) | (255,255,255) |
| a_top / a_bot | 0.52 / 0.60 | 0.58 / 0.48 |
| text / text_dim | #F5F5F7 / #C2C4CC | #17181B / #4E5158 |
| capsule / capsule_strong | 白26 / 白48 | 白120 / 白195 |
| border | 白52 | 白190 |
| hairline | 白84 | 黑24 |
| specular | 56 | 110 |
| row_hover | 白24 | 白150 |

---

## 4. 壁纸面板：`build_wallpaper_panel()`（glass_render.py:371）

```python
card = load_cover(path, w, h)                       # LANCZOS 等比铺满 + 居中裁切
if card is None: card = 纯色 (26,26,30)
if scrim > 0:  card = Image.blend(card, 黑, 0.18)   # 整体压暗 18%
img = card.convert("RGBA")
img.alpha_composite(_left_scrim((w,h), 118))        # 左侧压暗，见下
p = palette(dark)                                   # ← dark 用全局色调，不是硬编码 True
if radius > 0 and p["specular"] > 0:
    sh = max(2, min(12, int(h*0.06)))
    img.alpha_composite(_vgrad((w,sh), (255,255,255, int(specular*0.55)), (255,255,255,0)), (0,0))
img = add_border(img, radius, dark)
return to_chroma(img, radius)                       # 圆角外填抠色
```

`_left_scrim(size, alpha=118, hold=0.30, fade=0.92)`（第 223 行）：
横向 x/w ≤ 0.30 满强度 118，0.30→0.92 线性衰减到 0，之后为 0；颜色是纯黑。

**→ 壁纸面板与玻璃面板「形状完全一致」：圆角 + 1px 描边 + 圆角外抠色。**

---

## 5. 面板行高亮（hover_panel.py:255）—— 是**不透明实色**

```python
base = self._glass_avg or ((20,20,22) if dark else (240,240,242))
hover = blend(base, (255,255,255), 0.13) if dark else blend(base, (0,0,0), 0.07)
self._row_hover_hex = hex_of(hover)      # "#RRGGBB"，Tk 的 fill 不带 alpha
```

绘制是 `create_rectangle(row_x1, y1+px(2), row_x2, y2-px(2))` → **直角，无圆角**。

---

## 6. 面板几何（hover_panel.py:265）

```python
max_h = int(screen_height * 0.6)
if self._glass_like:
    inset, gap, pad = px(4), px(6), px(6)
    px_, py_ = x + inset, y + gap
    panel_w = max(width - inset*2, px(180))
else:
    pad = px(2); px_, py_ = x, y; panel_w = width
panel_h = min(max(screen_height // 4, self._row_height + pad*2), max_h)
```

行布局（_redraw 522-575）：
```
row_x1, row_x2 = px(6), w - px(6)      top = px(6)      （玻璃外观）
icon_size = max(px(14), px(size*1.8))  row_height = max(px(20), px(size*2.6))
icon_x = px(10)   gap = px(10)   name_x = icon_x + icon_size + gap
name_avail = row_x2 - name_x - px(12)
```

占位图标：`create_rectangle` 直角，填充
`#4E7A57`(目录) / `#3A3D44`(panel_dark 或 非玻璃) / `#D8DBE0`(其余)

---

## 7. 展开时序 —— 频闪根因（hover_panel.py:290）

```python
if not self._use_wallpaper:
    if self.winfo_ismapped():                      # 面板正显示着
        key = (panel_w, panel_h, px_, py_, "")
        if photo 且 key 相同:
            self._redraw(); self._start_polling(); return      # ① 直接复用，不重抓
        if not _overlaps(旧矩形, 新矩形):
            self._invalidate()
            self._ensure_glass(...)                # ② 不重叠 → 就地抓（新位置是干净屏幕）
            self.geometry(...); self._redraw(); return         #    不挪屏外，所以不闪
        self._rebuild_offscreen(...)               # ③ 重叠 → 才挪到屏幕外重建
        return
    self._invalidate()                             # ④ 从隐藏态弹出
self._ensure_glass(...)                            #    此时面板不可见，抓到干净背景
self.geometry(...); self.deiconify(); self.lift(); self._redraw()
```

**C++ 原实现缺少分支 ②**：切到相邻标签时无条件走「挪屏外 → 抓 → 挪回」，
于是用户看到面板先消失一帧再出现 —— 就是那一下频闪。

---

## 8. 背景巡检（main.py:501 / hover_panel.py:387）

```python
# 标签栏：700ms 一次，抓「标签栏正下方」那条 (0, px(24), w, px(48))
bd = glass_render.grab((0, h, w, h*2))
sig = _bar_signature(bd)              # 压成 24×3 采样点
if not force and not _sig_changed(self._bar_sig, sig): return
# 面板开着时它正好挡在下面 → 跳过
if self.hover_panel.winfo_ismapped(): return
# 整屏取样贵，最多 8 秒重判一次深/浅
if now - self._last_dark_pick > 8:
    self._last_dark_pick = now
    self._glass_dark = glass_render.pick_global_dark()
```

```python
# 面板：只在前台窗口句柄变化时才重抓（不是每 150ms 无条件重抓）
def _check_background_changed(self):
    if not self._glass_like or self._use_wallpaper: return
    fg = self._get_foreground()
    if not fg or fg == self._fg_hwnd: return
    self._fg_hwnd = fg
    if fg in self._own_hwnds(): return
    self._refresh_backdrop_live()      # → _rebuild_offscreen
```

---

## 9. 自动隐藏（main.py:333）

- tick 间隔 **120ms**（不是 150）
- `show = py <= px(2)`；已露出时 `py <= h + px(4)` 也算
- 面板映射中 或 右键菜单开着 → show = True
- 隐藏时：`hover_panel.hide_panel()` + 清 `tab_bar.hover_index` + redraw
- 滑动 4 帧，每帧 16ms

---

## 10. 卡片（菜单 / 弹窗 / 搜索 / 设置）—— 雾层确实存在

| 部件 | blur | 雾层（深 / 浅） | 源码位置 |
|---|---|---|---|
| 右键菜单 | 26 | 黑 60 / 白 52 | glass_menu.py:143 |
| 玻璃弹窗 | 30 | 黑 40 / 白 34 | glass_dialog.py:211 |
| 搜索窗 | 26 | 黑 58 / 白 46 | search_window.py:553 |
| 设置窗 | 30 | 黑 46 / 白 40 | settings_window.py:156 |

顺序：`build_surface` → 叠雾 → `average_color` → `add_border` → `to_chroma`。
**注意：平均色是在「叠雾之后、描边之前」取的。**
