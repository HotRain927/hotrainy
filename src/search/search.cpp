// Copyright (c) 2026 HotRain927
// Required Notice: Copyright (c) 2026 HotRain927 (https://github.com/HotRain927)
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// 本文件属于 hotrainy：仅限非商业用途，商业使用需另行获得作者授权（见 LICENSE）。

// hr/search.cpp — 玻璃搜索窗 + 本地索引
// 逐块对照 ui/search_window.py 与 core/search_service.py
#include "hr/search/pinyin.h"
#include "hr/search/search.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "hr/shared/common.h"
#include "hr/glass/glass.h"
#include "hr/shared/icons.h"
#include "hr/text/theme.h"

namespace hr {
namespace {

const wchar_t* kClass = L"HotrainySearch";
bool g_classReady = false;
SearchWindow* g_self = nullptr;   // 输入框子类过程要用

const wchar_t* kUI = L"Microsoft YaHei";
const wchar_t* kSym = L"Segoe UI Symbol";

const int kDesignW = 560;
const int kDesignH = 420;
const int kMinW = 380;
const int kMinH = 220;
const int kRadius = 14;
const int kMaxHits = 800;

// ── 索引 ────────────────────────────────────────

const int kMaxEntries = 50000;
const double kScanBudget = 8.0;

struct Entry {
    std::wstring nameLower;
    std::wstring name;
    std::wstring path;
    std::wstring parent;
    int kind = 2;   // 0 目录 / 1 程序 / 2 文件
};

std::vector<Entry> g_entries;
std::mutex g_mtx;
std::atomic<bool> g_scanning{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_ready{false};
std::atomic<long long> g_lastScanEnd{0};
std::thread g_thread;
std::string g_scope = "user";
std::vector<std::wstring> g_extraDirs;
HWND g_notifyWnd = nullptr;
const UINT WM_INDEX_READY = WM_APP + 21;

std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool endsWith(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    return lowerW(s.substr(s.size() - n)) == suffix;
}

bool hiddenName(const std::wstring& n) {
    if (n.empty()) return false;
    if (n[0] == L'.') return true;
    return n.size() > 1 && n[0] == L'~' && n[1] == L'$';
}

const wchar_t* kSkipDirs[] = {
    L"node_modules", L".git", L".svn", L".idea", L".vscode", L"__pycache__", L"appdata",
    L"windows", L"winsxs", L"system32", L"syswow64", L"programdata", L"$recycle.bin",
    L"system volume information", L"temp", L"tmp", L"site-packages", L"dist-info", L"cache",
    L"caches", L".cache", L"venv", L".venv", L"env", L"bin", L"obj", L"debug", L"release"};

bool skipDir(const std::wstring& low) {
    for (const wchar_t* s : kSkipDirs) {
        if (low == s) return true;
    }
    return false;
}

bool isAppExt(const std::wstring& name) {
    static const wchar_t* exts[] = {L".exe", L".lnk", L".bat", L".cmd", L".url", L".msc"};
    for (const wchar_t* e : exts) {
        if (endsWith(name, e)) return true;
    }
    return false;
}

std::wstring envOf(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n ? std::wstring(buf, n) : std::wstring();
}

bool isDirPath(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> fixedDrives() {
    std::vector<std::wstring> out;
    wchar_t buf[256] = {};
    DWORD n = GetLogicalDriveStringsW(256, buf);
    for (DWORD i = 0; i < n;) {
        std::wstring s(buf + i);
        i += (DWORD)s.size() + 1;
        if (s.empty()) continue;
        if (GetDriveTypeW(s.c_str()) == DRIVE_FIXED) out.push_back(s);
    }
    return out;
}

int maxDepth() {
    if (g_scope == "all") return 4;
    if (g_scope == "user") return 6;
    return 6;
}

std::vector<std::pair<std::wstring, bool>> roots() {
    std::vector<std::pair<std::wstring, bool>> out;
    std::wstring home = envOf(L"USERPROFILE");
    std::wstring prog = envOf(L"ProgramData");
    std::wstring appdata = envOf(L"APPDATA");

    // 开始菜单：这里的 .lnk 就是「能启动的程序」
    for (const std::wstring& p : {prog + L"\\Microsoft\\Windows\\Start Menu\\Programs",
                                  appdata + L"\\Microsoft\\Windows\\Start Menu\\Programs"}) {
        if (!p.empty() && isDirPath(p)) out.push_back({p, true});
    }

    if (g_scope == "all") {
        for (const auto& d : fixedDrives()) out.push_back({d, false});
    } else if (g_scope == "user") {
        if (!home.empty() && isDirPath(home)) out.push_back({home, false});
    } else {
        std::wstring pub = envOf(L"PUBLIC");
        for (const std::wstring& p : {home + L"\\Desktop", pub + L"\\Desktop"}) {
            if (!p.empty() && isDirPath(p)) out.push_back({p, false});
        }
        for (const wchar_t* sub : {L"\\Documents", L"\\Downloads", L"\\Pictures", L"\\Videos"}) {
            std::wstring p = home + sub;
            if (!home.empty() && isDirPath(p)) out.push_back({p, false});
        }
    }

    for (const auto& p : g_extraDirs) {
        if (!p.empty() && isDirPath(p)) out.push_back({p, false});
    }

    // 去重
    std::unordered_set<std::wstring> seen;
    std::vector<std::pair<std::wstring, bool>> uniq;
    for (const auto& r : out) {
        std::wstring k = lowerW(r.first);
        while (k.size() > 1 && k.back() == L'\\') k.pop_back();
        if (seen.insert(k).second) uniq.push_back(r);
    }
    // 整盘范围下用户目录已被 C:\ 覆盖
    if (g_scope == "all") {
        std::vector<std::pair<std::wstring, bool>> kept;
        for (const auto& r : uniq) {
            std::wstring k = lowerW(r.first);
            while (k.size() > 1 && k.back() == L'\\') k.pop_back();
            bool inside = false;
            for (const auto& q : uniq) {
                if (q.first == r.first) continue;
                std::wstring qk = lowerW(q.first);
                while (qk.size() > 1 && qk.back() == L'\\') qk.pop_back();
                if (k.size() > qk.size() && k.compare(0, qk.size(), qk) == 0) {
                    inside = true;
                    break;
                }
            }
            if (!inside) kept.push_back(r);
        }
        uniq = kept;
    }
    return uniq;
}

std::wstring displayNameOf(const std::wstring& path) {
    SHFILEINFOW info = {};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME))
        return info.szDisplayName;
    return L"";
}

void addEntry(std::vector<Entry>& out, const std::wstring& name, const std::wstring& path,
              const std::wstring& parent, int kind) {
    Entry e;
    e.nameLower = lowerW(name);
    e.name = name;
    e.path = path;
    e.parent = parent;
    e.kind = kind;
    out.push_back(std::move(e));
}

void addRootEntry(std::vector<Entry>& out, const std::wstring& root) {
    std::wstring p = root;
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    size_t pos = p.find_last_of(L'\\');
    std::wstring name = pos == std::wstring::npos ? p : p.substr(pos + 1);
    std::wstring parent = pos == std::wstring::npos ? L"" : p.substr(0, pos);
    if (name.empty()) {
        // 磁盘根：用「本地磁盘 (C:)」
        wchar_t unit[4] = {p.empty() ? L'C' : p[0], L':', 0, 0};
        name = std::wstring(L"本地磁盘 (") + unit + L")";
    }
    addEntry(out, name, p, parent, 0);
}

void walk(const std::wstring& rootIn, bool linksOnly, std::vector<Entry>& out, uint64_t t0,
          int depthMax) {
    // 根目录可能是「D:」加一个尾反斜杠，直接拼会得到 D: 后面两个反斜杠，
    // 既让界面显示难看，也让 SHCreateItemFromParsingName 找不到文件 -> 图标提取失败
    std::wstring root = rootIn;
    while (root.size() > 1 && root.back() == L'\\') root.pop_back();
    std::vector<std::pair<std::wstring, int>> stack{{root, 0}};
    while (!stack.empty()) {
        if (g_stop.load() || (int)out.size() >= kMaxEntries) return;
        if ((double)(GetTickCount64() - t0) / 1000.0 > kScanBudget) return;

        auto cur = stack.back();
        stack.pop_back();

        std::wstring pattern = cur.first + L"\\*";
        WIN32_FIND_DATAW fd = {};
        HANDLE hf = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                     nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (hiddenName(name)) continue;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring full = cur.first + L"\\" + name;
            if (isDir) {
                std::wstring low = lowerW(name);
                if (skipDir(low)) continue;
                addEntry(out, name, full, cur.first, 0);
                if (cur.second < depthMax) stack.push_back({full, cur.second + 1});
            } else {
                if (linksOnly) {
                    if (!endsWith(name, L".lnk") && !endsWith(name, L".url")) continue;
                    addEntry(out, name, full, cur.first, 1);
                } else {
                    addEntry(out, name, full, cur.first, isAppExt(name) ? 1 : 2);
                }
            }
            if ((int)out.size() >= kMaxEntries) break;
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
}

std::wstring cachePath() { return configDir() + L"\\search_index_cpp.txt"; }

void saveCache() {
    std::string out;
    out.reserve(g_entries.size() * 80);
    out += "v1\n";
    out += std::to_string((long long)time(nullptr)) + "\n";
    std::lock_guard<std::mutex> lk(g_mtx);
    for (const auto& e : g_entries) {
        out += std::to_string(e.kind);
        out += '\t';
        out += toUtf8(e.name);
        out += '\t';
        out += toUtf8(e.path);
        out += '\t';
        out += toUtf8(e.parent);
        out += '\n';
    }
    writeFileAtomic(cachePath(), out);
}

bool loadCache() {
    std::string data;
    if (!readFileUtf8(cachePath(), data)) return false;
    std::vector<Entry> loaded;
    size_t pos = 0;
    int line = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        std::string row = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? data.size() : nl + 1;
        ++line;
        if (line == 1) {
            if (row != "v1") return false;
            continue;
        }
        if (line == 2) continue;
        if (row.empty()) continue;
        std::vector<std::string> f;
        size_t p = 0;
        while (true) {
            size_t t = row.find('\t', p);
            f.push_back(row.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (f.size() < 4) continue;
        Entry e;
        e.kind = std::atoi(f[0].c_str());
        e.name = toWide(f[1]);
        e.path = toWide(f[2]);
        e.parent = toWide(f[3]);
        e.nameLower = lowerW(e.name);
        loaded.push_back(std::move(e));
    }
    if (loaded.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_entries = std::move(loaded);
    }
    g_ready = true;
    g_lastScanEnd = (long long)time(nullptr);
    logf("search: 缓存命中 %d 条", (int)g_entries.size());
    return true;
}

void scanJob() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<Entry> out;
    uint64_t t0 = GetTickCount64();
    int depth = maxDepth();

    for (const auto& r : roots()) {
        if (g_stop.load()) break;
        addRootEntry(out, r.first);
        walk(r.first, r.second, out, t0, depth);
        if ((int)out.size() >= kMaxEntries) break;
    }

    // 中文系统里的显示名别名（下载 / 图片 / 音乐 …）
    if (!g_stop.load()) {
        std::wstring home = envOf(L"USERPROFILE");
        std::vector<std::wstring> cands;
        if (!home.empty()) cands.push_back(home);
        for (const wchar_t* sub : {L"Desktop", L"Documents", L"Downloads", L"Pictures", L"Music",
                                   L"Videos", L"Favorites"}) {
            std::wstring p = home + L"\\" + sub;
            if (isDirPath(p)) cands.push_back(p);
        }
        for (const auto& d : g_extraDirs) cands.push_back(d);

        std::unordered_set<std::wstring> seen;
        for (const auto& p : cands) {
            if (p.empty() || !isDirPath(p)) continue;
            std::wstring k = lowerW(p);
            if (!seen.insert(k).second) continue;
            std::wstring disp = displayNameOf(p);
            std::wstring trim = p;
            while (trim.size() > 1 && trim.back() == L'\\') trim.pop_back();
            size_t pos = trim.find_last_of(L'\\');
            std::wstring base = pos == std::wstring::npos ? trim : trim.substr(pos + 1);
            if (!disp.empty() && lowerW(disp) != lowerW(base)) {
                addEntry(out, disp, trim, pos == std::wstring::npos ? L"" : trim.substr(0, pos), 0);
            }
        }
    }

    // 去重 + 截断（同名同路径只留一条）
    std::unordered_set<std::wstring> seen;
    std::vector<Entry> uniq;
    uniq.reserve(out.size());
    for (auto& e : out) {
        std::wstring key = e.nameLower + L"\x01" + lowerW(e.path);
        if (!seen.insert(key).second) continue;
        uniq.push_back(std::move(e));
        if ((int)uniq.size() >= kMaxEntries) break;
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_entries = std::move(uniq);
    }
    g_lastScanEnd = (long long)time(nullptr);
    g_ready = true;
    logf("search: 建索引完成 %d 条", (int)g_entries.size());
    saveCache();
    if (g_notifyWnd) PostMessageW(g_notifyWnd, WM_INDEX_READY, 0, 0);
}

void refreshIndex(HWND notify) {
    g_notifyWnd = notify;
    if (g_scanning.exchange(true)) return;
    g_stop = false;
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread([] {
        scanJob();
        g_scanning = false;
    });
}

void stopIndex() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    g_scanning = false;
}

// ── 打分（对照 search_service.py 的 _score / _initials / _subseq） ──

std::wstring initialsOf(const std::wstring& name) {
    std::wstring out;
    bool prevSep = true, prevLower = false;
    const std::wstring seps = L" _-.()[]{}·,，、";
    for (wchar_t ch : name) {
        if (seps.find(ch) != std::wstring::npos) {
            prevSep = true;
            prevLower = false;
            continue;
        }
        if (prevSep || (iswupper(ch) && prevLower)) out.push_back((wchar_t)towlower(ch));
        prevSep = false;
        prevLower = iswlower(ch) || iswdigit(ch);
    }
    return out;
}

bool isSubseq(const std::wstring& q, const std::wstring& s) {
    size_t pos = 0;
    for (wchar_t ch : q) {
        size_t j = s.find(ch, pos);
        if (j == std::wstring::npos) return false;
        pos = j + 1;
    }
    return true;
}

// ── 拼音搜索支持 ───────────────────────────────────────────────
//
// 用户："搜索中文的文件的时候可以用拼音搜，可以全拼也可以拼音首字母"。
// 「工作报表.xlsx」应当能被 gongzuobaobiao、gzb、zuobao 都搜到。
//
// ⚠ 为什么要有缓存：runQuery() 会对索引里的**每一条**逐条调 scoreOf()
//   （打分就是过滤，没有额外的预筛）。每敲一个键就重算几万条名字的拼音会明显卡，
//   而名字是不变的 —— 所以按名字缓存一份。
//   ⚠ 按**值**返回，不要把 map 里的引用交给调用方：
//     两个窗口（岛台内嵌搜索 / 独立搜索窗）可能同时查询，
//     map 被另一个线程 clear 掉时引用就悬空了。
std::mutex g_pyMtx;
std::unordered_map<std::wstring, PyForms> g_pyCache;

PyForms pyFormsCached(const std::wstring& name) {
    {
        std::lock_guard<std::mutex> lk(g_pyMtx);
        auto it = g_pyCache.find(name);
        if (it != g_pyCache.end()) return it->second;
    }
    PyForms f;
    pinyinForms(name, f);
    std::lock_guard<std::mutex> lk(g_pyMtx);
    if (g_pyCache.size() > 12000) g_pyCache.clear();   // 兜底：别无限长（原 80000 太占内存）
    g_pyCache.emplace(name, f);
    return f;
}

bool hasHanChar(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c >= 0x4E00 && c <= 0x9FFF) return true;
    }
    return false;
}

