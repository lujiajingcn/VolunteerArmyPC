# -*- coding: utf-8 -*-
"""把网页版的逻辑层（工具函数 + CFG + 一~八章）抽成独立参考文件，供 C++ 移植逐行对照。"""
import os, re

SRC = r'E:\study_lujiajing\Games\WorkSpace_VolunteerArmy\VolunteerArmy\index.html'
OUT = r'E:\study_lujiajing\Games\WorkSpace_VolunteerArmy\VolunteerArmyPC\tools\ref'

os.makedirs(OUT, exist_ok=True)
t = open(SRC, encoding='utf-8').read()
lines = t.split('\n')

# 逻辑层：从 "<script>" 后第一行注释块开始，到 "九、渲染" 注释块之前
start = None
for i, ln in enumerate(lines):
    if '断头谷公路 · 伏击指挥' in ln:
        start = i
        break
end = None
for i, ln in enumerate(lines):
    if ln.strip().startswith('9.7') or '九、渲染' in ln:
        end = i
        break

print('逻辑层起止行: %s ~ %s' % (start + 1 if start else None, end + 1 if end else None))
logic = '\n'.join(lines[start:end])
p = os.path.join(OUT, 'logic_ref.js')
open(p, 'w', encoding='utf-8').write(logic)
print('已写出 %s' % p)
print('  字符 %d  行 %d' % (len(logic), logic.count('\n') + 1))

# 顺便把音频层也导出（后续要移植到 Godot AudioStream）
snd_start = None
for i, ln in enumerate(lines):
    if '音频' in ln and ln.strip().startswith('/*'):
        snd_start = i
        break
print('音频段起始行:', snd_start + 1 if snd_start else None)

# 统计逻辑层的顶层函数名
fns = re.findall(r'^function\s+(\w+)\s*\(', logic, re.M)
print('逻辑层顶层函数 %d 个' % len(fns))
print('  ', ', '.join(fns))
