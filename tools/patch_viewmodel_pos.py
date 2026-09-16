# -*- coding: utf-8 -*-
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
p = os.path.join(ROOT, "src", "node", "viewmodel.cpp")
s = io.open(p, encoding="utf-8").read()
n0 = len(s)

REPL = []

# ---- 枪托缩短：尾端原来伸到局部 z=+0.324，而 root 在 z=-0.262，
#      于是尾端正好卡在近裁剪面上，被透视放大成占屏高近一半的黑板。
REPL.append((
    "    part(root, box(0.048f, 0.070f, 0.200f), Vector3(0, -0.012f, 0.205f), Vector3(), poly);        // 枪托\n"
    "    part(root, box(0.052f, 0.095f, 0.028f), Vector3(0, -0.014f, 0.310f), Vector3(), poly);        // 托底板",
    "    part(root, box(0.048f, 0.070f, 0.130f), Vector3(0, -0.012f, 0.118f), Vector3(), poly);        // 枪托（缩短，尾端不进相机）"
))

# ---- 前臂：肘部再往外挪，让它尽快出画面，只在手腕附近露一小段
REPL.append((
    "    limb(root, Vector3(0.006f, -0.086f, 0.048f), Vector3(0.235f, -0.560f, 0.760f), 0.036f, sleeve);  // 右前臂",
    "    limb(root, Vector3(0.006f, -0.086f, 0.048f), Vector3(0.300f, -0.640f, 0.560f), 0.033f, sleeve);  // 右前臂"
))
REPL.append((
    "    limb(root, Vector3(-0.004f, -0.034f, -0.300f), Vector3(-0.210f, -0.620f, 0.520f), 0.036f, sleeve); // 左前臂",
    "    limb(root, Vector3(-0.004f, -0.034f, -0.300f), Vector3(-0.290f, -0.680f, 0.560f), 0.033f, sleeve); // 左前臂"
))

# ---- 材质：VM 关键灯 2.4 能量把袖子打得发白
REPL.append((
    "    const Ref<StandardMaterial3D> sleeve = vm_mat(Color(0.300f, 0.310f, 0.245f), 0.0f, 0.96f);",
    "    const Ref<StandardMaterial3D> sleeve = vm_mat(Color(0.235f, 0.243f, 0.188f), 0.0f, 0.96f);"
))
REPL.append((
    "    vmkey->set_param(Light3D::PARAM_ENERGY, 2.4f);",
    "    vmkey->set_param(Light3D::PARAM_ENERGY, 1.9f);"
))

# ---- 腰射位 / 开镜位：整枪前移，让枪托落到相机前方而不是贴着近裁剪面
OLD_POS = (
    "    const Vector3 hip(0.172f, -0.150f, -0.262f);\n"
    "    const Vector3 aim(0.000f, -0.084f, -0.196f);"
)
NEW_POS = "\n".join([
    "    // 位置是从投影反推的，不是拍脑袋：",
    "    //   枪托尾端 局部 z=+0.18  → 相机 z=-0.22（half_height≈0.178，托板 0.070 高只占屏高约 20%）",
    "    //   机匣     局部 z=-0.095 → 相机 z=-0.495（屏高占比约 10%）",
    "    //   枪口     局部 z=-0.700 → 相机 z=-1.10",
    "    // 于是整枪在屏幕上从右下（约 69%, 87%）斜向中心偏右（约 54%, 57%），就是肩上据枪的构图。",
    "    // 第一版 root 只放到 z=-0.262，枪托直接贴到近裁剪面，被透视放大成一块占屏高近一半的黑板。",
    "    const Vector3 hip(0.135f, -0.130f, -0.400f);",
    "    const Vector3 aim(0.000f, -0.076f, -0.330f);",
])
REPL.append((OLD_POS, NEW_POS))

for a, b in REPL:
    assert a in s, "锚点缺失: " + a[:70]
    s = s.replace(a, b, 1)

io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("视图模型标定完成 %d -> %d 字节" % (n0, len(s)))