// 查询串 → 纯 ASCII 小写；含任何非 ASCII 字符就返回空串（调用方据此跳过拼音匹配）
std::string asciiQuery(const std::wstring& q) {
    std::string s;
    s.reserve(q.size());
    for (wchar_t c : q) {
        if (c < 0x20 || c > 0x7E) return std::string();
        s.push_back((char)c);
    }
    return s;
}

bool isSubseqAscii(const std::string& q, const std::string& s) {
    size_t pos = 0;
    for (char ch : q) {
        const size_t j = s.find(ch, pos);
        if (j == std::string::npos) return false;
        pos = j + 1;
    }
    return true;
}

// 拼音打分：全拼相等 > 全拼前缀 > 全拼子串 > 首字母子序列。
// 分数压在字面匹配（700+）之下、模糊匹配（480）之上 —— 拼音是"二等公民"，
// 真的字面命中永远排在前面。
int scorePinyin(const std::string& qa, const PyForms& py) {
    if (qa.empty() || py.full.empty()) return 0;
    if (py.full == qa) return 880;
    if (py.full.size() >= qa.size() && py.full.compare(0, qa.size(), qa) == 0) {
        return 860 - (int)std::min<size_t>(py.full.size() - qa.size(), 60);
    }
    const size_t i = py.full.find(qa);
    if (i != std::string::npos) return 660 - (int)i * 4;
    // 多音字次读音：银(行) → yinhang（主读音是 xing 时靠它兜底）
    if (!py.fullAlt.empty()) {
        if (py.fullAlt.size() >= qa.size() && py.fullAlt.compare(0, qa.size(), qa) == 0) {
            return 840 - (int)std::min<size_t>(py.fullAlt.size() - qa.size(), 60);
        }
        if (py.fullAlt.find(qa) != std::string::npos) return 650;
    }
    // 首字母子序列：gzb → gzbb（和英文名那套 isSubseq 同一个思路）
    if (py.initials.size() >= 2 && isSubseqAscii(qa, py.initials)) {
        return std::max(300, 600 - (int)(py.initials.size() - qa.size()) * 10);
    }
    return 0;
}

int scoreOf(const std::wstring& nameLower, const std::wstring& q, const std::wstring& name,
            const std::string& qa, bool fuzzy) {
    if (nameLower == q) return 1000;
    if (nameLower.compare(0, q.size(), q) == 0 && nameLower.size() >= q.size()) {
        return 900 - (int)std::min<size_t>(nameLower.size() - q.size(), 60);
    }
    size_t i = nameLower.find(q);
    if (i != std::wstring::npos) {
        return 700 - (int)i * 5 - (int)std::min<size_t>(nameLower.size() / 3, 40);
    }
    if (!name.empty()) {
        std::wstring ini = initialsOf(name);
        if (ini.size() >= 2 && isSubseq(q, ini)) {
            return std::max(300, 560 - (int)(ini.size() - q.size()) * 10);
        }
    }
    // ★ 拼音：中文名可以用全拼或首字母搜到（用户明确要求）。
    //   必须放在 fuzzy 判定之前 —— 首字母查询（gzb）不该受"至少 3 个字符才模糊"的限制。
    if (hasHanChar(name)) {
        const int ps = scorePinyin(qa, pyFormsCached(name));
        if (ps > 0) return ps;
    }
    if (!fuzzy) return 0;
    size_t pos = 0;
    int first = -1, gaps = 0;
    for (wchar_t ch : q) {
        size_t j = nameLower.find(ch, pos);
        if (j == std::wstring::npos) return 0;
        if (first < 0) first = (int)j;
        gaps += (int)j - (int)pos;
        pos = j + 1;
    }
    if (gaps > (int)q.size() * 2 + 3) return 0;
    if ((int)pos - first > (int)q.size() * 1.5 + 3) return 0;
    return std::max(120, 480 - gaps * 6 - (int)std::min<size_t>(nameLower.size() / 4, 60));
}

int cmpLogical(const std::wstring& a, const std::wstring& b) {
    int r = StrCmpLogicalW(a.c_str(), b.c_str());
    return r;
}

