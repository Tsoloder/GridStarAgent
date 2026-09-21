---
name: fin-spanwise-direction
description: 导弹翼/舵展向（spanwise direction）各向异性网格线分布处理，处理对象是翼/舵前缘线（前缘面与侧面的公共边）。前缘线按与弹体 body 或梢部 tip 是否共点分为两类，只处理最两端两根：共点端取 0.008×当地弦长，另一端保留原网格线端分布，中间值与翼/舵面尺寸一致。适用于弹翼(fin)和舵(rudder)两类部件。当总 Skill missile-anisotropic-mesh 路由到展向处理时加载本 Skill。
aliases: [翼展向分布, 舵展向分布, 展向加密, fin spanwise, rudder spanwise, 前缘线, 根部加密, 梢部加密]
tags: [CFD, 网格, 各向异性, 展向, 导弹, 翼, 舵, 前缘线]
category: CFD
version: 2.0.0
author: Tsolodancer
allowed-tools: []
---

# 导弹翼/舵展向各向异性网格线分布（前缘线处理）

**导弹翼/舵展向各向异性分布就是前缘线的处理**，二者是同一件事，不存在独立的"展向网格线"流程。本 Skill 只有一条流程，即下文的前缘线处理。

## 处理对象与范围

- **前缘线**：分部件分组**前缘面** `{prefix}LeadingEdge` 与**侧面** `{prefix}SideSurface` 的**公共边**，`{prefix}` = `fin` 或 `rudder`。
- 前缘线通常有**多根**（沿展向分段），**只处理最两端的两根**，中间段一律保持原分布不动。

| 类型 | 判定依据 | 位置 | 共点端间距 | 另一端间距 |
|---|---|---|---|---|
| 类型一 | 与弹体 `body` 分组的网格面有**共点** | 根部端那一根 | 0.008 × 当地弦长 | 原分布值 |
| 类型二 | 与梢部（`finTip` / `rudderTop`）分组的网格面有**共点** | 梢部端那一根 | 0.008 × 当地弦长 | 原分布值 |

## 部件分组名对照

| 部件类型 | 分组前缀 | 前缘面 | 侧面 | 梢面 | 结合部件 |
|---------|---------|--------|------|------|---------|
| 弹翼(fin) | `fin` | `finLeadingEdge` | `finSideSurface` | `finTip` | `body`（弹体） |
| 舵(rudder) | `rudder` | `rudderLeadingEdge` | `rudderSideSurface` | `rudderTop` | `body` 或 `rudderShaft`（舵轴） |

> 舵的根部结合处可能连弹体(`body`)或舵轴(`rudderShaft`)，需根据实际分组判断。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，确认对应分组存在。
   - 翼：`body`、`finTip`、`finLeadingEdge`、`finSideSurface`
   - 舵：`body` 或 `rudderShaft`、`rudderTop`、`rudderLeadingEdge`、`rudderSideSurface`
2. 调用 `GetMissileModelParameters`，获取弦长参数。
3. 表面网格已生成：`GetAllObjectByType`(6) 返回非空。
4. 上述任一条件不满足时停止。

## 分布参数

| 参数 | 值 |
|---|---|
| 共点端间距（`headspace` 或 `tailspace`） | 0.008 × 当地弦长 |
| 另一端间距 | **原分布值**，由 `GetConnectorStartAndEndUnitLenth` 读取 |
| `params` | `"1.2,50,1.2,50"`（增长率 1.2、层数 50） |
| 分布类型（`disFunc`） | 0（双曲正切） |
| `mindValue`（中间值） | 翼/舵面尺寸（= 0.02 × 当地弦长） |

> ⚠️ 共点端落在网格线的 start 端还是 end 端，决定 `headspace` / `tailspace` 怎么传，**必须先判端再传参**。

## 执行步骤

1. **获取各分组网格面 ID**：
   1. `GetSpliteAssemlyDomainsBatch`（`group_names` = [`{prefix}LeadingEdge`, `{prefix}SideSurface`, 结合部件, 梢面]）。
   任一返回为空 → 失败停止。

2. **识别前缘线**：通过前缘面与侧面网格面 ID 求公共边（两组共有的网格线），得到所有前缘线 ID 列表。
   - 与结合部件（body 或 rudderShaft）网格面有共点的前缘线 → **类型一**（根部端）
   - 与梢部（`finTip` / `rudderTop`）网格面有共点的前缘线 → **类型二**（梢部端）
   - 前缘线列表为空 → 失败停止。
   - 不在上述两端的其他前缘线 → **中间段，跳过不处理**。

3. **读取原分布值**：对类型一、类型二各调用 `GetConnectorStartAndEndUnitLenth`（线 ID）→ `{"start": s0, "end": e0}`。
   > 必须在 `UGReDimensionConfigDistribution` **之前**读取，否则原值会被覆盖。

4. **计算间距**：`edgeSpacing` = 0.008 × 当地弦长（根部端用根部弦长，梢部端用梢部弦长）；`mindValue` = 翼/舵面尺寸。

5. **组装参数并设置增长分布**（类型一、类型二各执行一次）：
   - 共点端 = start 端 → `headspace` = `edgeSpacing`，`tailspace` = `e0`
   - 共点端 = end 端 → `headspace` = `s0`，`tailspace` = `edgeSpacing`
   - 调用 `UGReDimensionConfigDistribution`。

6. **对边匹配**（如需）：`UGReDimensionMatch`。

7. **平滑过渡**（如需）：`UGReDimensionSmoothDistribution`。

## 注意事项

- **只处理最两端两根前缘线**（类型一、类型二各一根），中间段一律不动。
- 共点端取 0.008 × 当地弦长，另一端保留原分布值，**切勿两端都写** `edgeSpacing`。
- **翼和舵各需执行一次**，分组前缀不同但逻辑相同。
- 共点判定只比对**网点 ID**（`pointID`），ID 相同即为共点，不要用坐标近似比较。
- 展向分布**只控制内部点分布**，不更改边界分布。
- `params` 严格格式为 `"headRate,headLayer,tailRate,tailLayer"`。

## 完成标准

- 类型一与类型二各识别出一根前缘线。
- 两根线的共点端间距为 0.008 × 当地弦长，另一端为原分布值；中间段未被修改。
- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。