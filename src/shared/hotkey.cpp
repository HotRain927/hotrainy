// hr/hotkey.cpp — 快捷键文本与虚拟键码互转（对照 core/hotkey.py）
#include "hr/shared/hotkey.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>

namespace hr {
namespace {

// 名字 → 虚拟键码（小写匹配，与 Python 的 _VK 表一致）
const std::map<std::string, UINT>& vkTable() {
    static std::map<std::string, UINT> tbl = [] {
        std::map<std::string, UINT> t;
        t["space"] = VK_SPACE;
        t["enter"] = VK_RETURN;
        t["return"] = VK_RETURN;
        t["tab"] = VK_TAB;
        t["esc"] = VK_ESCAPE;
        t["escape"] = VK_ESCAPE;
        t["backspace"] = VK_BACK;
        t["delete"] = VK_DELETE;
        t["insert"] = VK_INSERT;
        t["home"] = VK_HOME;
        t["end"] = VK_END;
        t["pageup"] = VK_PRIOR;
        t["pagedown"] = VK_NEXT;
        t["up"] = VK_UP;
        t["down"] = VK_DOWN;
        t["left"] = VK_LEFT;
        t["right"] = VK_RIGHT;
        t["`"] = VK_OEM_3;
        t["-"] = VK_OEM_MINUS;
        t["="] = VK_OEM_PLUS;
        t["["] = VK_OEM_4;
        t["]"] = VK_OEM_6;
        t["\\"] = VK_OEM_5;
        t[";"] = VK_OEM_1;
        t["'"] = VK_OEM_7;
        t[","] = VK_OEM_COMMA;
        t["."] = VK_OEM_PERIOD;
        t["/"] = VK_OEM_2;
        for (char ch = 'A'; ch <= 'Z'; ++ch) {
            std::string one(1, (char)(ch - 'A' + 'a'));
            t[one] = (UINT)ch;
        }
        for (char ch = '0'; ch <= '9'; ++ch) t[std::string(1, ch)] = (UINT)ch;
        for (int i = 1; i <= 24; ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "f%d", i);
            t[buf] = (UINT)(VK_F1 + i - 1);
        }
        return t;
    }();
    return tbl;
}

// 虚拟键码 → 显示名（与 Python 的 _DISPLAY 一致）
std::wstring vkName(UINT vk) {
    struct Fixed {
        UINT vk;
        const wchar_t* name;
    };
    static const Fixed kFixed[] = {
        {VK_SPACE, L"Space"},     {VK_RETURN, L"Enter"},   {VK_TAB, L"Tab"},
        {VK_ESCAPE, L"Esc"},      {VK_BACK, L"Backspace"}, {VK_DELETE, L"Delete"},
        {VK_INSERT, L"Insert"},   {VK_HOME, L"Home"},      {VK_END, L"End"},
        {VK_PRIOR, L"PageUp"},    {VK_NEXT, L"PageDown"},  {VK_UP, L"Up"},
        {VK_DOWN, L"Down"},       {VK_LEFT, L"Left"},      {VK_RIGHT, L"Right"},
        {VK_OEM_3, L"`"},         {VK_OEM_MINUS, L"-"},    {VK_OEM_PLUS, L"="},
        {VK_OEM_4, L"["},         {VK_OEM_6, L"]"},        {VK_OEM_5, L"\\"},
        {VK_OEM_1, L";"},         {VK_OEM_7, L"'"},        {VK_OEM_COMMA, L","},
        {VK_OEM_PERIOD, L"."},    {VK_OEM_2, L"/"},
    };
    for (const auto& f : kFixed) {
        if (f.vk == vk) return f.name;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    wchar_t buf[16];
    std::swprintf(buf, 16, L"0x%02X", vk);
    return buf;
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

bool parseHotkey(const std::string& spec, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    if (spec.empty()) return false;
    const auto& tbl = vkTable();
    bool hasVk = false;

    size_t start = 0;
    while (start <= spec.size()) {
        size_t pos = spec.find('+', start);
        std::string part = spec.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        // 去掉首尾空白
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t' || part.back() == '\r')) part.pop_back();

        if (!part.empty()) {
            std::string low = lower(part);
            if (low == "ctrl" || low == "control") {
                mods |= MOD_CONTROL;
            } else if (low == "alt") {
                mods |= MOD_ALT;
            } else if (low == "shift") {
                mods |= MOD_SHIFT;
            } else if (low == "win" || low == "super" || low == "meta") {
                mods |= MOD_WIN;
            } else if (low.size() > 2 && low[0] == '0' && low[1] == 'x') {
                // ★ vkName() 对"表外"的键返回 "0xXX"（小键盘、媒体键、某些 IME 键…）。
                //   这里必须认它 —— 否则「录音器写出的字符串」解析器读不懂，
                //   表现就是改快捷键**永远提示已被占用**（用户报的）。
                const unsigned long v = std::strtoul(low.c_str() + 2, nullptr, 16);
                if (v == 0 || v > 0xFF) return false;
                vk = (UINT)v;
                hasVk = true;
            } else {
                auto it = tbl.find(low);
                if (it == tbl.end()) return false;
                vk = it->second;
                hasVk = true;
            }
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return hasVk && vk != 0;
}

std::string formatHotkey(UINT mods, UINT vk) {
    std::string out;
    auto add = [&out](const char* s) {
        if (!out.empty()) out += "+";
        out += s;
    };
    if (mods & MOD_CONTROL) add("Ctrl");
    if (mods & MOD_ALT) add("Alt");
    if (mods & MOD_SHIFT) add("Shift");
    if (mods & MOD_WIN) add("Win");

    std::wstring name = vkName(vk);
    std::string utf8;
    int need = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), nullptr, 0, nullptr,
                                   nullptr);
    utf8.resize((size_t)std::max(0, need));
    if (need > 0) {
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), (int)name.size(), &utf8[0], need, nullptr,
                            nullptr);
    }
    // ★ 这里原来直接 out += utf8 —— 少了它前面那个 '+'。
    //
    //   于是 "Ctrl+Alt+Space" 被写成 "Ctrl+AltSpace"；
    //   parseHotkey 按 '+' 切分只拿到 "altspace"，查表失败 -> 解析失败 ->
    //   注册必然失败 -> 设置界面**永远显示"已被占用"**（用户报的 bug）。
    //   单元测试（把 1..0xFF 全跑一遍）一下就把它抓出来了。
    if (!out.empty() && !utf8.empty()) out += "+";
    out += utf8;
    return out;
}

}  // namespace hr
