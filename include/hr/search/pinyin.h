// hr/search/pinyin.h — 汉字 → 拼音（全拼 / 首字母），给中文文件名的拼音搜索用。
//
// 用户要求："搜索中文的文件的时候可以用拼音搜，可以全拼也可以拼音首字母"。
// 例：「工作报表.xlsx」应当能被 gongzuobaobiao、gzb、zuobao 都搜到。
#pragma once

#include <string>

namespace hr {

// 一个汉字的全部读音（最多 maxOut 个），返回实际个数；非汉字返回 0。
// ⚠ 返回的指针指向静态数据，调用方不要释放。
int pinyinOf(wchar_t ch, const char** out, int maxOut);

// 一个名字的几种可搜索形式（都是纯 ASCII 小写）。
struct PyForms {
    std::string full;       // 主读音全拼：工作报表 → gongzuobaobiao
    std::string fullAlt;    // 多音字的第二读音：银(行) → yinhang（主读音是 xing 时用它兜底）
    std::string initials;   // 主读音首字母：工作报表 → gzbb

    bool empty() const { return full.empty(); }
};

// 把整串转成上面几种形式。
//   · 汉字取拼音；原串里的 ASCII 字母/数字原样保留（统一小写）
//   · 其余字符（空格、点、横线、括号…）跳过 —— 这样 gzb 能跨过 "工作-报表" 匹配
//   · 完全没有汉字时 full 为空，调用方据此跳过拼音匹配（纯英文名走原有逻辑更快）
void pinyinForms(const std::wstring& text, PyForms& out);

}  // namespace hr