# 载具参考图来源

**本目录不存放参考图本体** —— 只记「怎么取到、选了哪一张、为什么这么裁」。
四条参考图是**图生3D 的输入**，落在 `assets/art/veh/veh_*.png`（已 gitignore）。

和武器那批（`ref/wpn/SOURCES.md`）的处置**不一样**，原因在授权：

| 批次 | 来源 | 授权 | 处置 |
|---|---|---|---|
| 三把武器 | 维基共享资源 | CC，附作者与许可 | 随仓库入库 |
| 四辆载具 | Bing 图片搜索命中的商品页 / 素材站 | 两套商业素材站 CG 商品图 + 一张商品摄影 + 一张博物馆实拍 | **不入库**，按 `/ref/` 的既有原则「需要时按脚本自行重建」 |

成品 `assets/art/veh/model/veh_*.glb` 是我们自己生成的，照常入库。

## 选取结果

取图通道：`tools/fetch_refs_bing.py`（Bing 图片搜索，`cn.bing.com`）。
`#NN` 是它输出的编号对照表里的序号 —— 换一次搜索顺序就会变，**不是稳定标识**。

| 产物 | 模型键 | 查询词 | 中选 | 来源站点 | 说明 |
|---|---|---|---|---|---|
| `veh_jeep.png`  | `jeep`  | Willys MB jeep side view | `#00` | motorcarclassics.com | 左侧视白棚实拍，1918×1280，带地面反光 |
| `veh_apc.png`   | `apc`   | M3 half-track armored personnel carrier white background | `#07` | oldboyhobby.com | 白底商品照，1000×1000，3/4 前视 |
| `veh_tank.png`  | `tank`  | M4A3E8 Sherman tank 3d model white background | `#09` | cdn.renderhub.com | 白底 CG 渲染，3840×2160，3/4 前视 |
| `veh_truck.png` | `truck` | GMC CCKW truck side view | `#01` | p.turbosquid.com | 白底 CG 渲染，**纯侧视** |

取图与规范化：

```bash
# 1) 出候选 + 带编号的对照表（四条查询之间自带 3 秒间隔）
python tools/fetch_refs_bing.py --jobs
# 2) 按下面的"选图口径"挑出中选候选，另存到 sweep/veh_ref/pick/raw_<键>.jpg
# 3) 统一车头朝右 + 抠底补方 → assets/art/veh/veh_<键>.png（1600 长边、正方形）
tools/prep_veh_refs.sh
# 4) 提交图生3D（token 走 stdin，不落 argv）
echo -n "<token>" | python tools/gen3d_batch.py --kind veh --jobs 2 veh_jeep
```

## 选图口径（四条，按优先级）

1. **白底或纯色底** —— `cut_bg.py` 只有"亮度 < 阈值算物体"这一条判据，
   草地 / 树林背景吃不掉；
2. **避开图库水印** —— alamy / dreamstime / shutterstock 的预览图都带水印，
   而**水印会被图生3D 当成车身上的涂装烘进贴图，建好之后不再有机会修**；
3. **侧视或 3/4 视** —— 要同时看清车头、负重轮、车厢；
4. **分辨率 ≥ 1000 px** —— 生成侧还会缩到 1024 级，低于这个数就是白给。

## 为什么是这四个型号

游戏是 **1951 年铁原阻击战**设定，而敌方身份由角色立绘钉死：
`char_enemy_rifle.png` 是 **M1 钢盔 + M1 加兰德 + M1943 野战夹克** —— **美军**。
所以车队取朝鲜战争美军制式，四辆一个体系：

| 模型键 | 型号 | 在车队里的角色 |
|---|---|---|
| `jeep`  | Willys MB | 轻便指挥 / 侦察车 |
| `apc`   | M3 Half-track | 半履带装甲输送车 |
| `tank`  | M4A3E8 Sherman | 中型坦克（游戏中唯一带主炮的） |
| `truck` | GMC CCKW | 六轮卡车，运兵 / 运弹 |

`VehicleSpec` 里四个键的顺序（`va_config.cpp` 的 `vehicle_of()`）与本表一致。

## 两条和图生3D 有关的硬约束（都踩过）

**① 输入必须补成正方形。** 生成侧会把输入**按中心裁成正方形**。
车辆侧视图长宽比约 1.5:1 ~ 2:1，不补方就会被切掉车头或车尾 ——
而且**模型照样生成成功、文件非空**，只是成品缺头少尾。
`cut_bg.py --square` 就是干这个的，"文件非空 ≠ 任务成功"。

**② 车头一律朝右。** 引擎里载具节点的局部 **+X 就是车头**（依据见
`tools/prep_veh_refs.sh` 的推导段）。而图生3D 把"图像的哪个方向"映到
"模型的哪个轴"是服务端定的、**只能实测**。武器那轮的实测结论是
"图像右 = +X"，本轮载具**实测复现了同一条**：

> `VA_UNIT_SHOW=veh:jeep`（临时偏航 0°）看到的是**车头正面** —— 格栅、大灯、
> 挡风玻璃、保险杠正对镜头。所以 `VEH_MODEL_YAW_DEG = 0.0f` 成立，
> 四辆车**共用**这一个校正角，不需要逐辆试。

统一朝右的收益就在这里：四张图都朝同一边 → 一个角度管四辆 →
省掉三轮"改角度 → 重编 → 重拍"的往返。
