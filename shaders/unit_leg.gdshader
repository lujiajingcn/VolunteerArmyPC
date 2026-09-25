shader_type spatial;
/* ============================================================================
   VolunteerArmyPC —— 角色「双腿交替」顶点位移（见 src/node/unit_leg.h 的完整设计说明）

   为什么是着色器而不是骨骼：11 个角色 glb 全是**单块融合网格**
   （nodes=1 meshes=1 prims=1 skins=0 joints=0 anims=0），没有骨架可驱动。
   但实测左右腿在几何上**本来就是分离的两簇** ——
   腿区间（身高下 35~45%）跨中线三角形 = 0 个，中轴 ±2%身高内几乎没有顶点。
   所以"按 sign(x) 分左右腿"是可靠的，不需要切开网格。

   ⚠️ 坐标口径（这里最容易错，错一次所有符号全反）
   顶点是 Blender 的 Z-up 数据，节点上才带 +90°X 旋转。所以 **VERTEX 在着色器里看到的是**：
       X = 左右（±）
       Y = 前后（+Y 是模型正面）
       Z = 高度，但 **-Z 朝上** —— z = 0 是脚，z = -H 是头顶（H = 包围盒高 ≈ 1.05~1.09）
   ⇒ "在髋以下" 的判据是 `VERTEX.z > hip_z`（**大于**，不是小于）。
      这个符号关系与直觉相反，是实测出来的，不要凭常识改。

   ⚠️ 两条写这个文件时踩过的 Godot 着色器语言规则（都会让编译直接失败）：
     1) **PI / TAU / E 是内置常量**，自己再 `const float PI = ...` 会报
        "Redefinition of 'PI'"。
     2) **`const` 只能由编译期常量初始化**，拿 uniform 去初始化会报
        "Expected constant expression" —— 所以下面函数体里一个 `const` 都没有。
       （这两条都只在着色器**真正被编译**时才暴露，看代码看不出来。
         快速自查的办法：`--headless --script` 里 load 一次本文件。）

   与表现层其余部分的分工：整体跑动节奏（起伏/前倾/侧摇/肩摆）在 UnitAnim 里，
   作用于单位节点的变换；本着色器只管**腿部局部形变**，两者叠加。
   ========================================================================= */

render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

// ---- 原 glTF 材质的三张贴图（由 C++ 从 StandardMaterial3D 搬过来）----------
uniform sampler2D albedo_tex : source_color;
// glTF 的 metallicRoughness 纹理：G = roughness，B = metallic。
// 与 Godot 的 ORM 约定同布局，所以可以直接按通道取，不需要重排。
uniform sampler2D orm_tex;
uniform sampler2D normal_tex : hint_normal;

// ---- 材质级旋钮（每个角色键一份材质，值在 setup 时定）---------------------
// 小腿额外弯曲（度）。0 = 不做（第一版默认），走路时膝盖收腿是加分项，不是判据。
uniform float knee_deg = 0.0;
// 膝高（mesh 单位 z）。沿用与 hip 同一套"z 越小越高"的口径。
uniform float knee_z = -1e9;
// 染色探针：把"受腿摆影响的程度"直接画成颜色，用来**验证口径**而不是看效果。
// 髋以上保留参照色，髋以下是彩色：
//     左腿 = 黑→亮绿，右腿 = 黑→亮红，亮度 = 权重 w
// 判读：彩色区必须**只出现在两条腿上**；若头顶/胳膊上色，说明 hip_z 或符号错了。
// 诊断染色。0 = 关（正常渲染）；1 = 把"受腿摆影响的程度"画成颜色（验髋高与左右分界）；
// 2 = 把**每实例参数本身**画成颜色（验 instance uniform 有没有逐实例送达）。
// mode 2 是为了回答一个用别的办法很难回答的问题：
//   跑动排 6 格的相位各不相同，如果 6 格颜色各异 ⇒ 逐实例参数生效；
//   若 6 格同色 ⇒ 值根本没送到，问题在传递链上，而不在几何/角度公式上。
uniform int leg_probe = 0;

/* ---- 每实例参数：与单位节点一一对应，逐帧由 C++ 写入 ------------------------
   x = 相位（弧度）
   y = 大腿摆角幅度（度，**已乘好该单位的幅度包络**）
   z = 髋高 hip_z（mesh 单位）
   w = 过渡带长度（mesh 单位）
   静止的单位传 y=0，此时整段形变退化为恒等，不会有任何抖动。 */
instance uniform vec4 u_leg = vec4(0.0, 0.0, -0.5, 0.16);

varying float v_w;      // 顶点权重 0..1（0 = 髋及以上，1 = 完全跟随腿）
varying float v_side;   // -1 = 左，+1 = 右

/* 绕平行于 X 轴、过 (·, 0, pivot_z) 的轴旋转 (y, z)，按 +X 右手方向。
   写成"取 vec2 返回 vec2"而不是 inout 参数：没有副作用、也不可能踩到
   "对 VERTEX.y 这种分量用 inout"的语法限制，调用点反而更短。
   法线用同一套旋转（pivot 传 0）：不转的话光照会"焊死"在原始朝向上，
   腿摆起来看着像贴图在表面滑动。 */
