// hr/config.cpp — 读写 %APPDATA%\hotrainy\config.json（与 Python 版同一份）
#include "hr/shared/config.h"

#include <algorithm>
#include <cmath>

#include "hr/shared/common.h"
// 复用项目里同一份手写 JSON（不引入第三方库）
#include "../third_party/tbg/include/tbg/json.h"

namespace hr {

namespace json = tbg::json;

const int kFontMin = 7;
const int kFontMax = 20;

namespace {

int clampInt(double v, int lo, int hi, int def) {
    if (!(v == v)) return def;  // NaN
    int i = (int)std::lround(v);
    return i < lo ? lo : (i > hi ? hi : i);
}

Shortcut shortcutFromJson(const json::Value& v) {
    Shortcut s;
    s.id = v.getStr("id", "");
    s.name = toWide(v.getStr("name", ""));
    s.targetPath = toWide(v.getStr("targetPath", ""));
    s.iconPath = toWide(v.getStr("iconPath", ""));
    s.args = toWide(v.getStr("args", ""));
    s.createdAt = toWide(v.getStr("createdAt", ""));
    return s;
}

json::Value shortcutToJson(const Shortcut& s, const json::Value* old) {
    json::Value v = (old && old->isObj()) ? *old : json::Value::makeObj();
    v.setStr("id", s.id);
    v.setStr("name", toUtf8(s.name));
    v.setStr("targetPath", toUtf8(s.targetPath));
    v.setStr("iconPath", toUtf8(s.iconPath));
    v.setStr("args", toUtf8(s.args));
    if (!s.createdAt.empty()) v.setStr("createdAt", toUtf8(s.createdAt));
    return v;
}

}  // namespace

Tab* findTab(Config& cfg, int index) {
    if (index < 0 || index >= (int)cfg.tabs.size()) return nullptr;
    return &cfg.tabs[index];
}

bool loadConfig(Config& cfg, std::vector<std::string>& warnings) {
    cfg = Config();

    std::wstring path = configPath();
    std::string text;
    if (!readFileUtf8(path, text)) {
        warnings.push_back("没有找到 config.json，使用默认配置");
        return false;
    }

    json::Value root;
    std::string err;
    if (!json::parse(text, root, err) || !root.isObj()) {
        warnings.push_back("config.json 解析失败：" + err);
        return false;
    }

    cfg.version = (int)root.getNum("version", 1);
    cfg.displayMonitor = (int)root.getNum("displayMonitor", 0);
    cfg.position = root.getStr("position", "top");
    cfg.audioBar = root.getBool("audioBarEnabled", false);
    cfg.hoverMode = root.getBool("hoverMode", true);
    cfg.themeColor = root.getStr("themeColor", "默认");
    cfg.glassMode = root.getBool("glassMode", true);
    cfg.wallpaperMode = root.getBool("wallpaperMode", false);
    cfg.autoHide = root.getBool("autoHide", true);
    cfg.shortcutFontSize = clampInt(root.getNum("shortcutFontSize", 10), kFontMin, kFontMax, 10);
    // 音频条（像素）
    cfg.audioBarW = clampInt(root.getNum("audioBarW", 0), 0, 10000, 0);
    cfg.audioBarH = clampInt(root.getNum("audioBarH", 0), 0, 10000, 0);
    {
        const std::string d = root.getStr("audioBarDir", "up");
        cfg.audioBarDir = (d == "down" || d == "left" || d == "right") ? d : "up";
    }
    cfg.audioBarX = clampInt(root.getNum("audioBarX", -1), -1, 20000, -1);
    cfg.audioBarY = clampInt(root.getNum("audioBarY", -1), -1, 20000, -1);

    cfg.searchEnabled = root.getBool("searchEnabled", true);
    cfg.searchHotkey = root.getStr("searchHotkey", "Ctrl+Alt+Space");
    cfg.searchFollow = root.getBool("searchFollowExplorer", true);
    if (const json::Value* d = root.find("searchDirs")) {
        if (d->isArr()) {
            for (const auto& v : d->arr) {
                if (v.isStr() && !v.str.empty()) cfg.searchDirs.push_back(toWide(v.str));
            }
        }
    }
    cfg.searchW = clampInt(root.getNum("searchW", 560), 380, 2000, 560);
    cfg.searchH = clampInt(root.getNum("searchH", 420), 220, 1400, 420);
    cfg.searchSizeCustom = root.getBool("searchSizeCustom", false);
    cfg.searchX = clampInt(root.getNum("searchX", -1), -1, 20000, -1);
    cfg.searchY = clampInt(root.getNum("searchY", -1), -1, 20000, -1);
    cfg.searchPosCustom = root.getBool("searchPosCustom", false);
    cfg.searchScope = root.getStr("searchScope", "user");
    if (cfg.searchScope != "common" && cfg.searchScope != "user" && cfg.searchScope != "all") {
        cfg.searchScope = "common";
    }
    cfg.searchDialogAuto = root.getBool("searchDialogAuto", true);

    // ── 任务栏透明 ──
    cfg.taskbarTransparent = root.getBool("taskbarTransparent", false);

    // 托盘文字颜色：auto / dark / light。
    // 非法值一律回落到 auto —— 手工编辑配置写错时不该让程序行为异常。
    cfg.taskbarTextColor = toWide(root.getStr("taskbarTextColor", "dark"));
    // 「自动」已删除：旧配置里的 auto 迁移成 dark（默认值）
    if (cfg.taskbarTextColor == L"auto") cfg.taskbarTextColor = L"dark";
    if (cfg.taskbarTextColor != L"dark" && cfg.taskbarTextColor != L"light") {
        cfg.taskbarTextColor = L"dark";
    }
    cfg.taskbarAlpha = (int)root.getNum("taskbarAlpha", 180);
    // 夹取非法值：手工编辑配置时可能写出越界数字
    if (cfg.taskbarAlpha < 100) cfg.taskbarAlpha = 100;
    if (cfg.taskbarAlpha > 245) cfg.taskbarAlpha = 245;

    // ── 岛台 ──
    cfg.islandEnabled = root.getBool("islandEnabled", false);
    cfg.islandW = (int)root.getNum("islandW", 720);
    cfg.islandH = (int)root.getNum("islandH", 520);
    cfg.islandX = (int)root.getNum("islandX", -1);
    cfg.islandY = (int)root.getNum("islandY", -1);
    // 面板不透明度。老配置里没这一项时用默认值，
    // 并且夹到合法区间 —— 手改坏了配置也不至于让面板消失。
    cfg.islandOpacity = root.getNum("islandOpacity", 0.72);
    cfg.panelOpacity = clampInt(root.getNum("panelOpacity", 100), 0, 100, 100);
    if (cfg.islandOpacity < 0.05) cfg.islandOpacity = 0.05;
    if (cfg.islandOpacity > 1.0) cfg.islandOpacity = 1.0;
    cfg.islandCategory = root.getStr("islandCategory", "__all__");

    // 尺寸夹取：太小连搜索框都放不下，太大超出屏幕也没意义。
    // 手工编辑配置写出离谱数字时靠这个兜住。
    if (cfg.islandW < 420) cfg.islandW = 420;
    if (cfg.islandH < 320) cfg.islandH = 320;

    // 「全部」分类的快捷方式（用户从面板右键添加的）
    if (const json::Value* isv = root.find("islandShortcuts")) {
        if (isv->isArr()) {
            for (const auto& sv : isv->arr) {
                cfg.islandShortcuts.push_back(shortcutFromJson(sv));
            }
        }
    }

    if (const json::Value* tabs = root.find("tabs")) {
        if (tabs->isArr()) {
            for (const auto& tv : tabs->arr) {
                Tab t;
                t.id = tv.getStr("id", "");
                t.name = toWide(tv.getStr("name", ""));
                t.order = (int)tv.getNum("order", 0);
                t.wallpaper = toWide(tv.getStr("wallpaper", ""));
                if (const json::Value* ss = tv.find("shortcuts")) {
                    if (ss->isArr()) {
                        for (const auto& sv : ss->arr) t.shortcuts.push_back(shortcutFromJson(sv));
                    }
                }
                cfg.tabs.push_back(t);
            }
        }
    }
    if (cfg.tabs.empty()) {
        warnings.push_back("配置里没有标签，建三个默认标签");
        Tab a, b, c;
        a.id = "system"; a.name = L"系统"; a.order = 0;
        b.id = "tools"; b.name = L"工具"; b.order = 1;
        c.id = "entertainment"; c.name = L"娱乐"; c.order = 2;
        cfg.tabs.push_back(a);
        cfg.tabs.push_back(b);
        cfg.tabs.push_back(c);
    }
    return true;
}

bool saveConfig(const Config& cfg) {
    // 先读现有文件，保住未知键（Python 版有、C++ 版还没实现的设置项）
    json::Value root = json::Value::makeObj();
    std::string text;
    if (readFileUtf8(configPath(), text)) {
        json::Value old;
        std::string err;
        if (json::parse(text, old, err) && old.isObj()) root = old;
    }

    root.setNum("version", cfg.version);
    root.setNum("displayMonitor", cfg.displayMonitor);
    root.setStr("position", cfg.position);
    root.setBool("audioBarEnabled", cfg.audioBar);
    root.setBool("hoverMode", cfg.hoverMode);
    root.setStr("themeColor", cfg.themeColor);
    root.setBool("glassMode", cfg.glassMode);
    root.setBool("wallpaperMode", cfg.wallpaperMode);
    root.setBool("autoHide", cfg.autoHide);
    root.setNum("shortcutFontSize", cfg.shortcutFontSize);
    root.setNum("audioBarW", cfg.audioBarW);
    root.setNum("audioBarH", cfg.audioBarH);
    root.setStr("audioBarDir", cfg.audioBarDir);
    root.setNum("audioBarX", cfg.audioBarX);
    root.setNum("audioBarY", cfg.audioBarY);
    root.setBool("searchEnabled", cfg.searchEnabled);
    root.setStr("searchHotkey", cfg.searchHotkey);
    root.setBool("searchFollowExplorer", cfg.searchFollow);
    json::Value dirs = json::Value::makeArr();
    for (const auto& d : cfg.searchDirs) dirs.push(json::Value::makeStr(toUtf8(d)));
    root.set("searchDirs", dirs);
    root.setNum("searchW", cfg.searchW);
    root.setNum("searchH", cfg.searchH);
    root.setBool("searchSizeCustom", cfg.searchSizeCustom);
    root.setNum("searchX", cfg.searchX);
    root.setNum("searchY", cfg.searchY);
    root.setBool("searchPosCustom", cfg.searchPosCustom);
    root.setStr("searchScope", cfg.searchScope);
    root.setBool("searchDialogAuto", cfg.searchDialogAuto);

    // 任务栏透明（新键，Python 版不认识也没关系：它保存时会原样带回去）
    root.setBool("taskbarTransparent", cfg.taskbarTransparent);
    root.setNum("taskbarAlpha", (double)cfg.taskbarAlpha);
    root.setStr("taskbarTextColor", toUtf8(cfg.taskbarTextColor));

    // ── 岛台 ──
    root.setBool("islandEnabled", cfg.islandEnabled);
    root.setNum("islandW", (double)cfg.islandW);
    root.setNum("islandH", (double)cfg.islandH);
    root.setNum("islandX", (double)cfg.islandX);
    root.setNum("islandY", (double)cfg.islandY);
    root.setNum("islandOpacity", cfg.islandOpacity);
    root.setNum("panelOpacity", cfg.panelOpacity);
    root.setStr("islandCategory", cfg.islandCategory);

    // 「全部」分类的快捷方式：和 tab 里的一样，尽量沿用旧对象
    {
        const json::Value* oldIs = root.find("islandShortcuts");
        json::Value arr = json::Value::makeArr();
        for (size_t k = 0; k < cfg.islandShortcuts.size(); ++k) {
            const json::Value* oldOne = nullptr;
            if (oldIs && oldIs->isArr() && k < oldIs->arr.size() && oldIs->arr[k].isObj()) {
                oldOne = &oldIs->arr[k];
            }
            arr.push(shortcutToJson(cfg.islandShortcuts[k], oldOne));
        }
        root.set("islandShortcuts", arr);
    }

    // tabs 树尽量沿用旧对象，带上 wallpaper/order/createdAt 等字段
    const json::Value* oldTabs = root.find("tabs");
    json::Value tabs = json::Value::makeArr();
    for (size_t i = 0; i < cfg.tabs.size(); ++i) {
        const Tab& t = cfg.tabs[i];
        json::Value tv = json::Value::makeObj();
        if (oldTabs && oldTabs->isArr() && i < oldTabs->arr.size() && oldTabs->arr[i].isObj()) {
            tv = oldTabs->arr[i];
        }
        tv.setStr("id", t.id);
        tv.setStr("name", toUtf8(t.name));
        tv.setNum("order", t.order);
        tv.setStr("wallpaper", toUtf8(t.wallpaper));

        const json::Value* oldSs = tv.find("shortcuts");
        json::Value ss = json::Value::makeArr();
        for (size_t k = 0; k < t.shortcuts.size(); ++k) {
            const json::Value* oldOne =
                (oldSs && oldSs->isArr() && k < oldSs->arr.size()) ? &oldSs->arr[k] : nullptr;
            ss.push(shortcutToJson(t.shortcuts[k], oldOne));
        }
        tv.set("shortcuts", ss);
        tabs.push(tv);
    }
    root.set("tabs", tabs);

    std::string out = json::dump(root, true);
    bool ok = writeFileAtomic(configPath(), out);
    if (!ok) logf("config: 写入失败");
    return ok;
}

}  // namespace hr