int kindRank(int kind) {
    if (kind == 0) return 0;
    if (kind == 1) return 1;
    return 2;
}

// ── 文件对话框检测（对照 search_window.py 的 _is_file_dialog / file_dialog_bounds /
//    _dialog_edit / fill_file_dialog） ──

bool hasChildClass(HWND hwnd, const wchar_t* clsName) {
    struct Ctx {
        const wchar_t* name;
        bool* found;
    } ctx{clsName, nullptr};
    bool found = false;
    ctx.found = &found;
    EnumChildWindows(
        hwnd,
        [](HWND h, LPARAM lp) -> BOOL {
            Ctx* c = (Ctx*)lp;
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            if (_wcsicmp(cls, c->name) == 0) {
                *c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&ctx);
    return found;
}

bool dialogVisibleRect(HWND hwnd, RECT& out) {
    if (!IsWindow(hwnd) || IsIconic(hwnd) || !IsWindowVisible(hwnd)) return false;
    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        if (!GetWindowRect(hwnd, &r)) return false;
    }
    if (r.right - r.left < 200 || r.bottom - r.top < 120) return false;
    out = r;
    return true;
}

bool isFileDialog(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"#32770") != 0) return false;
    if (!IsWindowVisible(hwnd)) return false;
    RECT r;
    if (!dialogVisibleRect(hwnd, r)) return false;
    if (hasChildClass(hwnd, L"SHELLDLL_DefView")) return true;
    return hasChildClass(hwnd, L"DirectUIHWND") && hasChildClass(hwnd, L"Edit");
}

HWND dialogEdit(HWND hwnd) {
    if (!hwnd) return nullptr;
    struct Ctx {
        std::vector<HWND> edits;
    } ctx;
    EnumChildWindows(
        hwnd,
        [](HWND h, LPARAM lp) -> BOOL {
            Ctx* c = (Ctx*)lp;
            wchar_t cls[32] = {};
            GetClassNameW(h, cls, 32);
            if (_wcsicmp(cls, L"Edit") == 0 && IsWindowVisible(h)) c->edits.push_back(h);
            return TRUE;
        },
        (LPARAM)&ctx);
    if (ctx.edits.empty()) return nullptr;
    HWND best = nullptr;
    int bestScore = -1;
    for (HWND e : ctx.edits) {
        int s = 0;
        HWND p = e;
        for (int i = 0; i < 6; ++i) {
            p = GetParent(p);
            if (!p) break;
            wchar_t cls[32] = {};
            GetClassNameW(p, cls, 32);
            if (_wcsicmp(cls, L"ComboBoxEx32") == 0) {
                s = 10;
                break;
            }
        }
        if (s > bestScore) {
            bestScore = s;
            best = e;
        }
    }
    return best;
}

bool fillFileDialog(HWND hwnd, const std::wstring& path) {
    if (!hwnd || path.empty()) return false;
    HWND edit = dialogEdit(hwnd);
    if (!edit) return false;
    SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)path.c_str());
    PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(1, 0), 0);  // IDOK = 1
    return true;
}

}  // namespace

// ── 共用查询：把关键词跑一遍索引，返回排好序的命中项 ────────────────
//
// ⚠ 这段是从 SearchWindow::runSearch 里**原样抽出来**的，不是重写的。
//
//   为什么要抽：岛台面板现在要在**自己面板里**直接显示结果列表
//   （用户要求"把搜索框复制一份固定在岛台上"），而搜索窗也要显示结果。
//   两边必须共用同一套匹配与排序 —— 否则同一个词在岛台和搜索窗里
//   结果顺序不一样，用户会以为其中一个坏了。
//
//   纯函数：不碰搜索窗的任何成员状态，也不动索引（只读 g_entries）。
std::vector<SearchHit> runQuery(const std::wstring& q, int maxHits) {
    std::vector<SearchHit> out;
    if (q.empty()) return out;

    std::wstring ql = lowerW(q);
    bool pathMode = (q.find(L'\\') != std::wstring::npos || q.find(L'/') != std::wstring::npos);
    std::wstring qn = pathMode ? ql : L"";
    if (pathMode) {
        for (auto& c : qn) {
            if (c == L'/') c = L'\\';
        }
    }
    bool extMode = ql.size() > 1 && ql[0] == L'.';
    bool fuzzy = ql.size() >= 3 && !extMode;
    const std::string qa = asciiQuery(ql);   // 拼音匹配用（非 ASCII 查询会得到空串）

    struct Scored {
        int s;
        // ⚠⚠ 这里必须存**值**，不能存指向 g_entries 的指针。
        //
        //   踩过的坑（代码审计发现，属必崩点）：
        //     原来写的是 `const Entry* e;`，指向 g_entries 里的元素。
        //     而下面的锁**在 1183 行就释放了**，
        //     之后 1192-1215 还要拿这些指针去排序比较、取字段。
        //     与此同时后台扫描线程 scanJob 会整体替换 g_entries ——
        //     一旦撞上，就是 use-after-free，直接崩。
        //
        //     存值只多拷几个字符串（命中数上限是 maxHits，很小），
        //     换来的是彻底不依赖锁的生命周期。
        std::wstring nameLower;
        std::wstring name;
        std::wstring path;
        std::wstring parent;
        int kind;
    };
    std::vector<Scored> scored;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        scored.reserve(g_entries.size() / 8 + 16);
        for (const auto& e : g_entries) {
            int s = 0;
            if (extMode) {
                s = endsWith(e.nameLower, ql.c_str()) ? 640 : 0;
            } else {
                s = scoreOf(e.nameLower, ql, e.name, qa, fuzzy);
            }
            if (pathMode) {
                std::wstring hay = lowerW(e.parent + L"\\" + e.name);
                size_t i = hay.find(qn);
                if (i != std::wstring::npos) {
                    std::wstring a = hay, b = qn;
                    while (!a.empty() && a.back() == L'\\') a.pop_back();
                    while (!b.empty() && b.back() == L'\\') b.pop_back();
                    if (a == b) {
                        s = 990;
                    } else {
                        s = std::max(s, 560 - (int)std::min<size_t>(i, 80));
                    }
                }
            }
            if (s > 0) scored.push_back({s, e.nameLower, e.name, e.path, e.parent, e.kind});
        }
    }
    if (scored.empty()) return out;

    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.s != b.s) return a.s > b.s;
        return a.nameLower < b.nameLower;
    });
    if ((int)scored.size() > maxHits) scored.resize(maxHits);

    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        int r = cmpLogical(a.name, b.name);
        if (r) return r < 0;
        int ka = kindRank(a.kind), kb = kindRank(b.kind);
        if (ka != kb) return ka < kb;
        return cmpLogical(a.path, b.path) < 0;
    });

    out.clear();
    out.reserve(scored.size());
    for (const auto& s : scored) {
        SearchHit h;
        h.name = s.name;
        h.path = s.path;
        h.parent = s.parent;
        h.kind = s.kind;
        out.push_back(std::move(h));
    }

    if (!pathMode && !extMode) {
        std::stable_sort(out.begin(), out.end(), [&ql](const SearchHit& a, const SearchHit& b) {
            std::wstring al = lowerW(a.name), bl = lowerW(b.name);
            bool ap = al.compare(0, ql.size(), ql) == 0 && al.size() >= ql.size();
            bool bp = bl.compare(0, ql.size(), ql) == 0 && bl.size() >= ql.size();
            return ap && !bp;
        });
    }


    return out;
}


bool fileDialogBounds(RECT& out, HWND& hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg && isFileDialog(fg)) {
        if (dialogVisibleRect(fg, out)) {
            hwnd = fg;
            return true;
        }
    }
    // 不只看前台：用户点过搜索框后前台就变了，但对话框还开着，按 z 序找最上面那个
    HWND h = GetTopWindow(nullptr);
    int guard = 0;
    while (h && guard < 2000) {
        guard++;
        if (h != fg && isFileDialog(h)) {
            if (dialogVisibleRect(h, out)) {
                hwnd = h;
                return true;
            }
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }
    return false;
}

// ── 窗口实现 ────────────────────────────────────

bool SearchWindow::create(HINSTANCE hInst, Config* cfg, bool dark) {
    hinst_ = hInst;
    cfg_ = cfg;
    dark_ = dark;

    if (!g_classReady) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SearchWindow::proc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
        g_classReady = true;
    }

    if (hwnd_) return true;

    customSize_ = cfg_->searchSizeCustom;
    posCustom_ = cfg_->searchPosCustom && cfg_->searchX >= 0 && cfg_->searchY >= 0;
    w_ = px(std::max<double>(kMinW, cfg_->searchW));
    h_ = px(std::max<double>(kMinH, cfg_->searchH));

    // ⚠⚠ 必须带 WS_THICKFRAME（照抄岛台那条注释）：
    //   这是"窗口可拖边框改大小"的标志。没有它的话，即使 WM_NCHITTEST 正确
    //   返回了 HTBOTTOMRIGHT，系统也会直接忽略 —— 根本不进拖动循环，
    //   WM_SIZING / WM_EXITSIZEMOVE 一个都不会来。
    //   分层窗口（WS_EX_LAYERED + UpdateLayeredWindow）下那圈非客户区边框
    //   不渲染，所以外观不受影响。
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"搜索",
                            WS_POPUP | WS_THICKFRAME, 0, 0, w_, h_, nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        logf("search: 创建窗口失败 %lu", (unsigned long)GetLastError());
        return false;
    }
    g_self = this;

    // 输入框必须是「独立弹窗」而不是 WS_CHILD：
    // 主窗用 UpdateLayeredWindow 上屏，而这种窗口不会渲染子控件（文字整块消失，
    // 但键盘照样能输入，特别容易看漏）。做成 owner 为主窗的 popup 就正常了。
    // ★ 要 WS_EX_LAYERED：配合 WM_CTLCOLOREDIT 里的颜色键把输入框底色键成透明
    //   （EDIT 不支持逐像素透明，和岛台搜索框同一个做法）。
    edit_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", L"",
                            WS_POPUP | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, hwnd_, nullptr,
                            hInst, nullptr);
    if (edit_) {
        HFONT f = CreateFontW(-pt(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, kUI);
        SendMessageW(edit_, WM_SETFONT, (WPARAM)f, TRUE);
        editOldProc_ = (WNDPROC)SetWindowLongPtrW(edit_, GWLP_WNDPROC, (LONG_PTR)searchEditProc);
    }

    // 索引：先读缓存（秒开），再后台重建
    g_scope = cfg_->searchScope;
    g_extraDirs = cfg_->searchDirs;
    loadCache();
    refreshIndex(hwnd_);
    return true;
}

