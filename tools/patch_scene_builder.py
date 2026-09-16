# -*- coding: utf-8 -*-
"""对 scene_builder.cpp 做一次性原子替换（避免多次编辑互相覆盖）。"""
import io, sys, hashlib

P = r'E:\study_lujiajing\Games\WorkSpace_VolunteerArmy\VolunteerArmyPC\src\node\scene_builder.cpp'

RULES = [
    # 命名空间前缀（va::）
    ('return Color(clampf(', 'return Color(va::clampf('),
    ('clampf(base_g + k * 0.9f', 'va::clampf(base_g + k * 0.9f'),
    ('clampf(base_b + k * 0.7f', 'va::clampf(base_b + k * 0.7f'),
    ('const float len = distf(x1, y1, x2, y2);', 'const float len = va::distf(x1, y1, x2, y2);'),

    # Sky / Environment 真实 API
    ('sky->set_sky_material(sky_mat);', 'sky->set_material(sky_mat);'),
    ('env->set_background_mode(', 'env->set_background('),
    ('env->set_ambient_light_source(', 'env->set_ambient_source('),
    ('env->set_tonemap_mode(', 'env->set_tonemapper('),
    ('env->set_glow_hdr_threshold(', 'env->set_glow_hdr_bleed_threshold('),

    # Light3D：Godot 4 把多数属性挪到了 set_param(Param, float)
    ('sun->set_light_color(', 'sun->set_color('),
    ('sun->set_light_energy(1.35f);', 'sun->set_param(Light3D::PARAM_ENERGY, 1.35f);'),
    ('sun->set_shadow_enabled(true);', 'sun->set_shadow(true);'),
    ('sun->set_directional_shadow_mode(', 'sun->set_shadow_mode('),
    ('sun->set_directional_shadow_max_distance(', 'sun->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, '),
    ('sun->set_directional_shadow_split_1(', 'sun->set_param(Light3D::PARAM_SHADOW_SPLIT_1_OFFSET, '),
    ('sun->set_directional_shadow_split_2(', 'sun->set_param(Light3D::PARAM_SHADOW_SPLIT_2_OFFSET, '),
    ('sun->set_directional_shadow_split_3(', 'sun->set_param(Light3D::PARAM_SHADOW_SPLIT_3_OFFSET, '),
    ('sun->set_shadow_normal_bias(', 'sun->set_param(Light3D::PARAM_SHADOW_NORMAL_BIAS, '),
    ('sun->set_shadow_bias(', 'sun->set_param(Light3D::PARAM_SHADOW_BIAS, '),
]

src = io.open(P, encoding='utf-8').read()
before = hashlib.md5(src.encode('utf-8')).hexdigest()[:12]
total = 0
for old, new in RULES:
    n = src.count(old)
    if n:
        src = src.replace(old, new)
        total += n
    print('%-6s x%d  %s' % ('OK' if n else 'MISS', n, old[:66]))

# 补上 blend_splits（可选增强）
anchor = '    sun->set_shadow_mode(DirectionalLight3D::SHADOW_PARALLEL_4_SPLITS);\n'
if 'set_blend_splits' not in src and anchor in src:
    src = src.replace(anchor, anchor + '    sun->set_blend_splits(true);\n')
    total += 1
    print('%-6s x1  加上 set_blend_splits(true)' % 'OK')

# 头文件：补 light3d.hpp（PARAM 枚举）
inc = '#include <godot_cpp/classes/directional_light3d.hpp>\n'
if '#include <godot_cpp/classes/light3d.hpp>' not in src:
    src = src.replace(inc, inc + '#include <godot_cpp/classes/light3d.hpp>\n')
    total += 1
    print('%-6s x1  加上 light3d.hpp 头文件' % 'OK')

io.open(P, 'w', encoding='utf-8', newline='').write(src)
after = hashlib.md5(io.open(P, encoding='utf-8').read().encode('utf-8')).hexdigest()[:12]
print()
print('共替换 %d 处   md5 %s -> %s' % (total, before, after))
print('字节 %d' % len(io.open(P, 'rb').read()))
