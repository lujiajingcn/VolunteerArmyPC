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
│   │   ├── hud.*                 全套 COD 风格 HUD + 界面外壳（主菜单 / 简报 / 结算），全手绘
│   │   └── viewmodel.*           第一人称武器视图模型（sway / 后坐 / 呼吸 / 开镜 / 换弹）
│   └── register_types.cpp
├── assets/art/               美术素材
│   ├── art_*.png                 任务素材（主菜单 / 简报 / 区域态势 / 结算两张）
│   └── char/                     角色素材（见「角色形象」一节）
│       ├── char_<键>.png             清理版全身立绘（图生3D 与胸像裁切的输入）
│       ├── portrait/char_<键>.png    256×332 胸像（简报名册用）
│       └── model/char_<键>.glb       三维模型（运行时 GLTFDocument 载入）
├── scenes/Main.tscn          主场景（Node3D "Main" + WorldSim）
├── tools/                    开发工具（截图取证 / PNG 分析 / 素材预处理 / 3D 生成）
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

### 在 VS 2022 里运行 / 调试（F5）

打开 `build/VolunteerArmyPC.sln` 后直接按 F5 会弹：

> 无法启动程序 …\bin\volunteer_army_pc.dll。… 不是有效的 Win32 应用程序。

**这是正常的，不是文件损坏、也不是架构不对。** 本工程的目标是一个 SHARED 库（GDExtension），
它本身不是可执行程序；而 VS 在没有"调试器命令"时，会退而拿**目标的输出文件**去启动 ——
`CreateProcess` 只能启动 EXE，喂给它一个 DLL，报的就是这句话。

CMake 里已经配好了正确的调试方式（`VS_DEBUGGER_COMMAND`），F5 会变成：

```
sdk/godot/Godot_v4.5-stable_win64.exe  --path <工程根> --log-file <工程根>/debug_run.log
```

引擎加载 `bin/volunteer_army_pc.dll`，**断点直接落在我们自己的源码里** ——
GDExtension 没有独立的宿主 EXE，这也是它唯一的本地调试方式。

三个容易踩的点：

| 点 | 说明 |
|---|---|
| 不能指向 `*_console.exe` | 它是 197 KB 的**外壳**（只负责 fork 主 exe 并挂控制台），VS 会附到外壳上，而外壳里没加载我们的 dll → 断点永远绑不上。自动挑选时已排除 |
| 必须有 `--log-file` | 主 exe 是 GUI 子系统，**stdout 无人接收**。不加这个参数，`print` 与 Godot 自己的报错全部丢掉，而本项目的回归判据恰恰是"日志里有没有 ERROR" |
| 想要断点就得选有符号的配置 | `Release` 没有 `/Zi`，能跑但断点打不上。切 `RelWithDebInfo` / `Debug`（首次会连 godot-cpp 一起按该配置重建，耗时是正常的） |

改了 `CMakeLists.txt` 后要**重新生成一次工程**才会写进 `.vcxproj`（VS 会提示"重新加载项目"）：

```bash
cmake -S . -B build -DVA_DEBUG_ENV="VA_SCRIPT_A=1;VA_FF=6"   # 可选：F5 时注入取证旋钮
```

`VA_DEBUG_ENV` 用分号分隔、写进 `<LocalDebuggerEnvironment>`；不设就是空。
不重新生成、只想临时改，也可以走 VS 的「项目属性 → 调试 → 命令 / 命令参数 / 环境」。

### 导出可执行程序（Windows）

```bash
# 前置：本机要有 Godot 4.5 的导出模板（见下"模板从哪来"）
sdk/godot/Godot_v4.5-stable_win64_console.exe --headless --path . \
    --export-release "Windows Desktop" dist/VolunteerArmyPC.exe
```

产物是 `dist/` 下的**两个文件，一起拷走就能玩**：

| 文件 | 说明 |
|---|---|
| `VolunteerArmyPC.exe` | 约 165 MB。导出模板（92 MB）+ 内嵌 pck（`binary_format/embed_pck=true`） |
| `volunteer_army_pc.dll` | GDExtension 库。Godot **不会**把 dll 嵌进 exe，必须与 exe 同目录 |

导出配置 `export_presets.cfg` 已入库，其中 `exclude_filter` 排掉了 `ext/`（godot-cpp 几万文件）、
`sweep/`、`captures/`、`docs/`、`tools/`、`build/` —— 不排的话 pck 会被撑爆。