void SearchWindow::destroy() {
    stopIndex();
    if (hwnd_) DestroyWindow(hwnd_);
    g_self = nullptr;
}

void SearchWindow::reloadIndex() {
    if (!cfg_) return;
    g_scope = cfg_->searchScope;
    g_extraDirs = cfg_->searchDirs;
    g_ready = false;
    refreshIndex(hwnd_);
}

bool SearchWindow::indexReady() const { return g_ready.load(); }
int SearchWindow::indexCount() const { return (int)g_entries.size(); }

// ── 位置 / 尺寸 ─────────────────────────────────

void SearchWindow::fitBelow(int t, int b, int sh, int& y, int& h) const {
    int gap = px(6), margin = px(8), minH = px(kMinH);
    int availBelow = sh - margin - (b + gap);
    if (availBelow >= minH) {
        y = b + gap;
        h = std::min(h_, availBelow);
        return;
    }
    int availAbove = t - gap - margin;
    if (availAbove >= minH) {
        h = std::min(h_, availAbove);
        y = t - gap - h;
        return;
    }
    h = std::max(minH, std::min(h_, sh - margin * 2));
    y = std::max(0, sh - h - margin);
}

bool SearchWindow::targetBounds(RECT& out, HWND& hwnd, bool& isDialog) const {
    HWND fg = GetForegroundWindow();
    if (fg && fg != hwnd_) {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, 64);
        bool explorer =
            (wcscmp(cls, L"CabinetWClass") == 0 || wcscmp(cls, L"ExploreWClass") == 0);
        if (explorer) {
            RECT rc = {};
            if (GetWindowRect(fg, &rc)) {
                RECT ext = {};
                if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &ext,
                                                     sizeof(ext)))) {
                    rc = ext;
                }
                if (rc.right - rc.left >= px(200) && rc.bottom - rc.top >= px(100)) {
                    out = rc;
                    hwnd = fg;
                    isDialog = false;
                    return true;
                }
            }
        }
    }
    // 文件对话框：前台或按 z 序（对照 target_bounds -> file_dialog_bounds）
    RECT rc = {};
    HWND dlg = nullptr;
    if (fileDialogBounds(rc, dlg)) {
        out = rc;
        hwnd = dlg;
        isDialog = true;
        return true;
    }
    return false;
}

// 客户区坐标 → 内容坐标。
//
// ★ 和"窗口向外让出外阴影留白"是**配套**的：窗口比内容大了一圈，
//   客户区坐标就比内容坐标多出那一圈 —— 不换算的话所有悬停/点击
//   （结果行、滚动条、关闭按钮、右下角缩放）都会整体偏掉。
//   对照 IslandWindow::contentPt()，它踩过同样的坑。
static POINT searchContentPt(HWND h, LPARAM l) {
    POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    RECT wr{}, cr{};
    if (::GetWindowRect(h, &wr) && ::GetClientRect(h, &cr)) {
        POINT c0 = {0, 0};
        ::ClientToScreen(h, &c0);
        p.x += (c0.x - wr.left);
        p.y += (c0.y - wr.top);
    }
    // ⚠ 这里必须与 SearchWindow::shadowInset() 保持一致
    p.x -= px(14);
    p.y -= px(14);
    return p;
}

void SearchWindow::place(bool autoOnly) {
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int sw = work.right - work.left;
    int sh = work.bottom - work.top;

    // 用户手动挪过 → 就摆在他放的地方
    if (posCustom_ && !autoOnly) {
        w_ = std::max(px(kMinW), std::min(w_, sw));
        h_ = std::max(px(kMinH), std::min(h_, sh));
        x_ = work.left + std::max(0, std::min(px(cfg_->searchX), sw - w_));
        y_ = work.top + std::max(0, std::min(px(cfg_->searchY), sh - h_));
        attached_ = nullptr;
        return;
    }

    RECT tb = {};
    HWND th = nullptr;
    bool isDialog = false;
    if (cfg_ && cfg_->searchFollow && targetBounds(tb, th, isDialog)) {
        int l = tb.left, t = tb.top, r = tb.right, b = tb.bottom;
        int w = customSize_ ? std::max(px(kMinW), std::min(w_, sw)) : std::max(px(kMinW), r - l);
        int ny = 0, nh = 0;
        fitBelow(t - work.top, b - work.top, sh, ny, nh);
        // ★ 用户手动调过大小（customSize_）就**保留他的高度**。
        //   原来这里无条件采用 fitBelow 按目标窗口算出的高度 —— 于是
        //   "跟随资源管理器/文件对话框时调好的大小，下次打开又被改回去"
        //   （用户反馈）。宽度上面早就有 customSize_ 分支了，高度漏了。
        if (customSize_) nh = std::max(px(kMinH), std::min(h_, sh));
        w_ = w;
        h_ = nh;
        x_ = l;
        y_ = work.top + ny;
        if (x_ + w_ > work.right) x_ = std::max(work.left, work.right - w_);
        attached_ = th;
        attachedDialog_ = isDialog;
        return;
    }

    w_ = std::max(px(kMinW), std::min(w_, sw));
    h_ = std::max(px(kMinH), std::min(h_, sh));
    x_ = work.left + std::max(0, (sw - w_) / 2);
    y_ = work.top + std::max(0, (int)(sh * 0.18));
    attached_ = nullptr;
    attachedDialog_ = false;
}

// ── 玻璃 ────────────────────────────────────────

// 设置里的面板透明度 / 外观变了 → 重烘焙雾化底并重出帧。
// （和标签栏、标签面板、岛台同一个入口名，方便在 applySetting 里统一通知。）
void SearchWindow::refreshAppearance() {
    if (!hwnd_) return;
    buildGlass();
    if (visible_) redraw();
}

int SearchWindow::shadowInset() const {
    return px(14);
}

void SearchWindow::buildGlass() {
    glass_.create(w_, h_);
    const int r = px(kRadius);

    // ★ 与标签面板 / 岛台 / 设置窗统一：**纯色雾 + 玻璃边**（buildFlatPanel）。
    //
    //   不再抓屏、不再 buildCard —— 那是早期"抓屏做玻璃"的遗留管线：
    //   它要求先把窗口藏起来（rebuildGlassOffscreen 里挪到 -20000 再挪回来），
    //   一进一出就是用户看到的闪烁，而且窗口一变尺寸就得重抓。
    // 与岛台 rebuildBase() 完全一致：兜底 0 = 纯透明（边框始终保留）
    const int op = cfg_ ? cfg_->panelOpacity : 0;
    glass_.buildFlatPanel(op, r, dark_);
    // ★ 这一步是"边框/雾化看起来和岛台一样"的关键：岛台有 cutRoundCorners，
    //   搜索窗原来漏了 —— 四角的雾和边框因此对不上。
    glass_.cutRoundCorners(r);
    avg_ = glass_.average();
    hasGlass_ = true;

    // 外阴影只跟尺寸/深浅有关，跟透明度无关 ——
    // 拖透明度滑块时不要重算它（那是一整张逐像素距离场，白算会让拖动发涩）。
    const int s = shadowInset();
    if (shadow_.width() != w_ + s * 2 || shadow_.height() != h_ + s * 2) {
        shadow_.create(w_ + s * 2, h_ + s * 2);
        shadow_.clear();
        RECT content = {s, s, s + w_, s + h_};
        shadow_.applyOuterShadow(content, r, dark_, px(2));
    }
}

void SearchWindow::rebuildGlassOffscreen() {
    if (!hwnd_) return;
    // ★ 不再"挪到 -20000 再挪回来"：现在不抓屏了，重烘焙只跟尺寸/主题有关，
    //   没有"把自己拍进去"的问题，也就没有那一进一出的闪烁。
    buildGlass();
    redraw();
}

void SearchWindow::pushFrame() {
    HDC screen = GetDC(nullptr);
    if (!screen) return;

    // ★ 抬命中地板：分层窗口的命中测试**只看 alpha**，雾/边框之外的地方 alpha 很低，
    //   不抬的话窗口边缘点不到（和设置窗、岛台同一个理由）。
    RECT contentRc = {0, 0, w_, h_};
    frame_.ensureHitTestableArea(contentRc, px(kRadius),
                                 hitTestAlphaFor(cfg_ ? cfg_->panelOpacity : 100));

    // ★ 内容 + 外阴影合成到"带留白"的画布再整幅提交；
    //   落点向左上各让出留白（dst = 内容位置 - 留白），内容坐标一像素不动。
    const int s = shadowInset();
    outer_ = shadow_;
    outer_.blendOver(frame_, s, s);

    HBITMAP bmp = outer_.createPremultipliedBitmap(screen);
    if (bmp) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT dst = {x_ - s, y_ - s};
        SIZE sz = {outer_.width(), outer_.height()};
        POINT src = {0, 0};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bmp);
    }
    ReleaseDC(nullptr, screen);
}

uint32_t SearchWindow::bandAvg(int y1, int y2) const {
    if (glass_.width() <= 0 || glass_.height() <= 0) return avg_;
    const auto& pxv = glass_.pixels();
    int H = glass_.height(), W = glass_.width();
    y1 = std::max(0, std::min(H - 1, y1));
    y2 = std::max(y1, std::min(H - 1, y2));
    uint64_t r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = y1; y <= y2; y += 2) {
        for (int x = 0; x < W; x += 7) {
            uint32_t c = pxv[(size_t)y * W + x];
            r += rOf(c);
            g += gOf(c);
            b += bOf(c);
            ++n;
        }
    }
    if (!n) return avg_;
    return rgb((uint8_t)(r / n), (uint8_t)(g / n), (uint8_t)(b / n));
}

