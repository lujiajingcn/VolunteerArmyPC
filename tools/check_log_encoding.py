#!/usr/bin/env python3
"""检查日志调用里的非 ASCII 字面量是否都走了 String::utf8()。

为什么需要它
------------
godot-cpp 的 String(const char *) 直接绑定引擎的窄字符构造，那是按 **Latin-1**
逐字节取码点的：源文件里的中文是 UTF-8 字节，"取证" = E5 8F 96 会被读成三个
Latin-1 字符，写到日志里再按 UTF-8 编码，就变成 C3A5 C28F C296（二次编码）。
只有 String::utf8() 才按 UTF-8 解释。

所以任何 `UtilityFunctions::print("中文…")` 出来的都是乱码 ——
而本项目的回归判据恰恰是「日志里有没有 ERROR / 有没有某条打点」，
日志读不了等于判据失效。这类错误编译期完全不报，只能靠这个检查兜住。

用法
----
    python tools/check_log_encoding.py            # 扫 src/，有发现则退出码 1
    python tools/check_log_encoding.py <目录…>     # 指定目录

注意
----
只检查**日志调用**（print / print_rich / push_warning / push_error）的参数表内。
别处的非 ASCII 窄字面量是合法的：例如 `va::W.overKind = "成功"` 赋给 std::string，
那本来就是字节透传，不能包 String::utf8（类型都不对）。
"""
import glob
import os
import re
import sys

CALL = re.compile(
    r"\b(?:UtilityFunctions::(?:print|print_rich|push_warning|push_error)|godot::print)\s*\("
)
DEFAULT_ROOTS = ["src"]


def strip_comments(text):
    """把注释替换成同长度空白（保留换行与偏移，行号才准）。"""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out[i:j] = [' '] * (j - i)
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out[i:j] = ['\n' if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
            i += 1
        else:
            i += 1
    return ''.join(out)


def match_paren(text, open_idx):
    depth, i, n = 0, open_idx, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
            i += 1
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def utf8_arg_spans(span):
    """span 里所有 String::utf8( … ) 的实参区间（左括号位置, 右括号位置）。

    为什么按"区间"判而不是按"字面量紧跟在 String::utf8( 之后"判：
    实参可以是个表达式 —— 例如
        String::utf8(p_ok ? "失焦" : "界面外壳")
    每个分支都是 `const char *` 字面量，String::utf8 会把它按 UTF-8 解释，
    是**正确**写法；但"紧跟"式的判据会把它当成漏包而误报。
    （注意：包到 std::string 上就不对了 —— 那种情况形如
        va::W.overKind = "成功";
    不在这条日志判据的管辖范围内，见文件头。）
    """
    out = []
    for m in re.finditer(r"String::utf8\s*\(", span):
        op = span.index('(', m.start())
        cl = match_paren(span, op)
        if cl > 0:
            out.append((op, cl))
    return out


def find_unwrapped(path):
    raw = open(path, encoding='utf-8', errors='replace').read()
    masked = strip_comments(raw)
    found = []
    for m in CALL.finditer(masked):
        start = masked.find('(', m.end() - 1)
        end = match_paren(raw, start)
        if end < 0:
            continue
        span = raw[start:end + 1]
        if not any(ord(ch) > 127 for ch in span):
            continue
        wrapped = utf8_arg_spans(span)
        # 逐个字面量看：含非 ASCII 且没落在任何 String::utf8( … ) 的实参区间内
        for lit in re.finditer(r'"((?:[^"\\]|\\.)*)"', span):
            if not any(ord(c) > 127 for c in lit.group(1)):
                continue
            if any(a < lit.start() < b for a, b in wrapped):
                continue
            line = raw.count('\n', 0, start + lit.start()) + 1
            found.append((line, lit.group(1)))
    return found


def main(argv):
    roots = argv[1:] or DEFAULT_ROOTS
    total = 0
    for root in roots:
        pattern = os.path.join(root, "**", "*.cpp")
        for path in sorted(glob.glob(pattern, recursive=True)):
            for line, text in find_unwrapped(path):
                print("%s:%d" % (path.replace(os.sep, '/'), line))
                print('    "%s"' % text[:88])
                total += 1
    if total:
        print("\n发现 %d 处日志字面量没走 String::utf8()，中文会是乱码。" % total)
        print("改法：UtilityFunctions::print(String::utf8(\"中文…\"), ...)")
        return 1
    print("OK：日志调用里的非 ASCII 字面量都已走 String::utf8()。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
