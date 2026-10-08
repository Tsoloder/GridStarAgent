---
name: missile-root-junction
description: 翼/舵与弹体（或舵轴）结合处各向异性网格线分布处理。该位置不修改两端分布，使用增长分布（增长率 1.2，层数 50），中间值与翼/舵面尺寸一致。适用于弹翼(wing)连弹体(fuselage)和舵(fin)连弹体或舵轴(finshaft)两种结合场景。当导弹总 Skill 路由到结合处处理时加载本 Skill。
aliases: [翼身结合, 翼体结合, 舵身结合, 舵体结合, fin-fuselage, rudder-fuselage, fin-junction, rudder-junction]
tags: [CFD, 网格, 各向异性, 结合处, 导弹, 翼, 舵]
category: CFD
version: 2.0.0
author: Tsolodancer
allowed-tools: []
---

# 翼/舵与弹体（舵轴）结合处各向异性网格线分布

## 适用位置

- 翼（wing）与弹体的结合处
- 舵（fin）与弹体或舵轴的结合处
- 根部附近曲率变化较大、需要局部加密的结合区域

## 结合类型

| 部件 | 结合对象 | 分组名 |
|------|---------|--------|
| 弹翼(wing) | 弹体 | `fuselage` |
| 舵(fin) | 弹体 | `fuselage` |
| 舵(fin) | 舵轴 | `finshaft` |

> 弹翼与舵的**子面前缀**分别为 `wing` / `fin`（如 `wingSideSurface` / `finRoot`）；结合处通过子面分组与结合对象（`fuselage` 或 `finshaft`）取公共边。
> ⚠️ 经「分组归并」后主组变为 `wing` / `fin`，**子面区分丢失**，需按归并前的面 ID 台账或几何反推定位。

## 前置条件

- 已获取**当地弦长**（由 `GetMissileDimensions` 返回：弹翼取 `wing_*_chord`，舵取 `fin_*_chord`）
- 已获取翼/舵面尺寸（中间值用，0.02 × 当地弦长）
- 表面网格已完成
- `UGReDimensionConfigDistribution` 系列工具参数已就绪
- **链路接口可用**：`GetConnectorsByDomains` 能返回真实网格线 ID（若返回兜底值，整个各向异性阶段按主 Skill「工具可用性判定」标记 `skipped`）

## 尺寸参数

| 参数 | 值 |
|---|---|
| 首端间距 | 不修改两端分布 |
| 尾端间距 | 不修改两端分布 |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 分布类型（disFunc） | 0（双曲正切） |
| 中间值（mindValue） | 翼/舵面尺寸一致（0.02 × 当地弦长） |
| 根部段 | 保证平滑分布 |

## 网格线分布设置

### 设置步骤

1. **获取结合处的网格线 ID**（翼/舵与结合对象的共享边）：

```
# 例：弹翼(wing)与弹体(fuselage)的结合处
# 分组 domain ID 从 GetAllSpitAssemblyGroupProperty 解析（返回 list[{组名:{line:[...], domain:[{ids:[...]}]}}]）
finSideDomains = GetGroupDomainIds("wingSideSurface")   # 解析 helper → [101, 102, ...]
fuselageDomains = GetGroupDomainIds("fuselage")                # → [3, 28, 29, 30]

finSideConnectors = set()
for id in finSideDomains:
    result = GetConnectorsByDomains([id])                    # → {"domains":[{"domain_id":id,"connector_ids":[...]}]}
    finSideConnectors.update(result["domains"][0]["connector_ids"])

fuselageConnectors = set()
for id in fuselageDomains:
    result = GetConnectorsByDomains([id])
    fuselageConnectors.update(result["domains"][0]["connector_ids"])

junctionConnectors = finSideConnectors & fuselageConnectors     # 取交集 = 结合处共享边

# 同理，舵(fin)结合处：
# finRootDomains = GetGroupDomainIds("finRoot")
# 结合对象按分组名 fuselage 或 finshaft 获取
```

2. **获取每条共享边的原分布值**（不修改两端间距）：

```
# 对每条结合处网格线，读取当前 headSpace 和 tailSpace
for cid in junctionConnectors:
    spacing = GetConnectorsStartAndEndUnitLenth(cid)      # → {"start": s0, "end": e0}
    # 保留原有间距值传入步骤 3
```

3. 调用 `UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)` 设置分布参数（只修改中间值，两端保持原分布）：

| 参数 | 值 |
|---|---|
| `ids` | 结合处共享边 ID（逗号分隔） |
| `headspace` | **不修改**（传原分布值） |
| `tailspace` | **不修改**（传原分布值） |
| `params` | `"1.2,50,1.2,50"`（严格格式 `"headRate,headLayer,tailRate,tailLayer"`） |
| `mindValue` | 翼/舵面尺寸（0.02 × 当地弦长） |

> ⚠️ 该工具只有上述 5 个参数；没有 `disFunc`、`headRate` 等独立参数。

4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution(ids, headspace, tailspace, params, mindValue)`（参数格式同 `UGReDimensionConfigDistribution`）。

5. 对于根部段（若存在），参照 `missile-chordwise-direction` Skill 的弦向分布做平滑衔接。

### 工具调用表

| 序号 | 工具 | 参数 | 说明 |
|------|------|------|------|
| 1 | `GetAllSpitAssemblyGroupProperty` | 无（解析 `["wingSideSurface"]` / `["finRoot"]` 及 `["fuselage"]` / `["finshaft"]` 的 `domain[].ids`） | 获取结合处涉及的分组网格面 ID |
| 2 | `GetConnectorsByDomains` | `domain_ids`: 每个网格面 ID | 获取面的网格线集合 |
| 3 | `GetConnectorsStartAndEndUnitLenth` | `connector_ids`: 每条共享边 ID | 获取原有两端间距（保留不修改） |
| 4 | `UGReDimensionConfigDistribution` | `ids`: 结合边 ID; `headspace`: 原值; `tailspace`: 原值; `params`: `"1.2,50,1.2,50"`; `mindValue`: 0.02 × 当地弦长 | 设置增长分布（不修改两端） |
| 5 | `UGReDimensionSmoothDistribution` | `ids`: 结合边 ID; `headspace`/`tailspace`: 原值; `params`: `"1.2,50,1.2,50"`; `mindValue`: 0.02 × 当地弦长 | 平滑过渡到相邻区域 |

### 注意事项

- **严禁修改两端分布**——本位置的核心约束。
- 翼和舵的结合处各需处理一次（若有）。
- 舵可能同时连接弹体和舵轴，需分别判断。
- **双曲正切分布参数统一**：增长率 1.2、层数最大 50、分布类型 0。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`，且两端分布未被修改。
- 根部段（若存在）平滑过渡无误。
- 任一步骤失败时停止，不继续后续步骤。