**模板从哪来。** 导出模板不在仓库里（整包 **1294 MB**），而本机 GitHub release 资产
（走 `objects.githubusercontent.com`）实测 45 秒零字节、完全不通。可用的做法是
**只取需要的那一个条目**：ZIP 的中央目录在文件末尾，配合 HTTP Range 就能先读目录、
再精确拉 `templates/windows_release_x86_64.exe` 的数据段（压缩 33.8 MB，占整包 2.6%），
inflate 后放进 `%APPDATA%\Godot\export_templates\4.5.stable\`。

```bash
python tools/fetch_export_template.py --list   # 先看远端有什么
python tools/fetch_export_template.py          # 下载并装 Windows Release 模板
```

三个坑都写进了脚本注释，这里只点最要紧的：**下载要交给 curl，别用 Python 的 urllib** ——
同一偏移同一镜像，curl 稳定 206 + 精确字节数，urllib 会把整个 1.29 GB 吞进内存
（进程涨到 1.7 GB 且永不返回）。另外加速站要**横向测速**：本机实测
`gh.xxooo.cf` 2.5 MB/s、`gitproxy.mrhjx.cn` 1.8 MB/s，而 `ghproxy.net` 只有 88 KB/s、
`gh-proxy.com` 干脆忽略 Range（任何分段请求都回 200 + 全量）。

### 离线跑逻辑层（`va_sweep`）

CMake 里还有一个**不链接引擎**的目标 `va_sweep`：把 `src/sim/*` 直接编进一个控制台程序，
不开窗口、不走渲染、连 godot-cpp 都不需要。它是「逻辑层可以脱离引擎单独跑」这句话的
**可执行证据** —— 在这之前，README 一直这么写着，但仓库里没有任何东西证明过它。

它也是平衡数据的来源：改一个常数重编一次，就能把 6 套方案 × 10 个种子跑完
（真跑一局是 620 秒模拟时间，手工比十局根本不现实）。

```bash
cmake --build build --config Release --target va_sweep
sweep/sim/va_sweep.exe            # 全部对照方案 × 10 种子
sweep/sim/va_sweep.exe base       # 只跑名字以 base 开头的方案
sweep/sim/va_sweep.exe -v         # 逐局明细（活/亡/倒、坦克、箱、撤离、天气）
```

两个容易踩的点，都写在源文件注释里：① 逻辑层里 `va_flow` 是**直接解引用** `EV` 的
（不像 `va_world` 的 `say/sfx` 走 `ev()` 兜底），无头程序必须 `set_events()`，
否则当场段错误；② 输出要 `setvbuf(..., _IONBF, 0)`，不然崩溃一次就把整轮输出吞掉。

源清单是从 `VA_SOURCES` 里按 `src/sim/` 前缀筛出来的，**不维护第二份清单** ——
两份清单一定会漂。产物落在 `sweep/sim/`（已被 `.gitignore` 忽略）。

> **关于 `.gdignore`**：仓库里 `sdk/.gdignore`、`build/.gdignore`、`captures/.gdignore`、`docs/.gdignore`、
> `tools/.gdignore`、`sweep/.gdignore` 都是**故意保留并入库**的空文件（这也正是 `.gitignore` 里写成
> `sdk/*` + `!sdk/.gdignore` 而不是 `sdk/` 的原因）。
>
> `sweep/` 那条是补上的：它里面存着 `gen3d/char_*.raw.glb`（图生3D 的原始包，**每个 29MB**）。
> 少了这个文件，`godot --import` 会把它们当模型逐个导入 —— 实测一次导入从 7 分钟降到约 20 秒，
> 差值基本就是这 10 个原始包。取证截图也放在 `sweep/` 下，同样不该被当成工程素材。
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

- **第一人称不渲染自己的身体**：逻辑层里玩家也是一个普通单位（`W.units[0]`，`isPlayer`），
  于是 `sync_entity_nodes` 一开始给包括玩家在内的每个人都建了节点、每帧摆到 `(x, y)` ——
  而**相机就在同一个 `(x, y)` 上、高 1.65 m**，角色模型归一化后高 1.68 m，
  镜头正好落在自己模型的**头里**。症状是"用户视野有大片遮挡"：一大块贴脸的深色面，
  天空只剩左上角一条缝。实测遮挡量 = **44.6% 的像素**（同种子、同帧，
  `VA_SHOW_SELF=1` 与默认各拍一张，`tools/diff_png.py` 逐像素相减）。
  现由 `sync_entity_nodes` 显式跳过玩家自己（`VA_SHOW_SELF=1` 可放回来复现）。
  三点值得记住：
  - 它是**换成三维模型之后**才出现的 —— 之前的图元士兵是个矮块，镜头在它上面，
    所以旧截图（`docs/screenshot-hipfire.png`）里没有这块遮挡，可以直接当"没有遮挡"的基线。
  - 排除"其实是出生点旁边 1~2 米的队友挡的"这个很像的解释，靠的是 `VA_DBG_UNITS`：
    打出来是 `#0 char_leader d=0.00 m self=1`，最近的队友在 3.78 m 之外。
    **一个 1.68 m 的模型在 2 m 处就占满 66% 画高**，所以"贴脸的一大块"必须落到具体对象上才算查清。
  - 检阅台（`VA_UNIT_SHOW`）早就把 `refs_.units` 整个藏掉了，注释里写的理由正是
    "真单位用的是同一批模型，又正好冻在出生点（大多就在玩家身边几米内）" ——
    同一个根因，那里是靠连队友一起藏才绕过去的。

- **"按住不放"与"松开"是两回事：按键重复不是 key-up**。逻辑层只有"键现在按不按着"
  一个概念（`va::IN` 里全是 bool），而引擎侧的事件有三类：按下 / **系统按键重复** / 松开。
  重复事件（`InputEventKey::is_echo()`）的 `is_pressed()` **仍然是 1**，它说的是
  "键还按着"。原来那行 `const bool down = k->is_pressed() && !k->is_echo();`
  把它当成了松开，于是**按住方向键只走 0.53 秒**（Windows 默认重复延迟 500ms），
  之后一直到手指真的抬起来都不动 —— 表现为"按着没反应、只能反复点"。
  实测（`tools/inject_key.py` 注入 + `VA_DBG_INPUT` 逐事件打点）：

  ```
  6.4s pressed=1 echo=0 → IN.w=1     随后两条心跳位移 11.733 / 23.467 m
  ≈7.0s pressed=1 echo=1 → IN.w=0     ← 被自己的重复事件按停
  7.0~8.4s 共 30 条 echo=1 → 位移 0.0 m
  ```

  现在 echo 不写任何状态；**只在"闩锁刚被清过"时**才用它把"键其实还按着"补回来 ——
  那种情况下真正的 press 事件压根没送到过，echo 是唯一的线索。
  开关类键（R 换弹 / G 手雷 / F 烟雾 / Z 标记）**绝不允许**被 echo 重建：
  多补一次按下等于多打一枪、多丢一颗雷（`is_hold_key()` 划这条线）。

- **丢 keyup 的三条路径必须一起堵**（否则症状正好相反：**角色自己一直往那个方向走**）：
  ① **窗口失焦** —— Windows 把 keyup 送给了抢走焦点的那个窗口，本进程永远收不到；
  ② **界面外壳** —— `_input` 在外壳期间整条 return，release 也一起被吃掉
  （战斗里按住 W 再进菜单松手，`IN.w` 会永远停在 true）；
  ③ 失焦通知万一来不到 —— `_process` 每帧核对一次窗口焦点兜底。
  三条都归到 `clear_held_input()`，`p_reacquire_ok` 区分它们：
  失焦后允许 echo 把键态补回来（键可能真的还按着），**进菜单则不允许**
  （菜单里的 W 不能变成"一进场就自己往前走"）。
  失焦那条的实测很干净：事件表里**只有一条 keydown、一条 keyup 都没有**，
  闩锁在失焦那一刻归零，之后 60 多条心跳位移全是 `0.0 m`。

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

- **日志里的中文必须走 `String::utf8()`**：godot-cpp 的 `String(const char *)` 绑定的是
  引擎的窄字符构造，那是按 **Latin-1 逐字节取码点**的。源文件是 UTF-8，"取证" 的
  `E5 8F 96` 会被读成三个 Latin-1 字符，写进日志再按 UTF-8 编码 → `C3A5 C28F C296`，
  也就是**二次编码**的乱码：`åè¯æ³¨å`。只有 `String::utf8()` 才按 UTF-8 解释。

  ```cpp
  // 错：日志里是 åè¯æ³¨å¥
  UtilityFunctions::print("[combat-ev] 取证注入：自动战斗=", autoplay_ ? "开" : "关");
  // 对
  UtilityFunctions::print(String::utf8("[combat-ev] 取证注入：自动战斗="),
                          autoplay_ ? String::utf8("开") : String::utf8("关"));
  ```

  编译期完全不报错，只能靠 `tools/check_log_encoding.py` 兜住
  （扫 `src/**/*.cpp` 的日志调用参数表，有发现退出码 1）：

  ```bash
  python tools/check_log_encoding.py
  ```

  注意它只管**日志调用**。别处的非 ASCII 窄字面量是合法的、不能乱包 ——
  例如 `va::W.overKind = "成功"` 是赋给 `std::string` 的字节透传，包了类型都不对。

- **诊断日志要看编码**：同一条日志在三种输出通道下字节可能不同，
  别把"乱码"当成引擎 bug。判定方法是对原始字节做十六进制看：
  `取` 的正确 UTF-8 是 `e5 8f 96`，二次编码则是 `c3 a5 c2 8f c2 96`。

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
| `VA_SCREEN` | 指定初始屏 `menu` / `brief` / `play`（优先级高于"取证模式自动跳过前奏"，否则新屏永远拍不到） |
| `VA_SKIP_MENU` | 跳过主菜单直接开打 |
| `VA_END` | 把战局按指定结局收尾 `win` / `lose`（给结算面板取证，不必真跑满一局） |
| `VA_UNIT_SHOW` | **角色模型检阅台**：`1`/`row` 陈列排、`one:<键>[:<度>]` 近景单体（见下） |
| `VA_AUTO` | **自动战斗**（取证/回归）：每帧朝最近活敌对准并按住开火。命中的判定、`hits`/`kills` 自增、伤害与死亡全走真实战斗代码 —— 它顶替的是玩家的手，不是 HUD 的状态 |
| `VA_SCRIPT_A` | **按离线扫描同一套剧本发口令**：`全体，隐蔽` → `老白，起爆`（先头车 `x < 1180`，按位置）→ `全体，开火` → … → `全体，撤离`。它是 `VA_AUTO` 的**补充而不是替代**：`VA_AUTO` 只顶替玩家的手，而**伏击是由"第一枪"触发的**，车队不进射界就永远没有第一枪 —— 整局会以「车队冲过了西侧出口，伏击失败」收场（实测踩到过，那时画面上一条命中都没有，看着像命中判定坏了） |
| `VA_DOWN_AT` | 到指定战局秒数让玩家被**真实伤害**击倒（走 `damage_unit` → `down_player`，不是把 `downed` 置 true） |
| `VA_FF` | **快进倍率**（默认 1）。逻辑层按真实经过时间推进，而车队要到 `CFG.convoyIn = 175` 秒才进地图 —— 不打快进的话，"打到交火"的取证跑图就是 5 分钟真实时间起步。它只放大"喂进来的时间"，步长仍是固定 1/60，所以同一战局秒数下的状态与不快进时一致。**但标称倍率在 1080p 下会被削**：每帧最多跑 `8 × 倍率` 个固定步（= 0.8 秒仿真/帧），帧率跟不上时就地触顶 —— 报告里别把它当实测值写 |
| `VA_CAPTURE_EV` | **战斗事件当帧自动落盘**：命中标记只亮 0.24 秒，定时截图撞不上，改由事件自己声明"该留证据了"。落盘时机是**等 0.05 秒墙钟 + 至少跨 1 帧**（不是"隔 N 帧"），理由与判读方法见下节 |
| `VA_HUD_EV` | 把命中/击杀/队友阵亡/倒地的**触发时刻**打成 `[hud-ev]` 日志（把"事件什么时候发生"变成可读的数字） |
| `VA_MODEL_YAW` | 覆盖三维模型的朝向校正角（默认 90°），现场调朝向用 |
| `VA_HIDE_HUD` `VA_HIDE_PROPS` `VA_HIDE_UNITS` `VA_HIDE_VEH` | 分层消融：判定"画面上这块到底属于谁"（**归属问题一律用消融答，不用眼睛答**） |
| `VA_DBG_UNITS` | 把"镜头最近的 6 个单位"打成数字（编号 / 模型键 / **水平距离** / 是否在画 / 是不是自己）。消融只答到"属于哪一层"，它才能答到"具体是哪一个"—— 见「第一人称不渲染自己的身体」 |
| `VA_SHOW_SELF` | **把玩家自己的身体画回来**。这不是画面风格开关，是**复现"视野被大片遮挡"**用的（放回来 = 镜头被自己的模型包住） |
| `VA_DBG_INPUT` | **逐事件打印按键流**（键码 / `pressed` / `echo` → 写进 `IN` 的结果）**+ 每 0.25 秒墙钟一次位置心跳**（含两次心跳之间的**位移**）。输入手感问题只看画面判不了："走得慢"和"走两步就停"在屏幕上读不出量级。判据两条：`echo=1` 那行之后的 `IN(w,s,a,d)` 必须**不变**；`位移=0.0 m` 只能出现在 release 之后 |
| `VA_NO_AO` / `VA_NO_SHADOW` | 关掉环境光遮蔽 / 阴影，量化各层对画面的贡献 |
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

## 玩法平衡（数字是离线扫描量出来的，不是感觉出来的）

**起点**：网页版记录的是「10 种子胜率 4/10，车顶机枪占我方伤亡 33%–59%」。
把量测搬进 `va_sweep` 后第一步不是调参，而是**先验证基线**：
`base`（= 改动前的原值）跑出 **3/10**、车顶机枪占我方伤亡 **66%** ——
比网页版更差。基线对得上，后面的"变好了"才有意义。

三套候选方案各跑 10 个种子（同种子表、同剧本）：

| 方案 | 胜率 | 全灭局 | 我方伤亡 | 其中车顶机枪 |
|---|---|---|---|---|
| `base` 原值 | 3/10 | 1 | 71 人 | 66% |
| **A 削弱车顶机枪**（伤害 ×0.55、射击间隔 ×1.80） | **8/10** | 1 | **36 人** | 72% |
| B 提高队友耐久（生命 ×1.40） | 3/10 | 0 | 56 人 | 82% |
| C 降低撤离门槛（6 人 → 5 人） | 3/10 | 1 | 71 人 | 66% |
| A+B | 8/10 | 0 | 26 人 | 73% |
| A+C | 8/10 | 2 | 41 人 | 68% |

**结论：车顶机枪是唯一瓶颈，只采用 A。**

- **B 减伤不加胜场**：伤亡 71 → 56、全灭局 1 → 0，但胜率一动没动。
  说明队友不是"赢得不够"而是"赢的条件里没有他们"。
- **C 完全无效**：胜率、伤亡总数、全灭局三项与 `base` 逐项相同 ——
  6→5 这个门槛**没有卡住过任何一局**，它调的是一个从未生效的约束。
- **A 单独就已经到顶**：再叠 B/C 都停在 8/10，只是在同一个上限下面换伤亡分布。
  A+B 的 26 人看着最漂亮，代价是多引入一个平衡旋钮去换"更耐打"而非"更容易赢"，
  不值。**一个根因、一个杠杆。**
- A 之后车顶机枪的**占比**反而升到 72%：分母（总伤亡）掉得比它自己快。
  它仍然是第一威胁，只是量级下来了 —— 这和"根因还能再优化"是两件事。
- 掩体位置与 `p.r` **一行未动**（这是硬约束，动了等于重做关卡）。

三个旋钮收在 `va_config.h` 的 `BalanceCfg BAL` 里，默认值就是 A。
`va_sweep` 的方案表里有一行 `BAL 默认`：它不覆盖任何参数、直接读默认值，
**那一行的数字必须与 `A-mg` 完全相同** —— 否则就是"扫描报告"和"实机行为"分了家，
而这种分家不会有任何报错来提醒你。

## 角色形象（立绘 → 胸像 → 三维模型）

11 个角色：我方 8 种职务 + 敌方 3 种。键名（`char_*`）在**三维模型文件名、
胸像纹理名、代码里的映射表**三处是同一个，只有一处真值（`scene_builder.cpp`
的 `all_art_keys()` / `ally_art_key()` / `enemy_art_key()`）。

| 任务简报 · 小队名册 | 三维模型近景（正面） | 三维模型近景（侧面） |
|---|---|---|
| ![名册](docs/char_roster.png) | ![正面](docs/char_model.png) | ![侧面](docs/char_model_side.png) |

近景两张同时说明了三件事：模型**朝向**标定正确（正面能看到胸挂 / 水壶 / 双脚正对镜头，
侧面是干净的侧影）、**比例**为 1.68 m、**脚底**踩在地面上（靴底与草地相交，
脚下还留了一段地面）。

```bash
# 1) 立绘去水印 + 规范命名 + 自动裁 256×332 胸像
python tools/prep_char.py
python tools/montage_char.py          # 拼联络表 —— 并排才看得出切歪