uint32_t SearchWindow::tint(int y1, int y2, double amount, bool towardWhite) const {
    if (!dark_) {
        towardWhite = false;
        amount *= 0.45;
    }
    uint32_t base = bandAvg(y1, y2);
    return blendColor(base, towardWhite ? 0xFFFFFFFFu : 0xFF000000u, amount) | 0xFF000000u;
}

// ── 绘制 ────────────────────────────────────────

void SearchWindow::drawHeader() {
    uint32_t text = 0, dim = 0;
    readableText(avg_, dark_, text, dim);
    double lum = (0.2126 * rOf(avg_) + 0.7152 * gOf(avg_) + 0.0722 * bOf(avg_)) / 255.0;
    if (lum >= 0.28 && lum <= 0.62) dim = (lum < 0.45) ? rgb(216, 218, 224) : rgb(51, 53, 58);

    uint32_t hair = (blendColor(avg_, dark_ ? 0xFFFFFFFFu : 0xFF000000u, dark_ ? 0.16 : 0.12)) |
                    0xFF000000u;
    // 放大镜：圆圈 + 手柄（也是拖动手柄）
    int cx = px(26), cy = px(24), r = px(7);
    bool dragHot = (GetAsyncKeyState(VK_LBUTTON) == 0);
    uint32_t col = dim;
    if (dark_) col = (GetCapture() == hwnd_) ? text : dim;
    else col = (GetCapture() == hwnd_) ? text : dim;
    (void)dragHot;
    frame_.strokeCircle(cx, cy, r, col, (float)std::max(1, px(1.4)));
    frame_.drawLine(cx + (int)(r * 0.7), cy + (int)(r * 0.7), cx + (int)(r * 1.9),
                    cy + (int)(r * 1.9), col, (float)std::max(1, px(1.6)));

    // 输入区：与岛台的搜索框**完全一致** —— 不铺底，只描一圈"玻璃边"。
    //   （原来这里铺了一块不透明实色胶囊，看着就是一块平的方框。）
    frame_.strokeGlassEdge(px(44), px(9), w_ - px(12) - px(44), px(39) - px(9), px(9), dark_);

    frame_.fillRect(px(12), px(48), w_ - px(24), 1, hair);
}

void SearchWindow::drawShapeIcon(const SearchHit& hit, int x, int y, int size, uint32_t tile) {
    int half = size / 2;
    uint32_t text = 0, dim = 0;
    readableText(avg_, dark_, text, dim);
    if (hit.kind == 0) {
        uint32_t tab = blendColor(tile, 0xFFFFFFFFu, 0.22) | 0xFF000000u;
        frame_.fillRoundRect(x, y - half - px(2), (int)(size * 0.52), px(7) + px(2), px(2), tab);
        frame_.fillRoundRect(x, y - half + px(3), size, size - px(3), px(3), tile);
    } else {
        frame_.fillRoundRect(x, y - half, size, size, px(4), tile);
        std::wstring ch = hit.name.empty() ? L"?" : hit.name.substr(0, 1);
        for (auto& c : ch) c = (wchar_t)towupper(c);
        RECT rc = {x, y - half, x + size, y + half};
        frame_.drawText(ch.c_str(), rc, (float)pt(10), text, 1, false, kSym);
    }
}

void SearchWindow::drawResults() {
    uint32_t text = 0, dim = 0;
    readableText(avg_, dark_, text, dim);
    double lum = (0.2126 * rOf(avg_) + 0.7152 * gOf(avg_) + 0.0722 * bOf(avg_)) / 255.0;
    if (lum >= 0.28 && lum <= 0.62) dim = (lum < 0.45) ? rgb(216, 218, 224) : rgb(51, 53, 58);
    uint32_t accent = dark_ ? rgb(138, 180, 255) : rgb(47, 107, 216);

    top_ = px(52);
    rowH_ = px(46);
    int avail = h_ - top_ - px(32);
    int rows = std::max(1, avail / rowH_);
    rowsVis_ = rows;

    int n = (int)hits_.size();
    int maxScroll = std::max(0, n - rows);
    scroll_ = std::max(0, std::min(scroll_, maxScroll));

    if (hits_.empty()) {
        std::wstring msg = hint_;
        if (msg.empty()) msg = query_.empty() ? L"输入关键字搜索应用和文件" : L"没有匹配的结果";
        RECT rc = {0, top_ + avail / 2 - px(10), w_, top_ + avail / 2 + px(10)};
        frame_.drawText(msg.c_str(), rc, (float)pt(8), dim, 1, false, kUI);
        return;
    }

    int start = scroll_;
    int end = std::min(n, start + rows);
    for (int i = 0; i < end - start; ++i) {
        int idx = start + i;
        const SearchHit& hit = hits_[(size_t)idx];
        int y1 = top_ + i * rowH_;
        int y2 = y1 + rowH_;
        bool sel = (idx == sel_);
        if (sel) {
            frame_.fillRoundRect(px(8), y1 + px(2), w_ - px(16), rowH_ - px(4), px(8),
                                 tint(y1, y2, 0.20, true));
            frame_.fillRoundRect(px(8), y1 + px(13), px(3), rowH_ - px(26), px(1), accent);
        } else if (idx == hotRow_) {
            frame_.fillRoundRect(px(8), y1 + px(2), w_ - px(16), rowH_ - px(4), px(8),
                                 tint(y1, y2, 0.12, true));
        }

        int isz = px(20);
        int ix = px(18);
        int iy = (y1 + y2) / 2;
        HBITMAP bmp = iconFor(hit.path, isz);
        if (bmp) {
            frame_.drawBitmap(bmp, ix, iy - isz / 2, isz);
        } else {
            drawShapeIcon(hit, ix, iy, isz, tint(y1, y2, 0.32, true));
        }

        int tx = ix + isz + px(11);
        int right = w_ - px(14) - (n > rows ? px(10) : 0);

        std::wstring name = Surface::fitText(hit.name.c_str(), (float)pt(11), right - tx, kUI);
        int hh = px(9);
        RECT rn = {tx, iy - px(9) - hh, right, iy - px(9) + hh};
        frame_.drawText(name.c_str(), rn, (float)pt(11), text, 0, false, kUI);

        std::wstring parent = hit.parent;
        if (parent.size() > 58) {
            std::wstring tail = parent.substr(parent.size() - 56);
            size_t cut = tail.find(L'\\');
            if (cut != std::wstring::npos && cut < 14) tail = tail.substr(cut);
            parent = L"…" + tail;
        }
        std::wstring pathText =
            Surface::fitText(parent.c_str(), (float)pt(8), right - tx, kUI);
        RECT rp = {tx, iy + px(10) - hh, right, iy + px(10) + hh};
        frame_.drawText(pathText.c_str(), rp, (float)pt(8), dim, 0, false, kUI);
    }

    if (n > rows) drawScrollbar(top_, avail, rows, n);
}

void SearchWindow::drawScrollbar(int top, int avail, int rows, int n) {
    int x1 = w_ - px(16);
    int x2 = w_ - px(11);
    int y0 = top + px(6);
    int y1 = top + avail - px(6);
    int trackH = std::max(px(10), y1 - y0);
    int barH = std::max(px(28), (int)((double)trackH * rows / std::max(1, n)));
    if (barH > trackH) barH = trackH;
    int travel = trackH - barH;
    int maxScroll = std::max(1, n - rows);
    int off = (int)((double)travel * scroll_ / maxScroll);

    uint32_t hair = (blendColor(avg_, dark_ ? 0xFFFFFFFFu : 0xFF000000u, dark_ ? 0.16 : 0.12)) |
                    0xFF000000u;
    uint32_t accent = dark_ ? rgb(138, 180, 255) : rgb(47, 107, 216);
    frame_.fillRoundRect(x1, y0, x2 - x1, trackH, px(2), hair);
    frame_.fillRoundRect(x1, y0 + off, x2 - x1, barH, px(2), accent);

    sbTrack_ = trackH;
    sbThumb_ = barH;
    sbTravel_ = travel;
    sbMaxScroll_ = maxScroll;
}

void SearchWindow::drawFooter() {
    uint32_t dim = 0, text = 0;
    readableText(avg_, dark_, text, dim);
    double lum = (0.2126 * rOf(avg_) + 0.7152 * gOf(avg_) + 0.0722 * bOf(avg_)) / 255.0;
    if (lum >= 0.28 && lum <= 0.62) dim = (lum < 0.45) ? rgb(216, 218, 224) : rgb(51, 53, 58);
    uint32_t hair = (blendColor(avg_, dark_ ? 0xFFFFFFFFu : 0xFF000000u, dark_ ? 0.16 : 0.12)) |
                    0xFF000000u;

    int fy = h_ - px(30);
    frame_.fillRect(px(12), fy, w_ - px(24), 1, hair);

    int y = h_ - px(15);
    std::wstring hint = L"↑↓ 选择   Enter 打开   Ctrl+Enter 所在位置   Esc 关闭";
    if ((int)hits_.size() > rowsVis_)
        hint = L"↑↓ 选择   Enter 打开   Ctrl+Enter 所在位置   滚轮翻页   Esc 关闭";
    int avail = w_ - px(28) - (hits_.empty() ? 0 : px(76));
    std::wstring fitted = Surface::fitText(hint.c_str(), (float)pt(8), avail, kUI);
    int hh = px(8);
    RECT rc = {px(14), y - hh, px(14) + avail, y + hh};
    frame_.drawText(fitted.c_str(), rc, (float)pt(8), dim, 0, false, kUI);

    if (!hits_.empty()) {
        std::wstring txt = std::to_wstring(sel_ + 1) + L" / " + std::to_wstring(hits_.size());
        RECT r2 = {w_ - px(14) - px(80), y - hh, w_ - px(14), y + hh};
        frame_.drawText(txt.c_str(), r2, (float)pt(8), dim, 2, false, kUI);
    }
}

