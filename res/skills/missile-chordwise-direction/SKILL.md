---
name: missile-chordwise-direction
description: 导弹翼/舵弦向（chordwise direction）各向异性网格线分布处理。处理翼/舵前缘到后缘方向的网格线加密，靠近前缘处和后缘处的分布均为 0.008×当地弦长，中间值与翼/舵面尺寸一致。适用于弹翼(wing)和舵(fin)两类部件。当总 Skill missile-anisotropic-mesh 路由到弦向处理时加载本 Skill。
aliases: [翼弦向分布, 舵弦向分布, 弦向加密, fin chordwise, rudder chordwise, 前缘加密, 后缘加密]
tags: [CFD, 网格, 各向异性, 弦向, 导弹, 翼, 舵]
category: CFD
version: 2.0.0
author: Tsolodancer
allowed-tools: []
---

# 导弹翼/舵弦向各向异性网格线分布

## 适用位置

- 导弹翼/舵弦向方向（前缘至后缘的流向）
- 适用于翼/舵的侧面（`wingSideSurface` / `finSideSurface`）的弦向网格线
- 两种部件：弹翼(`wing`) 和 舵(`fin`)

## 前置条件

- 已通过 `GetMissileDimensions` 获取弦长参数
- 弹翼用 `wing_root_chord`(C_root) / `wing_tip_chord`(C_tip)
- 舵用 `fin_root_chord`(C_root) / `fin_tip_chord`(C_tip)
- 已获取翼/舵面尺寸（中间值用）
- 表面网格已生成
- **链路接口可用**：`GetConnectorsByDomains` 能返回真实网格线 ID（若返回兜底值，整个各向异性阶段按主 Skill「工具可用性判定」标记 `skipped`）

## 部件分组名对照

| 部件类型 | 子面前缀 | 侧面 | 后缘面 | 梢面 |
|---------|---------|------|--------|------|
| 弹翼(wing) | `wing` | `wingSideSurface` | `wingTrailingEdge` | `wingTip`（仅梯形翼存在） |
| 舵(fin) | `fin` | `finSideSurface` | `finTrailingEdge` | `finTop` |

> 注：弹翼与舵的**子面前缀**分别为 `wing` / `fin`，直接以 `{prefix}*` 子面分组存在；舵另有舵轴 `finshaft` 独立分组。
> ⚠️ 经「分组归并」后主组变为 `wing` / `fin`，**子面区分丢失**，需按归并前的面 ID 台账或几何反推定位（见主 Skill `missile-cfd-meshing-workflow` 的 `references/missile-segmentation.md`）。

## 尺寸参数

| 参数 | 值 |
|---|---|
| 前缘端间距（首层高度） | 0.008 × 当地弦长 |
| 后缘端间距（尾端高度） | 0.008 × 当地弦长 |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 分布类型（disFunc） | 0（双曲正切） |
| 中间值（mindValue） | 翼/舵面尺寸一致（0.02 × 当地弦长） |

## 网格线分布设置

### 设置步骤

1. **获取弦向网格线 ID**：根据当前处理的部件类型，用对应分组前缀获取侧面的弦向网格线：

```
# 例：弹翼(wing)侧面（分组 domain ID 从 GetAllSpitAssemblyGroupProperty 的 domain[].ids 解析，
#    返回结构 list[{组名:{line:[...], domain:[{ids:[...]}]}}]）
groupId = GetGroupDomainIds("wingSideSurface")   # 解析 helper，返回该组 domain[].ids 列表
chordwiseConnectors = []
for id in groupId:
    result = GetConnectorsByDomains([id])                   # → {"domains":[{"domain_id":id,"connector_ids":[...]}]}
    chordwiseConnectors.extend(result["domains"][0]["connector_ids"])

# 舵同理，用 "finSideSurface"
```

2. 调用 `UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)` 设置分布参数：

| 参数 | 值 |
|---|---|
| `ids` | 弦向网格线 ID（逗号分隔） |
| `headspace` | 0.008 × 当地弦长 |
| `tailspace` | 0.008 × 当地弦长 |
| `params` | `"1.2,50,1.2,50"`（严格格式 `"headRate,headLayer,tailRate,tailLayer"`） |
| `mindValue` | 翼/舵面尺寸（0.02 × 当地弦长） |

> ⚠️ 该工具**只有上述 5 个参数**；**没有 `disFunc`、`headRate`、`headLayer` 等独立参数**——增长率与层数统一打包进 `params` 字符串。

3. 如需对边匹配，调用 `UGReDimensionMatch(ids, targetId)`。
4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution(ids, headspace, tailspace, params, mindValue)`（参数格式同 `UGReDimensionConfigDistribution`）。

### 注意事项

- 弦向分布**只控制内部点分布**，不更改边界分布。
- 若根部和梢部的当地弦长不同（`C_root` ≠ `C_tip`），需沿展向分段分别计算前缘/后缘间距。
- 前缘点数 ≥ 7，梢部点数 ≥ 7（对齐 需求文件 表面网格参数要求）。
- **翼和舵各需执行一次**，分组名不同但逻辑相同。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 对边匹配无误。
- 任一步骤失败时停止，不继续后续步骤。