# 2) 图生3D（先只做 1 个，近景确认朝向，再批量 —— 顺序错了整批返工）
python tools/gen3d_batch.py                     # 会跳过已有成品，中断后可直接续跑
#    产物 42.78MB → 瘦身到 1.56MB（网格 5 万面不动，只降内嵌 4K 贴图 → 512²）
python tools/gen3d_batch.py --force char_mg     # 指定重做某一个

# 生成通道可切换（三条路的产物规格一致，都是 5 万面 / 1.5MB）：
#   默认（不设变量）   内置通道，平台侧每天 5 次提交且当天不重置
#   VA_GEN3D_BACKEND=hy   TokenHub 直连（一把 sk- 开头的 API Key，无每日提交上限）
#   VA_GEN3D_BACKEND=tc   腾讯云 CAM 签名直连（要 SecretId/SecretKey）
# 换型号档位用 VA_GEN3D_MODEL（默认 hy-3d-3.1）：TokenHub 的免费额度是**按模型**给的，
# 3.1 用尽后同族的 3.0 还能接着做 —— 换相邻版本号是零风格风险的做法。
VA_GEN3D_BACKEND=hy VA_GEN3D_MODEL=hy-3d-3.0 python tools/gen3d_batch.py char_medic

# 额度用尽的报错是 401008（免费额度用尽且未开后付费），此时两条路二选一：
#   ① 换到同族另一个档位（上面这条，继续吃免费额度）
#   ② 在腾讯云控制台给该模型开通「后付费」—— 开通后**同一把 key、同一个模型名
#      直接就能继续**，脚本与参数一行都不用改（实测 3.1 开通后立即续做成功）。
# 另：hy-3d-express（极速版）跑得通但**不采用** —— 它只出 OBJ zip（不是 GLB，
# 还要过一遍 Blender），且贴图烘焙有明显瑕疵，与已接入的 3.1 成品风格不齐。
```

**检阅台**（`VA_UNIT_SHOW`）是模型接进去之后唯一能回答"朝向 / 比例 / 脚底落地 /
倒地姿态"的通道 —— 战场截图里单位只有几十像素高，答不了这四件事。
它与战场共用同一套建节点与姿态代码，所以看到的**就是**战场上那个模型：

```bash
# 陈列排：11 格等距两排 + 一个倒地姿态，看整体齐备度与彼此差异
VA_UNIT_SHOW=1 VA_CAPTURE=1 VA_CAPTURE_DIR=res://sweep/show \
  sdk/godot/Godot_v4.5-stable_win64_console.exe --path .