void SearchWindow::redraw() {
    if (!hwnd_) return;
    if (frame_.width() != w_ || frame_.height() != h_) frame_.create(w_, h_);
    // ★ 不再有"拖动/缩放中先铺一块实色"的兜底 —— 那是抓屏玻璃时代的产物
    //   （当年抓的底图在新尺寸下是旧的，只能铺色顶着）。现在底是**雾**，
    //   重画一遍很便宜，所以尺寸一变就直接重建，全程保持雾化/透明，
    //   不会再出现"缩放期间冒出底色"（用户反馈的那个现象）。
    if (glass_.width() != w_ || glass_.height() != h_) buildGlass();
    frame_ = glass_;
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }

    drawHeader();
    drawResults();
    drawFooter();
    pushFrame();
}

// ── 交互 ────────────────────────────────────────

void SearchWindow::layoutEntry() {
    if (!edit_) return;
    int ex = px(48), ey = px(11);
    int ew = std::max(px(80), w_ - ex - px(14));
    int eh = px(26);
    // 输入框是独立窗口了，坐标要用屏幕坐标
    SetWindowPos(edit_, HWND_TOPMOST, x_ + ex, y_ + ey, ew, eh,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : 0));
    syncEntryColors();
}

void SearchWindow::syncEntryColors() {
    if (!edit_) return;
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }
    InvalidateRect(edit_, nullptr, TRUE);
}

int SearchWindow::rowAt(int y) const {
    if (y < top_) return -1;
    int i = (y - top_) / std::max(1, rowH_);
    int idx = scroll_ + i;
    if (idx < 0 || idx >= (int)hits_.size()) return -1;
    return idx;
}


//
// 做法是把词写进输入框再走一遍正常的 runSearch ——
// 这样搜索行为、结果排序、提示文本与用户手打完全一致，
// 不会出现"岛台搜出来的和手打搜出来的不一样"。
void SearchWindow::runSearch() {
    if (!edit_) return;
    wchar_t buf[512] = {};
    GetWindowTextW(edit_, buf, 512);
    std::wstring q = buf;
    while (!q.empty() && (q.front() == L' ' || q.front() == L'\t')) q.erase(q.begin());
    while (!q.empty() && (q.back() == L' ' || q.back() == L'\t')) q.pop_back();
    query_ = q;
    scroll_ = 0;
    hotRow_ = -1;

    // 匹配/排序全交给共用函数（见文件上方 runQuery 的说明）
    hits_ = runQuery(q, maxHits_);

    sel_ = 0;
    hint_ = hits_.empty()
                ? (q.empty() ? std::wstring()
                             : (g_ready ? std::wstring(L"没有匹配的结果")
                                        : std::wstring(L"索引还没建好…")))
                : std::wstring();
    redraw();
}

// 给岛台面板用：查一次关键词拿回结果，**不改动搜索窗自己的状态**。
std::vector<SearchHit> queryHits(const std::wstring& q, int maxHits) {
    if (maxHits <= 0) maxHits = 200;
    return runQuery(q, maxHits);
}

void SearchWindow::moveSel(int delta) {
    if (hits_.empty()) return;
    sel_ = std::max(0, std::min((int)hits_.size() - 1, sel_ + delta));
    ensureVisible();
    redraw();
}

void SearchWindow::pageSel(int dir) {
    if (hits_.empty()) return;
    int step = std::max(1, rowsVis_ - 1) * (dir > 0 ? 1 : -1);
    sel_ = std::max(0, std::min((int)hits_.size() - 1, sel_ + step));
    ensureVisible();
    redraw();
}

void SearchWindow::ensureVisible() {
    int rows = std::max(1, rowsVis_);
    if (sel_ < scroll_) {
        scroll_ = sel_;
    } else if (sel_ >= scroll_ + rows) {
        scroll_ = sel_ - rows + 1;
    }
}

void SearchWindow::clampSelToView() {
    if (hits_.empty()) return;
    int rows = std::max(1, rowsVis_);
    if (sel_ < scroll_) {
        sel_ = scroll_;
    } else if (sel_ >= scroll_ + rows) {
        sel_ = std::min((int)hits_.size() - 1, scroll_ + rows - 1);
    }
}

void SearchWindow::scrollBy(int rows) {
    if (hits_.empty()) return;
    int maxScroll = std::max(0, (int)hits_.size() - std::max(1, rowsVis_));
    int next = std::max(0, std::min(maxScroll, scroll_ + rows));
    if (next != scroll_) {
        scroll_ = next;
        clampSelToView();
        redraw();
    }
}

void SearchWindow::openSelected() {
    if (hits_.empty()) return;
    open(hits_[(size_t)sel_]);
}

void SearchWindow::revealSelected() {
    if (hits_.empty()) return;
    reveal(hits_[(size_t)sel_]);
}

void SearchWindow::open(const SearchHit& hit) {
    bool filled = false;
    if (attached_ && attachedDialog_) {
        // 贴文件对话框时：把路径填进文件名框并确认（对照 fill_file_dialog）
        filled = fillFileDialog(attached_, hit.path);
    }
    logf("search: 选中 %s  attached=%p dialog=%d filled=%d", toUtf8(hit.name).c_str(),
         (void*)attached_, (int)attachedDialog_, (int)filled);
    if (!filled) {
        ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
        ShellExecuteW(nullptr, L"open", hit.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    hide();
}

void SearchWindow::reveal(const SearchHit& hit) {
    std::wstring sel = L"/select,\"" + hit.path + L"\"";
    ::AllowSetForegroundWindow(ASFW_ANY);   // ★ 允许新进程把窗口带到前台
    ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
    hide();
}

void SearchWindow::sbPress(int y) {
    sbDragging_ = true;
    sbStartY_ = y;
    sbStartScroll_ = scroll_;
}

void SearchWindow::sbDrag(int yRoot) {
    if (!sbDragging_ || sbTravel_ <= 0) return;
    int dy = yRoot - sbStartY_;
    int next = sbStartScroll_ + (int)((double)dy / sbTravel_ * sbMaxScroll_ + 0.5);
    next = std::max(0, std::min(sbMaxScroll_, next));
    if (next != scroll_) {
        scroll_ = next;
        clampSelToView();
        redraw();
    }
}

// ── 显示 / 隐藏 ─────────────────────────────────

void SearchWindow::show(bool activate) {
    if (!hwnd_) return;
    visible_ = true;
    hits_.clear();
    sel_ = 0;
    scroll_ = 0;
    hotRow_ = -1;
    query_.clear();
    hint_.clear();
    if (edit_) {
        SetWindowTextW(edit_, L"");
        ShowWindow(edit_, SW_HIDE);   // 等主窗摆好再露出来，避免闪一下孤零零的输入框
    }

    place(!activate);
    const int s = shadowInset();
    SetWindowPos(hwnd_, HWND_TOPMOST, x_ - s, y_ - s, w_ + s * 2, h_ + s * 2,
                 SWP_NOACTIVATE);
    buildGlass();
    redraw();
    ShowWindow(hwnd_, SW_SHOW);
    SetWindowPos(hwnd_, HWND_TOPMOST, x_ - s, y_ - s, w_ + s * 2, h_ + s * 2,
                 SWP_NOACTIVATE);
    layoutEntry();
    if (activate) {
        // 后台进程直接 SetForegroundWindow 会被系统拒绝（前台锁定），
        // 结果就是窗口刚显示、焦点还在别人身上 -> 140ms 后被自己的失焦逻辑收起。
        // 走 AttachThreadInput 借用前台线程的输入状态才稳。
        HWND fg = GetForegroundWindow();
        DWORD t1 = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD t2 = GetCurrentThreadId();
        bool attached = (t1 && t1 != t2 && AttachThreadInput(t1, t2, TRUE));
        SetForegroundWindow(hwnd_);
        BringWindowToTop(hwnd_);
        if (edit_) SetFocus(edit_);
        if (attached) AttachThreadInput(t1, t2, FALSE);
        logf("search: 前台请求 fg=%p -> hwnd=%p now=%p", (void*)fg, (void*)hwnd_,
             (void*)GetForegroundWindow());
    } else {
        autoMode_ = true;
    }
    SetTimer(hwnd_, 4, 200, nullptr);
    logf("search: 显示 %dx%d @(%d,%d) 索引 %d 条%s", w_, h_, x_, y_, indexCount(),
         g_ready ? "" : "（还没建好）");
}

void SearchWindow::hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, 4);
    visible_ = false;              // 先清，免得 layoutEntry 又把输入框显示出来
    if (edit_) ShowWindow(edit_, SW_HIDE);
    ShowWindow(hwnd_, SW_HIDE);
    // 自动弹出后被用户关掉：告诉主程序别再为同一个对话框弹回来
    if (autoMode_ && attached_ && onAutoClosed) onAutoClosed(attached_);
    attached_ = nullptr;
    attachedDialog_ = false;
    autoMode_ = false;
    // ★ 收起时释放画布（搜索窗不常开，下次显示重烘焙即可）
    glass_.release();
    frame_.release();
    shadow_.release();
    outer_.release();
    logf("search: 收起");
}

void SearchWindow::toggle() {
    if (visible_) {
        hide();
    } else {
        show(true);
    }
}

void SearchWindow::persistSize() {
    if (!cfg_) return;
    double s = dpiScale();
    cfg_->searchW = (int)std::lround(w_ / s);
    cfg_->searchH = (int)std::lround(h_ / s);
    cfg_->searchSizeCustom = true;
    customSize_ = true;      // ★ 内存里也要认，否则同一会话再打开又按目标窗口重算
    customSize_ = true;
    saveConfig(*cfg_);
}

void SearchWindow::persistPos() {
    if (!cfg_) return;
    double s = dpiScale();
    cfg_->searchX = (int)std::lround(x_ / s);
    cfg_->searchY = (int)std::lround(y_ / s);
    cfg_->searchPosCustom = true;
    posCustom_ = true;
    saveConfig(*cfg_);
}

