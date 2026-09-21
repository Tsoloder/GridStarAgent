---
name: fin-body-junction
description: 翼/舵与弹体（或舵轴）结合处各向异性网格线分布处理。该位置不修改两端分布，使用增长分布（增长率 1.2，层数 50），中间值与翼/舵面尺寸一致。适用于弹翼(fin)连弹体(body)和舵(rudder)连弹体或舵轴(rudderShaft)两种结合场景。当导弹总 Skill 路由到结合处处理时加载本 Skill。
aliases: [翼身结合, 翼体结合, 舵身结合, 舵体结合, fin-body, rudder-body, fin-junction, rudder-junction]
tags: [CFD, 网格, 各向异性, 结合处, 导弹, 翼, 舵]
category: CFD
version: 2.0.0
author: Tsolodancer
allowed-tools: []
---

# 翼/舵与弹体（舵轴）结合处各向异性网格线分布

## 适用位置

- 翼（fin）与弹体的结合处
- 舵（rudder）与弹体或舵轴的结合处
- 根部附近曲率变化较大、需要局部加密的结合区域

## 结合类型

| 部件 | 结合对象 | 分组名 |
|------|---------|--------|
| 弹翼(fin) | 弹体 | `body` |
| 舵(rudder) | 弹体 | `body` |
| 舵(rudder) | 舵轴 | `rudderShaft` |

## 前置条件

- 已获取**当地弦长**（由 `GetMissileModelParameters` 返回）
- 已获取翼/舵面尺寸（中间值用，0.02 × 当地弦长）
- 表面网格已完成
- `GenerateANisoDomainGrid` 相关参数已就绪

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
# 例：弹翼(fin)与弹体(body)的结合处
finSideDomains = GetSpliteAssemlyDomainsBatch(["finSideSurface"])  # → {"groups":[{"groupName":"finSideSurface","domain_ids":[...]}]}
bodyDomains = GetSpliteAssemlyDomainsBatch(["body"])               # → {"groups":[{"groupName":"body","domain_ids":[...]}]}

finSideConnectors = set()
for id in finSideDomains["groups"][0]["domain_ids"]:
    result = GetConnectorsByDomain(id)                      # → {"ids": [...]}
    finSideConnectors.update(result["ids"])

bodyConnectors = set()
for id in bodyDomains["groups"][0]["domain_ids"]:
    result = GetConnectorsByDomain(id)
    bodyConnectors.update(result["ids"])

junctionConnectors = finSideConnectors & bodyConnectors     # 取交集 = 结合处共享边

# 同理，舵(rudder)结合处：
# rudderRootDomains = GetSpliteAssemlyDomainsBatch(["rudderRoot"])
# 结合对象按分组名 body 或 rudderShaft 获取
```

2. **获取每条共享边的原分布值**（不修改两端间距）：

```
# 对每条结合处网格线，读取当前 headSpace 和 tailSpace
for cid in junctionConnectors:
    spacing = GetConnectorStartAndEndUnitLenth(cid)      # → {"start": s0, "end": e0}
    # 保留原有间距值传入步骤 3
```

3. 调用 `UGReDimensionConfigDistribution` 设置分布参数（只修改中间值，两端保持原分布）：

| 参数 | 值 |
|---|---|
| `headspace` | **不修改**（保持原分布值） |
| `tailspace` | **不修改**（保持原分布值） |
| `headRate` | 1.2 |
| `headLayer` | 50 |
| `tailRate` | 1.2 |
| `tailLayer` | 50 |
| `disFunc` | 0 |
| `mindValue` | 翼/舵面尺寸（0.02 × 当地弦长） |

3. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution(id)`，参数只需传入网格线 ID。

4. 对于根部段（若存在），参照 `fin-chordwise-direction` Skill 的弦向分布做平滑衔接。

### 工具调用表

| 序号 | 工具 | 参数 | 说明 |
|------|------|------|------|
| 1 | `GetSpliteAssemlyDomainsBatch` | `group_names`: `["finSideSurface"]` / `["rudderRoot"]` 及 `["body"]` / `["rudderShaft"]` | 获取结合处涉及的分组网格面 ID |
| 2 | `GetConnectorsByDomain` | `id`: 每个网格面 ID | 获取面的网格线集合 |
| 3 | `GetConnectorStartAndEndUnitLenth` | `id`: 每条共享边 ID | 获取原有两端间距（保留不修改） |
| 4 | `UGReDimensionConfigDistribution` | `id`: 结合边 ID; `headSpace`: 原值; `tailSpace`: 原值; `headRate/tailRate`: 1.2; `headLayer/tailLayer`: 50; `disFunc`: 0; `mindValue`: 0.02 × 当地弦长 | 设置增长分布（不修改两端） |
| 5 | `UGReDimensionSmoothDistribution` | `id`: 结合边 ID | 平滑过渡到相邻区域 |

### 注意事项

- **严禁修改两端分布**——本位置的核心约束。
- 翼和舵的结合处各需处理一次（若有）。
- 舵可能同时连接弹体和舵轴，需分别判断。
- **双曲正切分布参数统一**：增长率 1.2、层数最大 50、分布类型 0。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`，且两端分布未被修改。
- 根部段（若存在）平滑过渡无误。
- 任一步骤失败时停止，不继续后续步骤。