vec2 spin_x(vec2 yz, float pivot_z, float ang) {
	float c = cos(ang);
	float s = sin(ang);
	float dy = yz.x;
	float dz = yz.y - pivot_z;
	return vec2(dy * c - dz * s, pivot_z + dy * s + dz * c);
}

void vertex() {
	v_w = 0.0;
	v_side = 1.0;

	// 注意：这里**不能**用 const —— hip_z / band 来自 uniform，
	// 而 const 要求编译期常量。写成 const 会直接编译失败。
	float hip_z = u_leg.z;
	float band = max(u_leg.w, 1e-4);

	// ⚠️ -Z 朝上 ⇒ z 比髋大 = 在髋以下。符号别改。
	float below = VERTEX.z - hip_z;
	if (below > 0.0) {
		// 权重只在**髋下方 band 这一段**内由 0 升到 1：
		// 髋处权重 0（不位移）⇒ 与上半身天然连续，不会裂开；
		// band 以下是刚性旋转（走路时腿是刚体，不该像橡皮筋）。
		float w = clamp(below / band, 0.0, 1.0);
		w = w * w * (3.0 - 2.0 * w);          // smoothstep：过渡更柔和

		float side = (VERTEX.x >= 0.0) ? 1.0 : -1.0;
		// 左右相位差 π ⇒ **天然交替**：一条往前时另一条正在往后。
		float ph = u_leg.x + (side < 0.0 ? PI : 0.0);
		// 角度乘 w：位移在髋部连续归零。这是简化版 LBS，
		// 代价是过渡带内腿会被轻微拉伸 —— 换取的是任何情况下都不会撕裂。
		float ang = radians(u_leg.y) * w * sin(ph);

		vec2 rv = spin_x(vec2(VERTEX.y, VERTEX.z), hip_z, ang);
		VERTEX.y = rv.x;
		VERTEX.z = rv.y;
		vec2 rn = spin_x(vec2(NORMAL.y, NORMAL.z), 0.0, ang);
		NORMAL.y = rn.x;
		NORMAL.z = rn.y;

		if (knee_deg != 0.0 && VERTEX.z > knee_z) {
			// 小腿再补一次弯曲。只在"往后摆"的半个周期里收腿（max(0,-sin)），
			// 否则会变成膝盖反关节。
			float kb = clamp((VERTEX.z - knee_z) / max(band * 0.6, 1e-4), 0.0, 1.0);
			float ks = max(0.0, -sin(ph));
			float kk = radians(knee_deg) * (kb * kb * (3.0 - 2.0 * kb)) * ks;
			vec2 kv = spin_x(vec2(VERTEX.y, VERTEX.z), knee_z, kk);
			VERTEX.y = kv.x;
			VERTEX.z = kv.y;
			vec2 kn = spin_x(vec2(NORMAL.y, NORMAL.z), 0.0, kk);
			NORMAL.y = kn.x;
			NORMAL.z = kn.y;
		}

		v_w = w;
		v_side = side;
	}
}

void fragment() {
	vec4 base = texture(albedo_tex, UV);
	ALBEDO = base.rgb;

	vec3 orm = texture(orm_tex, UV).rgb;
	ROUGHNESS = clamp(orm.g, 0.0, 1.0);
	METALLIC = clamp(orm.b, 0.0, 1.0);

	// 与 StandardMaterial3D 的默认口径一致：用第一套 UV 当法线贴图的 UV。
	NORMAL_MAP = texture(normal_tex, UV).rgb;
	NORMAL_MAP_DEPTH = 1.0;

	if (leg_probe == 2) {
		// 每实例参数可视化：R = 相位（0..2π → 0..1），G = 摆角/60，B = 髋高归一化。
		// 三格同相位才会同色 —— 6 格跑动排里相位是均分的，颜色必须**逐格不同**。
		ALBEDO = vec3(fract(u_leg.x / 6.2831853),
		              clamp(u_leg.y / 60.0, 0.0, 1.0),
		              clamp((u_leg.z + 1.0) * 2.0, 0.0, 1.0));
		ROUGHNESS = 0.9;
		METALLIC = 0.0;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);
	} else if (leg_probe == 1) {
		if (v_w > 0.0) {
			// 左腿绿、右腿红；亮度 = 权重，于是"过渡带在哪、多宽"直接可读。
			float k = 0.12 + 0.88 * v_w;
			ALBEDO = (v_side < 0.0) ? vec3(0.06, k, 0.10) : vec3(k, 0.06, 0.06);
		} else {
			ALBEDO = vec3(0.35, 0.35, 0.38);   // 髋以上：中性灰，当参照系
		}
		ROUGHNESS = 0.9;
		METALLIC = 0.0;
		NORMAL_MAP = vec3(0.5, 0.5, 1.0);      // 探针下关掉法线细节，免得干扰判读
	}
}