# 近景单体：正/侧/背三张对照即可钉死朝向标定
VA_UNIT_SHOW=one:char_rifleman       VA_CAPTURE=1 ...   # 正面
VA_UNIT_SHOW=one:char_rifleman:90    VA_CAPTURE=1 ...   # 侧面
VA_UNIT_SHOW=one:char_rifleman:180   VA_CAPTURE=1 ...   # 背面
```

判朝向用**身体**（胸挂 / 背囊 / 脚），不要用脸 —— 面部姿态常常是烧进网格的
（立绘里头只占很小一块，生成器会把微侧 / 低头一起烘进去）。

## 开发工具

`tools/` 下的脚本基本都是零依赖的纯 Python（不依赖 PIL / numpy，自己实现 PNG 编解码）：

| 脚本 | 用途 |
|---|---|
| `capture.sh` / `capture_ui.sh` | 无人值守截图取证：设好 `VA_CAPTURE` 后启动引擎，到点存图并自动退出。`capture_ui.sh` 走五屏（菜单 / 简报 / 结算胜负 / 战斗） |
| `probe_png.py` | 客观量测画面：分区域报平均 RGB、亮度、饱和度、过曝率（判断"画面发灰"必须靠数据，不能靠肉眼） |
| `crop_png.py` | 裁剪 + 整数放大 + 叠加 NDC 网格，让"某物在屏幕的哪一行哪一列"直接读成数字 |
| `vm_eval.py` | 按颜色掩码提取枪模各部件像素，统计亮度分布（死黑 / 正常 / 过曝占比） |
| `diff_png.py` | 两图逐像素求差，定位某个物体实际占据的屏幕区域 |
| `period_probe.py` | 去趋势 + 自相关，判定画面里的规律条纹（**肉眼看不出合成出来的周期条纹**） |
| `check_log_encoding.py` | 检查日志调用里的中文是否都走了 `String::utf8()`（漏了会写出二次编码的乱码，**编译期不报错**） |
| `inject_key.py` | 把一段"人按住方向键"的按键流打进前台窗口（按下 → 按 Windows 节奏重发 keydown → 松开），并支持"只按下 / 只松开 / 前台切给别的窗口"。**输入类手感问题的唯一可靠取证手段** —— 人手没法精确复现"按住 2 秒、其间每 50ms 一次重复"。安全约束：标题优先**精确相等**、每条事件发出前核实目标窗口仍在前台，否则中止（本机同时开着 Visual Studio，按子串匹配会**先命中 VS**，32 条 W 全打进编辑器，实测踩过） |
| `input_hold_test.sh` | **「按住方向键」输入回归**，自己给 PASS/FAIL：`hold` = 按住 3 秒（其间按系统节奏重复），`focus` = 按住 → 前台切走 → 在别的窗口松开。判据：echo 不得改动 `IN`；按住期间不得有"位移为 0"的心跳；松开后不得有位移；失焦后**一次心跳之内**必须停住（判据只看"清空闩锁"那行之后，且放过跨过渡的第一条 —— 它的区间跨了失焦那一刻） |
| `prep_art.py` / `prep_char.py` | 任务素材 / 角色立绘预处理：去半透明水印、规范命名、自动裁胸像 |
| `montage_char.py` | 把 11 张胸像拼成联络表（**并排才看得出切歪**） |
| `gen3d.py` / `gen3d_batch.py` | 图生3D 单张 / 批量编排（绕命令行长度上限、按服务端并发配额限流、断点续跑） |
| `slim_glb.py` | GLB 瘦身：只替换内嵌贴图的 bufferView，不动网格（**仅这一步需要 Pillow**） |
| `downscale_png.py` | 盒式降采样，生成 README 配图 |
| `patch_*.py` | 开发过程中的一次性补丁脚本（已全部应用完毕，保留仅作历史记录，**不要重复执行**） |

## 当前进度

**已完成**

- 逻辑层 100% 移植（16 文件 / 4261 行），逻辑层与引擎彻底解耦
- 程序化场景：地形铺展 700m、公路与沥青贴图、草地贴图、远景两圈山脊、程序化云层
- 光照与大气：三套天气配方、ACES 色调映射、三点布光 + 环景光 + SSAO/SSIL、体积雾、色调分级 3D LUT
- 掩体建模：岩石（低模不规则多面体）、树（高干 + 分枝 + 多球簇树冠）、灌木
- 第一人称武器视图模型：投影反推标定姿态、五盏独立打光、导轨齿/散热孔/抛壳口等分件、红点瞄具、换弹动画
- **界面外壳**：主菜单 / 任务简报（区域态势图 + 任务目标 + **小队名册 11 格胸像**）/ 结算面板，全部手绘
- **战斗 HUD**：罗盘、雷达、任务目标横幅、警报、击杀回执、小队状态板、弹药与装备、指挥链路、无线电字幕、动态准星、命中标记、受击方位弧、击杀飘字、倒地倒计时、受伤暗角
- **角色形象**：11 个角色的立绘 + 256×332 胸像 + 三维模型（**11/11 齐备**，每个 42.78MB → 1.5MB 瘦身、
  5 万面档：实测三角面 49,672~50,702），按美术键在运行时载入，载入失败自动回退到程序化图元
  （少一个模型文件不该少一个人）
- 可复现取证链路：固定种子 + 定时截图 + 客观像素量测 + 分层消融 + 角色模型检阅台
- **HUD 事件路径已实测触发并取证**：命中标记 / 击杀飘字 / 倒地倒计时这三条路径
  此前只"存在于代码里"—— 它们只在特定战斗事件下出现，而定时截图（`VA_CAPTURE`）
  撞不上只亮 0.24 秒的标记，所以从未被真正验证过。改为**由事件自己声明取证时机**
  （`VA_CAPTURE_EV`），并以 `VA_HIDE_HUD` 消融对照确认三样都画在 HUD 层：
  同一次命中/击杀/倒地，HUD 一藏，标记与飘字整体消失、只剩纯 3D 画面
- 取证链路上有两个**看图就能踩进去的坑**，判据与自证日志都已固化进代码：
  - **落盘要等"墙钟时长"，不是"隔 N 帧"**。HUD 的瞬时标记按墙钟衰减（命中 0.24 秒），
    "隔 N 帧"在低帧率下会等过头：1080p + `VA_FF=6` 时 2 帧 = 0.267 秒 > 0.24 秒，
    标记已灭 —— 那一组因此**一张命中标记都截不到**（不是标记没画，是拍晚了）。
    现改为"等 0.05 秒墙钟 + 至少跨 1 帧"，与帧率解耦；落盘时打一行
    `[capture-ev] 落盘 … 墙钟 +0.0XXs N 帧` 自证时序，**墙钟值必须小于被取证据的寿命**。
    改后同配置复测：`VA_FF=6` 下**首次截到命中标记**，日志 `墙钟 +0.117s 1 帧` < 0.24 秒 ✅
  - **命中标记的判据是"斜线"，不是"十字"**。常驻准星画的是四条正交短线，命中标记画的
    才是四条 ±45° 斜线（`draw_crosshair` / `draw_hitmarker` 各有一条注释写明）——
    "截图里中央有十字"不能证明命中路径成立，要看有没有斜线
- **玩法平衡**：离线扫描器 `va_sweep`（`src/sim/*` 脱离引擎单独跑）量出**车顶机枪是唯一瓶颈**，
  采用 A 方案（伤害 ×0.55、射击间隔 ×1.80）—— 胜率 **3/10 → 8/10**、我方伤亡 **71 → 36 人**；
  配套 `BalanceCfg BAL` 三个旋钮 + 一行 `BAL 默认` 校验（防"扫描与实机分家"）
- **VS 2022 里 F5 可直接运行/调试**：目标本身是 SHARED 库，VS 默认会拿它去"启动"，
  报「不是有效的 Win32 应用程序」；现由 CMake 的 `VS_DEBUGGER_COMMAND` 指向引擎
  （`--path <工程根> --log-file debug_run.log`），断点落进自己的源码。详见「在 VS 2022 里运行 / 调试（F5）」
- **日志中文乱码已修**：`String(const char *)` 按 Latin-1 解释字节，28 处含中文的日志
  写成二次编码（`取证` → `åè¯æ³¨å`）；全部改走 `String::utf8()`，
  并加 `tools/check_log_encoding.py` 守卫（旧版报 58 处、当前 0 处）
- **第一人称视野遮挡已修**：玩家自己的身体被当成普通单位画在了相机位置上（镜头在自己模型的头里），
  实测遮挡 **44.6% 像素**。现由 `sync_entity_nodes` 跳过玩家自己；
  同时补了 `VA_DBG_UNITS`（"镜头最近的是谁"，报水平距离）与 `VA_SHOW_SELF`（复现用）两个取证旋钮
- **按住方向键的移动已修**（两个方向都堵上了，见「实现要点」）：
  - 原来**按住不放只走 0.53 秒**就被自己的按键重复事件按停（重复的 `is_pressed()` 仍是 1，
    被那行 `&& !is_echo()` 当成了松开）。实测同一次按住：改前 0.53 秒 / 35 m，
    改后 3 秒全程 11 条心跳**无一为 0**、合计 201 m。
  - 反过来，**丢 keyup 会卡住不放**（失焦 / 进菜单两条路径）→ 角色自己一直走。
    现统一走 `clear_held_input()`，并支持"失焦回来、键还按着"时靠按键重复把状态接上
    （实测：切走→停（60 多条心跳位移全 0）→切回→靠一条 `echo=1` 接着走→松开即停）。
  - 配套：`tools/inject_key.py`（真实按键注入）+ `VA_DBG_INPUT`（逐事件表 + 位置心跳）；
    `tools/check_log_encoding.py` 的判据从"字面量紧跟 `String::utf8(`"改为
    "落在 `String::utf8(…)` 的实参括号内"，消除三元表达式误报

**待办**

- 载具模型精致化（目前仍是块状体）
- 特效层：曳光弹、爆炸、弹壳抛出、烟尘
- 音频层（网页版有 62 个程序化音效 id，PC 版尚未接入）
- 语音指挥：计划走 **SAPI 识别 + TTS 回话 + 面板兜底**

## 许可

个人项目，未附许可协议。
