#pragma once
// VolunteerArmyPC —— UTF-8 字符级工具
//
// 为什么需要它：网页版的语音指令解析全部建立在「一个汉字 = 一个字符」的前提上 ——
//   s.length        → 用字符数算置信度
//   s.slice(i, i+n) → 按字符切片做模糊匹配
//   charSim(a,b)    → 逐字符的最长公共子序列
// C++ 的 std::string 是字节串，一个汉字占 3 字节，直接照搬这些操作会得到错误结果
// （切片会把汉字劈成半个，LCS 会把 3 个字节当 3 个字符，相似度完全失真）。
// 所以这里把字符串先解码成码点序列再处理。
#include <cstdint>
#include <string>
#include <vector>

namespace va {

// UTF-8 → 码点序列
inline std::vector<uint32_t> utf8_codes(const std::string &s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }   // 非法首字节，跳过
        if (i + extra >= n) break;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { cp = 0; extra = k - 1; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

// 码点序列 → UTF-8
inline std::string utf8_from_codes(const std::vector<uint32_t> &v) {
    std::string out;
    out.reserve(v.size() * 3);
    for (uint32_t cp : v) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// 字符数（等价 JS 的 str.length，对 BMP 中文成立）
inline size_t utf8_len(const std::string &s) { return utf8_codes(s).size(); }

// 按字符切片（等价 JS 的 slice(i, i+n)）
inline std::vector<uint32_t> utf8_slice(const std::vector<uint32_t> &cps, size_t start, size_t len) {
    if (start >= cps.size()) return {};
    size_t end = start + len;
    if (end > cps.size()) end = cps.size();
    return std::vector<uint32_t>(cps.begin() + start, cps.begin() + end);
}

inline std::string utf8_slice_str(const std::string &s, size_t start, size_t len) {
    return utf8_from_codes(utf8_slice(utf8_codes(s), start, len));
}

// 在码点序列里找子序列（等价 JS indexOf，返回字符下标，找不到返回 -1）
inline int utf8_find(const std::vector<uint32_t> &hay, const std::vector<uint32_t> &needle) {
    if (needle.empty() || hay.size() < needle.size()) return -1;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (hay[i + j] != needle[j]) { ok = false; break; }
        }
        if (ok) return (int)i;
    }
    return -1;
}

} // namespace va