void SearchWindow::followTarget() {
    if (!attached_ || !cfg_ || !cfg_->searchFollow) return;
    if (dragging_ || resizing_) return;
    RECT rc = {};
    RECT ext = {};
    if (!GetWindowRect(attached_, &rc) ||
        !IsWindowVisible(attached_)) {
        hide();
        return;
    }
    if (SUCCEEDED(DwmGetWindowAttribute(attached_, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext))))
        rc = ext;
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ny = 0, nh = 0;
    fitBelow(rc.top - work.top, rc.bottom - work.top, work.bottom - work.top, ny, nh);
    int nx = rc.left;
    int nw = customSize_ ? w_ : std::max(px(kMinW), (int)(rc.right - rc.left));
    if (abs(nx - x_) > 1 || abs(ny + work.top - y_) > 1 || nh != h_ || nw != w_) {
        x_ = nx;
        y_ = work.top + ny;
        w_ = nw;
        h_ = nh;
        const int s = shadowInset();
        SetWindowPos(hwnd_, nullptr, x_ - s, y_ - s, w_ + s * 2, h_ + s * 2,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layoutEntry();
        redraw();
        rebuildGlassOffscreen();
    }
}

void SearchWindow::attachTo(HWND hwnd, bool dialog) {
    if (!hwnd_ || !visible_) return;
    attached_ = hwnd;
    attachedDialog_ = dialog;
    autoMode_ = true;
    userDetached_ = false;
    followTarget();
    // 新对话框刚拿到前台，可能把宿主窗口顶到我们头上，补一下置顶
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    logf("search: 改贴对话框 %p", (void*)hwnd);
}

// ── 窗口过程 ────────────────────────────────────

LRESULT CALLBACK searchEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SearchWindow* self = g_self;
    if (self) {
        switch (msg) {
            case WM_KEYDOWN:
                switch (wp) {
                    case VK_DOWN:
                        self->moveSel(1);
                        return 0;
                    case VK_UP:
                        self->moveSel(-1);
                        return 0;
                    case VK_NEXT:
                        self->pageSel(1);
                        return 0;
                    case VK_PRIOR:
                        self->pageSel(-1);
                        return 0;
                    case VK_RETURN:
                        if (GetKeyState(VK_CONTROL) & 0x8000)
                            self->revealSelected();
                        else
                            self->openSelected();
                        return 0;
                    case VK_ESCAPE:
                        self->hide();
                        return 0;
                    case VK_TAB:
                        return 0;
                    default:
                        break;
                }
                break;
            case WM_MOUSEWHEEL: {
                int d = GET_WHEEL_DELTA_WPARAM(wp);
                self->scrollBy(d > 0 ? -3 : 3);
                return 0;
            }
            case WM_KILLFOCUS:
                // 输入框是独立弹窗，焦点一走就说明点了别处；延迟 140ms 再判，
                // 免得「点结果行 / 重新聚焦输入框」把自己关掉（对照 _on_focus_out）
                if (self->hwnd_) SetTimer(self->hwnd_, 5, 140, nullptr);
                break;
            default:
                break;
        }
    }
    SearchWindow* s = g_self;
    WNDPROC oldProc = s ? s->editOldProc_ : nullptr;
    return CallWindowProcW(oldProc ? oldProc : DefWindowProcW, hwnd, msg, wp, lp);
}

