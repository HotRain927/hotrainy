// hr/search/pinyin.cpp — 汉字 → 拼音查表与整串转换。
#include "hr/search/pinyin.h"

#include "hr/search/pinyin_data.h"

namespace hr {
namespace {

const char* sylAt(uint16_t off) {
    return pydata::kSyllables + off;
}

bool isAsciiLower(wchar_t c) { return c >= L'a' && c <= L'z'; }
bool isAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool isAsciiUpper(wchar_t c) { return c >= L'A' && c <= L'Z'; }

}  // namespace

int pinyinOf(wchar_t ch, const char** out, int maxOut) {
    if (maxOut <= 0) return 0;
    if (ch < 0x4E00 || ch > 0xA000) return 0;
    const uint16_t key = (uint16_t)(ch - 0x4E00);

    // 二分找第一个 cp >= key 的条目；同一个字的多读音在表里是连续的，
    // 所以往后扫到 cp 变化为止就是它的全部读音。
    int lo = 0, hi = pydata::kEntryCount;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (pydata::kEntries[mid].cp < key) lo = mid + 1;
        else hi = mid;
    }
    int n = 0;
    for (int i = lo; i < pydata::kEntryCount && n < maxOut; ++i) {
        if (pydata::kEntries[i].cp != key) break;
        out[n++] = sylAt(pydata::kEntries[i].off);
    }
    return n;
}

void pinyinForms(const std::wstring& text, PyForms& out) {
    out.full.clear();
    out.fullAlt.clear();
    out.initials.clear();

    const char* rs[3];
    bool sawHan = false;
    bool altValid = true;   // fullAlt 只有在"主读音/次读音长度一致"时才对得齐

    for (wchar_t ch : text) {
        // 原串里的 ASCII 字母/数字原样保留（文件名里的英文、数字要能搜）
        if (isAsciiLower(ch) || isAsciiDigit(ch)) {
            out.full.push_back((char)ch);
            out.initials.push_back((char)ch);
            if (altValid) out.fullAlt.push_back((char)ch);
            continue;
        }
        if (isAsciiUpper(ch)) {
            const char c = (char)(ch - L'A' + 'a');
            out.full.push_back(c);
            out.initials.push_back(c);
            if (altValid) out.fullAlt.push_back(c);
            continue;
        }

        const int n = pinyinOf(ch, rs, 3);
        if (n <= 0) continue;   // 非汉字（空格/点/横线/括号…）：跳过，匹配时当成不存在

        sawHan = true;
        out.full += rs[0];
        out.initials.push_back(rs[0][0]);
        // 多音字：第二个读音单独拼一份，用于「银行(yinhang)」这种主读音对不上的情况。
        // 只有"这一处用了次读音"的串才有意义；长度不一致就整体作废（避免错位假匹配）。
        if (altValid) {
            if (n >= 2) out.fullAlt += rs[1];
            else out.fullAlt += rs[0];
        }
    }

    if (!sawHan) {
        // 没有汉字：full 留空 —— 调用方据此跳过拼音匹配，别让纯英文名多花时间
        out.full.clear();
        out.fullAlt.clear();
        out.initials.clear();
        return;
    }
    // fullAlt 与 full 完全相同 = 没有多音字 → 没必要留着
    if (out.fullAlt == out.full) out.fullAlt.clear();
}

}  // namespace hr