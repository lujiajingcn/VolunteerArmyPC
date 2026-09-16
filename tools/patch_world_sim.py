# -*- coding: utf-8 -*-
"""修正 world_sim.cpp：删掉误留的占位调用、补 Time 头文件。"""
import io

P = r'E:\study_lujiajing\Games\WorkSpace_VolunteerArmy\VolunteerArmyPC\src\node\world_sim.cpp'
src = io.open(P, encoding='utf-8').read()

pairs = [
    ("""        if (!p->dead && !p->downed) {
            va::W_unused_guard();   // 占位：防止误删本段（无副作用）
        }
""", ""),
    ('#include <godot_cpp/classes/scene_tree.hpp>',
     '#include <godot_cpp/classes/scene_tree.hpp>\n#include <godot_cpp/classes/time.hpp>'),
]
n = 0
for old, new in pairs:
    if old in src:
        src = src.replace(old, new)
        n += 1
        print('OK  替换:', old.strip().splitlines()[0][:70])
    else:
        print('MISS 未找到:', old.strip().splitlines()[0][:70])
io.open(P, 'w', encoding='utf-8', newline='').write(src)
print('共 %d 处，字节 %d' % (n, len(io.open(P, 'rb').read())))
