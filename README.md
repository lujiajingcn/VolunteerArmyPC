# VolunteerArmyPC

**断头谷公路 · 第一人称伏击战 —— 原生 PC 版**

网页版（单文件 HTML5：[`lujiajingcn/VolunteerArmy`](https://github.com/lujiajingcn/VolunteerArmy)）的 3D 原生重制。
**游戏逻辑逐函数移植、常数与判定顺序保持一致**，而渲染、输入、表现层全部重写：Godot 4.5 + C++（GDExtension），
编译成真正的桌面客户端，不再依赖浏览器。

---

| 腰射 | 开镜 | 夜战 |
|---|---|---|
| ![腰射](docs/screenshot-hipfire.png) | ![开镜](docs/screenshot-ads.png) | ![夜战](docs/screenshot-night.png) |

> 上图为引擎实际渲染输出（2560×1369 截图降采样），非概念图。
> 生成方式见 [开发工具](#开发工具)：`VA_CAPTURE` 定时截图 + `tools/downscale_png.py`。

---

## 与网页版的关系

同一个游戏，两套实现。三件事**刻意保持不变**：

1. **逻辑层逐函数移植** —— `src/sim/`（16 个文件 / 4261 行）是网页版 `stepOnce` 及其全部子系统
   （关卡、单位、战斗、指令解析、敌我 AI、流程）的 C++ 重写，常数、判定顺序、随机数算法
   （mulberry32）都照抄。天气也是逻辑层概念：`init_world()` 里首个 `RNG.next()` 决定
   `sunny / rain / night`，并且**参与玩法** —— 下雨能见度 ×0.82、夜间 ×0.62，同时受伤倍率上升。
2. **逻辑层不认识引擎** —— `src/sim/` **不 include 任何 Godot 头文件**
   （`grep -l godot src/sim/*` 无输出）。需要播声音、弹字幕、出提示的地方，一律通过 `SimEvents`
   的 8 个虚回调向外抛出。因此逻辑层可以脱离引擎单独跑，也随时换得动渲染后端。
3. **掩体的位置与半径不可改** —— `va_config.cpp` 里 69 个掩体参与视线阻挡（`segCircle`）、
   掩体减伤、AI 找掩体评分。要调整只能改"怎么画"，不能改"在哪、多大"。

## 技术栈

| 项 | 版本 / 说明 |
|---|---|
| 引擎 | Godot **4.5-stable**（Forward+ 渲染器，MSAA 4×，1920×1080 全屏） |
| 语言 | **C++17**（GDExtension 原生扩展） |
| 绑定 | [`godot-cpp`](https://github.com/godotengine/godot-cpp) 固定到 `godot-4.5-stable`（submodule，见 `.gitmodules`） |
| 构建 | CMake 3.17+ / Visual Studio 2022（x64） |
| 产物 | `bin/volunteer_army_pc.dll`（约 900KB） |

## 目录结构

```
VolunteerArmyPC/
├── src/
│   ├── sim/                  逻辑层：零引擎依赖，移植自网页版
│   │   ├── va_types.h            单位 / 载具 / 弹丸 / 指令 / 武器规格
│   │   ├── va_math.h             数值工具 + mulberry32 随机数 + 线段-圆相交
│   │   ├── va_config.*           关卡常数、69 个掩体、公路路径、呼号表、回复语
│   │   ├── va_utf8.h             UTF-8 码点解码（中文语音指令解析用）
│   │   ├── va_grammar.cpp        呼号 / 动作 / 方位语法
│   │   ├── va_parser.cpp         指令解析（三种变体）
│   │   ├── va_world.*            WorldState + 主循环 step_once + SimEvents 接口
│   │   ├── va_level.cpp          关卡初始化、天气抽取
│   │   ├── va_units.cpp          单位状态更新
│   │   ├── va_combat.cpp         弹道、伤害、压制、掩体减伤
│   │   ├── va_commands.cpp       玩家指令下发
│   │   ├── va_ai_ally.cpp        友军 AI
│   │   ├── va_ai_enemy.cpp       敌军 AI
│   │   └── va_flow.cpp           任务目标与关卡流程
│   ├── node/                 桥接层：唯一接触引擎的部分
│   │   ├── world_sim.*           Node3D 主体：输入 → step_once → 同步场景节点
│   │   ├── scene_builder.*       程序化建场景：地形、天空、光照、大气、掩体、士兵、载具
│   │   └── viewmodel.*           第一人称武器视图模型（sway / 后坐 / 呼吸 / 开镜 / 换弹）
│   └── register_types.cpp
├── scenes/Main.tscn          主场景（Node3D "Main" + WorldSim）
├── tools/                    开发工具（截图取证 / PNG 分析 / 环境搭建）
├── docs/                     README 配图
├── ext/godot-cpp/            submodule：C++ 绑定，固定在 godot-4.5-stable
├── sdk/godot/                本地 Godot 编辑器（**不入库**，需自行放置）
└── build/  bin/              构建目录与产物（**不入库**，需自行构建）
```

## 构建

### 前置条件

- **Visual Studio 2022**，勾选「使用 C++ 的桌面开发」
- **CMake** ≥ 3.17
- **Python 3**（godot-cpp 生成绑定时要用，需在 `PATH` 里）
- **Godot 4.5-stable 编辑器**，从 [官方 Release](https://github.com/godotengine/godot/releases/tag/4.5-stable)
  下载后放进 `sdk/godot/`，需要这两个文件：
  ```
  sdk/godot/Godot_v4.5-stable_win64.exe
  sdk/godot/Godot_v4.5-stable_win64_console.exe
  ```

### 步骤

```bash
# 1) 克隆。必须带 --recursive —— godot-cpp 是 submodule，
#    漏了它 CMake 会在 add_subdirectory(ext/godot-cpp) 处报目录不存在。
git clone --recursive https://github.com/lujiajingcn/VolunteerArmyPC.git
cd VolunteerArmyPC

# 2) 配置（生成器必须与 VS 2022 匹配；x64 不能省）
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# 3) 构建
cmake --build build --config Release --parallel

# 4) 运行
sdk\godot\Godot_v4.5-stable_win64.exe --path .
```

构建产物固定输出到 `bin/volunteer_army_pc.dll`，文件名由 `VolunteerArmyPC.gdextension` 引用，**不要改名**。

> **关于 `.gdignore`**：仓库里 `sdk/.gdignore`、`build/.gdignore`、`captures/.gdignore`、`docs/.gdignore`、
> `tools/.gdignore` 都是**故意保留并入库**的空文件（这也正是 `.gitignore` 里写成
> `sdk/*` + `!sdk/.gdignore` 而不是 `sdk/` 的原因）。
> 它们让 Godot 不去扫描这些目录 —— 否则首次打开项目时，Godot 会把 `build/` 里 1000+ 个 `.obj`
> 当成 3D 模型逐个导入，卡死数分钟并污染 `.godot/` 缓存。删掉它们会立刻复现这个问题。

## 架构：逻辑层不认识引擎

```
输入（键鼠 / 语音） ──┐
                      ▼
              ┌───────────────┐
              │  src/sim/     │   纯 C++。不 include 任何 godot 头文件。
              │  step_once()  │   持有 WorldState：单位、载具、弹丸、天气、任务进度。
              └───────┬───────┘
                      │  SimEvents（8 个虚回调）
                      ▼
              ┌───────────────┐
              │  src/node/    │   把逻辑状态翻译成场景节点：
              │  world_sim    │   位置 / 朝向 / 动画 / 材质 / 光照 / 视图模型
              └───────────────┘
```

`src/node/world_sim.cpp` 每帧只做三件事：把输入推进 `va::IN` → 调 `va::step_once(dt)` →
按逻辑状态更新场景节点。**它从不反向写逻辑状态** —— 所以任何"画面问题"都不可能污染玩法。

## 实现要点（都是踩过的坑，改代码前建议先读）

- **颜色空间**：Godot 的 `Environment` 与材质颜色在**线性空间**，而选色直觉是 sRGB。
  把网页版的色值原样搬进线性字段会平白亮一大截（0.62 sRGB 当线性用 ≈ 屏幕 0.81）。
  外观色一律走 `scene_builder.cpp` 的 `SRGB()` 包装（内部调 `Color::srgb_to_linear()`）。

- **布光必须跟着天气走**：`apply_weather()` 按 `W.weather` 一次性重打太阳角度/色温、天空、
  雾、体积雾、曝光、饱和、对比、辉光。**漏调它会残留 Godot 默认的 `fog_density = 0.01`**
  （比晴天配方浓 3 倍多），整屏被一层奶白盖住 —— 而且因为所有区域亮度趋同，极易被误判成
  "光照太平"，让人跑去反复调光源（实测就是这么绕了一大圈）。改动环境后**务必先确认 `apply_weather()` 被调用**。

- **视图模型必须独立打光**：枪模单独占一个渲染层（`set_layer_mask(1u<<1)`），配几盏
  `set_cull_mask(VM_LAYER)` 的平行光，这样玩家任意转向，枪身明暗都稳定可读，又不会照亮身边的石头。
  其中**沿视轴的兜底光是必需的** —— 前臂圆柱的可见面法线几乎全朝向相机，只给侧向光时
  那些像素照度恒为 0（把 albedo 提高 2 倍都不会变亮）。

- **视图模型姿态用投影反推，不要试数**：先算"枪要在屏幕上落在哪几个点"，再反推几何。
  另有硬约束：**枪上任何顶点都不能落到相机后方**，否则会被近裁剪面切开、透视放大成占屏近一半的木板。

- **坐标系映射**：逻辑层 2D `(x, y)` → Godot `(x * S, 高度, y * S)`，其中 `S = 0.05`
  （1 世界单位 = 0.05 米，即 1 米 = 20 单位）。公路宽 140 ≈ 7 米，眼高 1.65 米。

- **模型旋转约定**：逻辑层「模型前方 = +X」→ Godot 绕 Y 轴旋转角 = `-facing`；
  相机 `yaw = -facing - π/2`。

## 开发期旋钮

全部靠环境变量控制，**无需改代码、无需重新编译**。这在做视觉 A/B 对照时极其省时间
（本轮标定枪模光照时靠它扫了 6 档能量，只编译过一次）。

| 旋钮 | 作用 |
|---|---|
| `VA_SEED` | 固定随机种子（同时决定天气与敌军部署）。不设则用系统 tick |
| `VA_TRACE` | 打开逻辑层追踪输出 |
| `VA_FOV` | 世界相机视场角（默认 65，垂直） |
| `VA_ADS` | 强制进入开镜状态（截图取证用） |
| `VA_CAPTURE` / `VA_CAPTURE_DIR` | 到指定战局秒数自动截图，全部拍完自动退出 |
| `VA_TONEMAP` | 色调映射：`aces`（默认）/ `agx` / `filmic` / `reinhardt` |
| `VA_EXPOSURE` `VA_SUN` `VA_FILL` `VA_BOUNCE` `VA_AMBIENT` `VA_FOG` `VA_LUT` | 曝光 / 主光 / 补光 / 弹光 / 环景光 / 雾 / 色调分级 LUT 的倍率（默认 1.0 = 完全按天气配方） |
| `VA_VMK` `VA_VMF` | 枪模关键灯 / 侧补光的能量倍率 |
| `VA_VM_HIP` `VA_VM_AIM` `VA_VM_ROT` | 覆盖枪模腰射位 / 开镜位 / 姿态角 |
| `VA_VM_HIDE` | 隐藏枪模（做"有枪 / 无枪"消融对照） |
| `VA_VM_MAT` | 给枪模各部件上识别色（红=枪身 绿=手套 蓝=袖子 白=金属），用于定位几何体 |

例：固定种子拍夜战

```bash
VA_SEED=3 sdk/godot/Godot_v4.5-stable_win64.exe --path .
```

## 开发工具

`tools/` 下的脚本都是零依赖的纯 Python（不依赖 PIL / numpy，自己实现 PNG 编解码）：

| 脚本 | 用途 |
|---|---|
| `capture.sh` | 无人值守截图取证：设好 `VA_CAPTURE` 后启动引擎，到点存图并自动退出 |
| `probe_png.py` | 客观量测画面：分区域报平均 RGB、亮度、饱和度、过曝率（判断"画面发灰"必须靠数据，不能靠肉眼） |
| `crop_png.py` | 裁剪 + 整数放大 + 叠加 NDC 网格，让"某物在屏幕的哪一行哪一列"直接读成数字 |
| `vm_eval.py` | 按颜色掩码提取枪模各部件像素，统计亮度分布（死黑 / 正常 / 过曝占比） |
| `diff_png.py` | 两图逐像素求差，定位某个物体实际占据的屏幕区域 |
| `downscale_png.py` | 盒式降采样，生成 README 配图 |
| `setup_env.py` | 环境搭建辅助 |
| `patch_*.py` | 开发过程中的一次性补丁脚本（已全部应用完毕，保留仅作历史记录，**不要重复执行**） |

## 当前进度

**已完成**

- 逻辑层 100% 移植（16 文件 / 4261 行），逻辑层与引擎彻底解耦
- 程序化场景：地形铺展 700m、公路与沥青贴图、草地贴图、远景两圈山脊、程序化云层
- 光照与大气：三套天气配方、ACES 色调映射、三点布光 + 环景光 + SSAO/SSIL、体积雾、色调分级 3D LUT
- 掩体建模：岩石（低模不规则多面体）、树（高干 + 分枝 + 多球簇树冠）、灌木
- 第一人称武器视图模型：投影反推标定姿态、五盏独立打光、导轨齿/散热孔/抛壳口等分件、红点瞄具、换弹动画
- 可复现取证链路：固定种子 + 定时截图 + 客观像素量测

**待办**

- 士兵与载具模型精致化（目前仍是块状体，是战场画面的主体）
- 特效层：曳光弹、爆炸、弹壳抛出、烟尘
- 完整 HUD：任务提示、雷达、小队状态、弹药、字幕、受伤反馈（目前仅 4 个基础 Label）
- 音频层（网页版有 62 个程序化音效 id，PC 版尚未接入）
- 语音指挥：计划走 **SAPI 识别 + TTS 回话 + 面板兜底**
- 玩法平衡调整

## 许可

个人项目，未附许可协议。
