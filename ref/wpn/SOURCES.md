# 武器参考图来源

三张年代制式武器的参考图，从**维基共享资源**取，经 `wsrv.nl` 图片代理下载
（本机 `commons.wikimedia.org` / `upload.wikimedia.org` 直连不通，代理可通）。

| 本仓库文件 | 维基文件名 | 说明 |
|---|---|---|
| `w_mosin.png` | `Mosin nagant m9130 from cia.jpeg` | 莫辛-纳甘 M91/30，白底产品图，无瞄镜 |
| `w_ppsh.png`   | `PPSh-41 from soviet.jpg`             | 波波沙-41，白底产品图，71 发弹鼓 |
| `w_dp27.png`   | `Machine gun DP MON.jpg`              | DP-27，博物馆实拍（石地板），已抠底 |

取图与规范化命令（见 `tools/cut_bg.py` 的注释）：

```bash
# 白底图只裁边补白
python tools/cut_bg.py sweep/wpn_ref/mosin_b.png ref/wpn/w_mosin.png --side 1600
python tools/cut_bg.py sweep/wpn_ref/ppsh_b.png  ref/wpn/w_ppsh.png  --side 1600
# DP-27 那张要抠底，并定点擦掉地面投影（投影与枪身同为中性灰，阈值分不开）
python tools/cut_bg.py sweep/wpn_ref/dp_test.png ref/wpn/w_dp27.png --thresh 110 --side 1600 \
    --erase 580,470,850,645 --erase 890,470,1120,645
```

## 为什么不用参考图集里的现代枪

游戏是 **1951 年铁原阻击战**的志愿军设定（角色立绘手持莫辛-纳甘）。
用户给的 CODOL 设计稿以现代/科幻武器为主，直接套用会与年代冲突，
因此**沿用其写实硬表面风格、把武器换成年代制式**（用户 2026-09-19 的裁决）。