LRESULT CALLBACK SearchWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SearchWindow* self = (SearchWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (SearchWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (self) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SearchWindow::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLOREDIT: {
            // 输入框底色跟玻璃凹槽一致
            uint32_t pill =
                (blendColor(avg_, dark_ ? 0xFFFFFFFFu : 0xFF000000u, dark_ ? 0.10 : 0.05)) |
                0xFF000000u;
            uint32_t text = 0, dim = 0;
            readableText(avg_, dark_, text, dim);
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(rOf(text), gOf(text), bOf(text)));
            SetBkColor(dc, RGB(rOf(pill), gOf(pill), bOf(pill)));
            if (!editBrush_)
                editBrush_ = CreateSolidBrush(RGB(rOf(pill), gOf(pill), bOf(pill)));
            // ★ 与岛台搜索框同一招：EDIT 不支持逐像素透明，但我们可以**让它刷什么
            //   颜色、就把那个颜色键成透明** —— 键色与它刷的底色是同一个颜色，
            //   于是整块填充 100% 透明，只剩文字浮在凹槽那一圈玻璃边上。
            //   每次 CTLCOLOR 都同步一次键色，壁纸/主题变了也不会留旧色边。
            ::SetLayeredWindowAttributes(edit_, RGB(rOf(pill), gOf(pill), bOf(pill)), 0,
                                         LWA_COLORKEY);
            return (LRESULT)editBrush_;
        }

        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE && (HWND)lp == edit_) {
                // 45ms 防抖：一边打字一边搜，但不要每个字符都全表扫一遍
                SetTimer(hwnd, 3, 45, nullptr);
            }
            return 0;

        case WM_TIMER:
            if (wp == 3) {
                KillTimer(hwnd, 3);
                runSearch();
                return 0;
            }
            if (wp == 4) {
                followTarget();
                return 0;
            }
            if (wp == 5) {
                // 失焦后延迟判断：输入框抢焦点、点结果行都会先来一次失焦
                KillTimer(hwnd, 5);
                if (!autoMode_ && !dragging_ && !resizing_) {
                    // 只要前台还是「本进程」的窗口就别收：输入框是独立 popup，
                    // 点它 / 点标签栏都会让主窗 WA_INACTIVE，但用户并没有离开搜索。
                    HWND fg = GetForegroundWindow();
                    DWORD pid = 0;
                    if (fg) GetWindowThreadProcessId(fg, &pid);
                    if (!fg || pid != GetCurrentProcessId()) {
                        logf("search: 失去焦点 -> 自动收起");
                        hide();
                    }
                }
                return 0;
            }
            return 0;

        case WM_NCHITTEST: {
            // ★ 与岛台同一套：把边框区域交还给**系统自己的缩放循环**。
            //   lParam 是**屏幕坐标**，先转客户区、再减掉外阴影留白得到内容坐标，
            //   然后自己判区域（原来复用 zoneAt()，已删除）。
            // ★ 判定坐标必须严格等于"画面里的内容坐标"，而它 =
            //   窗口左上 + 非客户区边框 + 外阴影留白。三者都要算上：
            //
            //   三次直接探测（SendMessage WM_NCHITTEST）发现命中点恒在
            //   "相对窗口左上 = 36"，而 36 = 21（留白）+ 15（WS_THICKFRAME 的
            //   边框）—— 说明原来只减了留白、漏了边框那一圈，判定带因此退化成
            //   ~0 宽（用户："有的角没反应、有的角只能一个方向"）。
            POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr{};
            if (!::GetWindowRect(hwnd, &wr)) return HTCLIENT;
            //   内容原点 = **窗口**左上 + 外阴影留白。
            //   ⚠ 不要再加非客户区边框：UpdateLayeredWindow 的位图贴在窗口左上角，
            //     不是客户区（实测 WS_THICKFRAME 的边框是 10px，多减会让判定再偏 10px）。
            const int s = shadowInset();
            // ★ 只依赖**窗口矩形**（物理事实），不碰 w_/h_（内部记账）——
            //   岛台就是这么做的。原因很实际：下侧/右侧的判定要用内容的
            //   下/右边界，一旦 w_/h_ 比真实内容小一点（客户区 vs 窗口、
            //   隐藏时的残留……），下侧/右侧算出来的坐标就越界被丢弃。
            //   实测正是"上面两个角好、下面两个角完全没反应"。
            const int cl = wr.left + s, ct = wr.top + s;   // 内容四角
            const int crr = wr.right - s, cbb = wr.bottom - s;
            const int corner = px(24);                     // 角：36 物理像素
            const bool cL = p.x < cl + corner;
            const bool cR = p.x >= crr - corner;
            const bool cT = p.y < ct + corner;
            const bool cB = p.y >= cbb - corner;
            if (cB && cR) return HTBOTTOMRIGHT;
            if (cB && cL) return HTBOTTOMLEFT;
            if (cT && cR) return HTTOPRIGHT;
            if (cT && cL) return HTTOPLEFT;
            return HTCLIENT;
            // ★ 照岛台：**只留四个角**当缩放柄，四条边不进缩放。
            //
            //   岛台源码里就写着用户当初的原话 ——
            //   "拖动一个角的时候上下左右移动的去改面板大小，删除拉边框更改的操作"，
            //   所以岛台四条边不再返回 HTLEFT/HTTOP 等。
            //
            //   我上一版给边也返回了 HT 码，结果手落点稍偏就命中一条边：
            //   "有的角只能左右、有的角只能上下、有的角干脆没反应"。
            //   现在拖任意一个角 = 系统按 HT*CORNER 同时改宽改高。

        }

        case WM_SETCURSOR: {
            // ★ 用户明确要求：鼠标移到面板边框上**不要变光标**（保持普通箭头）。
            //   带 WS_THICKFRAME 后系统会在边框上换成缩放光标，这里拦掉；
            //   输入框那种靠子窗口自己设的 I 形光标不受影响。
            const UINT hit = LOWORD(lp);
            if (hit == HTLEFT || hit == HTRIGHT || hit == HTTOP || hit == HTBOTTOM ||
                hit == HTTOPLEFT || hit == HTTOPRIGHT || hit == HTBOTTOMLEFT ||
                hit == HTBOTTOMRIGHT) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        }

        case WM_SIZING: {
            // ★ 照岛台：**拖动过程中就同步内部尺寸并重绘**。
            //
            //   岛台注释原话："分层窗口有个坑：窗口矩形由系统改（SetWindowPos），
            //   但**画出来的大小**由 UpdateLayeredWindow 的 psize 决定。
            //   如果只在拖动结束时更新 w_/h_，拖动全程画面都停在旧尺寸，
            //   看起来就像'拖不动'。在这里实时同步，拖的时候就能看到面板跟着变。"
            //
            //   这正是搜索窗之前"角拖不动"的直接原因：只在
            //   WM_WINDOWPOSCHANGED / WM_EXITSIZEMOVE 里同步，psize 一直没更新。
            //
            //   顺带夹住最小尺寸（夹的是内容，窗口另加四周留白），
            //   防止把面板拖成一条缝找不回来。不写配置，持久化交给 EXITSIZEMOVE。
            RECT* r = reinterpret_cast<RECT*>(lp);
            if (!r) return TRUE;
            const int s = shadowInset();
            const int minW = px(kMinW) + s * 2;
            const int minH = px(kMinH) + s * 2;
            if (r->right - r->left < minW) {
                if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) {
                    r->left = r->right - minW;
                } else {
                    r->right = r->left + minW;
                }
            }
            if (r->bottom - r->top < minH) {
                if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) {
                    r->top = r->bottom - minH;
                } else {
                    r->bottom = r->top + minH;
                }
            }
            const int nw = (r->right - r->left) - s * 2;
            const int nh = (r->bottom - r->top) - s * 2;
            if (nw > 0 && nh > 0 && (nw != w_ || nh != h_)) {
                w_ = nw;
                h_ = nh;
                x_ = r->left + s;
                y_ = r->top + s;
                buildGlass();
                layoutEntry();     // ★ 输入框是独立顶层窗口，必须跟着重摆
                redraw();          // 提交新 psize —— 画面才会跟着变
            }
            return TRUE;
        }


        case WM_ENTERSIZEMOVE:
            // 系统缩放循环开始：这期间不要因为"失焦"把面板收起来
            resizing_ = true;
            return 0;

        case WM_EXITSIZEMOVE:
            resizing_ = false;
            {
                // ★ 松手了才刷新 —— 这是整个拖动/缩放过程中**唯一**一次重建重绘
                //   （照岛台："松手后在 WM_EXITSIZEMOVE 里刷新一次"）。
                const int s = shadowInset();
                RECT wr{};
                if (::GetWindowRect(hwnd, &wr)) {
                    x_ = wr.left + s;
                    y_ = wr.top + s;
                    w_ = std::max(px(kMinW), (int)(wr.right - wr.left) - s * 2);
                    h_ = std::max(px(kMinH), (int)(wr.bottom - wr.top) - s * 2);
                }
                buildGlass();
                layoutEntry();
                redraw();
                // ★ 缩放结束 = 用户调完了大小，立刻落盘。
                //   用户反馈："跟随资源管理器时调好的大小，重启后又变回去" ——
                //   根因是持久化只在 WM_LBUTTONUP 里做，而系统缩放循环下
                //   WM_LBUTTONUP 不一定送到（见 WM_LBUTTONUP 里那句写死的
                //   wasResize = false）。WM_EXITSIZEMOVE 一定会来。
                //   ⚠ 只存尺寸，**不存位置** —— 位置要继续跟随目标窗口。
                persistSize();
            }
            return 0;

        case WM_WINDOWPOSCHANGED: {
            // ⚠ 用 WM_WINDOWPOSCHANGED 而不是 WM_SIZE（照抄岛台那条实测结论）：
            //   分层窗口被系统拖动改尺寸时 **WM_SIZE 不一定发**，
            //   但 WM_WINDOWPOSCHANGED 一定会来。不处理它的表现就是
            //   "能拖动改大小，但松手后又被画回原尺寸"。
            //
            //   而且**位置也必须同步**：拖左边/上边时系统会移动窗口原点，
            //   而提交用的是我们自己算的 dst=(x_-s, y_-s)，位置不同步的话
            //   下一帧就把窗口拉回原位，拖动全程在打架。
            //
            //   窗口矩形 = 内容 + 四周留白 ⇒ 位置 +s、尺寸 -2s。
            auto wpos = reinterpret_cast<WINDOWPOS*>(lp);
            if (wpos) {
                const int s = shadowInset();
                // ⚠ 尺寸必须用 **GetWindowRect（窗口矩形）**，不能用 wpos->cx：
                //   带 WS_THICKFRAME 时 wpos->cx 是**客户区**尺寸，比窗口小
                //   2×边框（实测 10px×2 = 20px），而 UpdateLayeredWindow 的位图
                //   是按**窗口**尺寸贴的（psize = 内容 + 2×留白）。
                //   用客户区尺寸会让 w_/h_ 比真实内容小 20px —— 于是右侧/下侧的
                //   判定算出来的坐标落到 w_/h_ 之外会被判空，
                //   表现就是"右下角怎么拖都没反应"（实测扫描确认）。
                RECT wr{};
                if (!(wpos->flags & SWP_NOMOVE)) {
                    if (::GetWindowRect(hwnd, &wr)) {
                        x_ = wr.left + s;
                        y_ = wr.top + s;
                    } else {
                        x_ = wpos->x + s;
                        y_ = wpos->y + s;
                    }
                }
                bool sizeChanged = false;
                if (!(wpos->flags & SWP_NOSIZE) && ::GetWindowRect(hwnd, &wr)) {
                    const int nw = std::max(px(kMinW), (int)(wr.right - wr.left) - s * 2);
                    const int nh = std::max(px(kMinH), (int)(wr.bottom - wr.top) - s * 2);
                    if (nw != w_ || nh != h_) {
                        w_ = nw;
                        h_ = nh;
                        sizeChanged = true;
                    }
                }
                // ★ 缩放/拖动期间**只同步几何，什么都不刷新**（照岛台）。
                //
                //   岛台注释原话："拖动中不做任何刷新。用户反馈：拖动的时候面板
                //   有频闪，不要连带面板一起刷新。确实如此：每 100ms 重烘焙一遍、
                //   再 UpdateLayeredWindow 推一帧，而系统同时在移窗口 —— 两边一叠
                //   就是频闪。现在：拖动中完全不碰，只让系统移动窗口；松手后在
                //   WM_EXITSIZEMOVE 里刷新一次。"
                //
                //   我这里每帧 buildGlass()（整张雾 + 逐像素距离场外阴影）+ redraw()
                //   + 提交，属于几百毫秒级的重活 —— 直接把系统缩放循环堵死，
                //   实测表现是"宽度只挪一点、高度干脆不动"。
                (void)sizeChanged;
                if (sizeChanged) {
                    layoutEntry();   // 输入框是独立窗口，跟着挪一下（很便宜）
                }
            }
            break;  // 继续交给 DefWindowProc
        }

        case WM_MOUSEMOVE: {
            const POINT cp = searchContentPt(hwnd, lp);
                int mx = cp.x, my = cp.y;
            if (dragging_) {
                POINT cur = {};
                GetCursorPos(&cur);
                int nx = cur.x - dragOffset_.x, ny = cur.y - dragOffset_.y;
                if (nx != x_ || ny != y_) {
                    dragMoved_ = true;
                    userDetached_ = true;
                }
                x_ = nx;
                y_ = ny;
                {
                    const int s = shadowInset();
                    SetWindowPos(hwnd, nullptr, x_ - s, y_ - s, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                layoutEntry();
                redraw();
                return 0;
            }
            if (sbDragging_) {
                POINT cur = {};
                GetCursorPos(&cur);
                sbDrag(cur.y);
                return 0;
            }
            int idx = rowAt(my);
            if (idx != hotRow_) {
                hotRow_ = (idx >= 0) ? idx : -1;
                SetCursor(LoadCursorW(nullptr, idx >= 0 ? IDC_HAND : IDC_ARROW));
                redraw();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT cp = searchContentPt(hwnd, lp);
                int mx = cp.x, my = cp.y;
            // 点窗里任何一处都确保自己是前台、输入框有焦点（对照 _on_any_click）
            HWND fg = GetForegroundWindow();
            if (fg != hwnd && fg != edit_) SetForegroundWindow(hwnd);
            if (edit_ && GetFocus() != edit_) SetFocus(edit_);
            // 滚动条
            if (mx >= w_ - px(18) && mx <= w_ - px(9) && my >= top_ && my <= h_ - px(32) &&
                (int)hits_.size() > rowsVis_) {
                POINT cur = {};
                GetCursorPos(&cur);
                sbPress(cur.y);
                SetCapture(hwnd);
                return 0;
            }
            // 放大镜区域 → 拖窗口
            if (mx <= px(44) && my <= px(48)) {
                POINT cur = {};
                GetCursorPos(&cur);
                dragOffset_.x = cur.x - x_;
                dragOffset_.y = cur.y - y_;
                dragging_ = true;
                dragMoved_ = false;
                attached_ = nullptr;
                attachedDialog_ = false;
                autoMode_ = false;
                SetCapture(hwnd);
                return 0;
            }
            int idx = rowAt(my);
            if (idx >= 0) {
                sel_ = idx;
                open(hits_[(size_t)idx]);
            }
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) SetTimer(hwnd, 5, 140, nullptr);
            return 0;

        case WM_LBUTTONUP: {
            if (sbDragging_) {
                sbDragging_ = false;
                ReleaseCapture();
                return 0;
            }
            // 注：缩放不在这里收尾 —— 系统的 WS_THICKFRAME 循环下
            //     WM_LBUTTONUP 不一定送到，缩放的落盘放在 WM_EXITSIZEMOVE。
            bool wasDrag = dragging_ && dragMoved_;
            bool was = resizing_ || dragging_;
            resizing_ = false;
            dragging_ = false;

            dragMoved_ = false;
            if (was) {
                ReleaseCapture();
                rebuildGlassOffscreen();
                syncEntryColors();
                if (wasDrag) {
                    persistPos();
                    persistSize();
                }
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            scrollBy(d > 0 ? -3 : 3);
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                hide();
                return 0;
            }
            return 0;

        case WM_CLOSE:
            hide();
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 3);
            KillTimer(hwnd, 4);
            if (editBrush_) {
                DeleteObject(editBrush_);
                editBrush_ = nullptr;
            }
            hwnd_ = nullptr;
            return 0;

        default:
            break;
    }
    if (msg == WM_INDEX_READY) {
        if (!query_.empty()) runSearch();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace hr
