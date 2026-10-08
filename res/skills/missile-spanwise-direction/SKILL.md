---
name: missile-spanwise-direction
description: 导弹翼/舵展向（spanwise direction）各向异性网格线分布处理，处理对象是翼/舵前缘线（前缘面与侧面的公共边）。前缘线按与弹体 fuselage 或梢部 tip 是否共点分为两类，只处理最两端两根：共点端取 0.008×当地弦长，另一端保留原网格线端分布，中间值与翼/舵面尺寸一致。适用于弹翼(wing)和舵(fin)两类部件。当总 Skill missile-anisotropic-mesh 路由到展向处理时加载本 Skill。
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

- **前缘线**：分部件分组**前缘面** `{prefix}LeadingEdge` 与**侧面** `{prefix}SideSurface` 的**公共边**，`{prefix}` = `wing` 或 `fin`。
- 前缘线通常有**多根**（沿展向分段），**只处理最两端的两根**，中间段一律保持原分布不动。

| 类型 | 判定依据 | 位置 | 共点端间距 | 另一端间距 |
|---|---|---|---|---|
| 类型一 | 与弹体 `fuselage` 分组的网格面有**共点** | 根部端那一根 | 0.008 × 当地弦长 | 原分布值 |
| 类型二 | 与梢部（`wingTip` / `finTop`）分组的网格面有**共点**；`wingTip` 缺失时以翼侧面自由梢端的边界线为梢部端 | 梢部端那一根 | 0.008 × 当地弦长 | 原分布值 |

## 部件分组名对照

| 部件类型 | 子面前缀 | 前缘面 | 侧面 | 梢面 | 结合部件 |
|---------|---------|--------|------|------|---------|
| 弹翼(wing) | `wing` | `wingLeadingEdge` | `wingSideSurface` | `wingTip`（仅梯形翼存在，三角翼无） | `fuselage`（弹体） |
| 舵(fin) | `fin` | `finLeadingEdge` | `finSideSurface` | `finTop` | `fuselage` 或 `finshaft`（舵轴） |

> 弹翼与舵的**子面前缀**分别为 `wing` / `fin`，分组直接以 `{prefix}*` 子面形式存在；舵另有舵轴 `finshaft` 独立分组。
> ⚠️ 经「分组归并」后主组变为 `wing` / `fin`，**子面区分丢失**，需按归并前的面 ID 台账或几何反推定位。
> 舵的根部结合处可能连弹体(`fuselage`)或舵轴(`finshaft`)，需根据实际分组判断。
> 按 需求文件 1.4：翼梢面只存在于梯形翼；三角翼无 `wingTip`，梢部端以前缘线最外端（与侧面自由边相邻的端点）判定。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，确认对应分组存在。
   - 翼：`fuselage`、`wingLeadingEdge`、`wingSideSurface`（`wingTip` 仅梯形翼存在，三角翼缺失属正常）
   - 舵：`fuselage` 或 `finshaft`、`finTop`、`finLeadingEdge`、`finSideSurface`
2. 调用 `GetMissileDimensions`，获取弦长参数（弹翼取 `wing_root_chord`/`wing_tip_chord`，舵取 `fin_root_chord`/`fin_tip_chord`）。
3. 表面网格已生成：`GetAllObjectByType`(6) 返回非空。
4. **链路接口可用**：`GetConnectorsByDomains` 能返回真实网格线 ID（若返回兜底值，整个各向异性阶段按主 Skill「工具可用性判定」标记 `skipped`）。
5. 上述任一条件不满足时停止。

## 分布参数

| 参数 | 值 |
|---|---|
| 共点端间距（`headspace` 或 `tailspace`） | 0.008 × 当地弦长 |
| 另一端间距 | **原分布值**，由 `GetConnectorsStartAndEndUnitLenth` 读取 |
| `params` | `"1.2,50,1.2,50"`（增长率 1.2、层数 50） |
| `mindValue`（中间值） | 翼/舵面尺寸（= 0.02 × 当地弦长） |

> 注：`UGReDimensionConfigDistribution` 无 `disFunc` 参数；分布类型由所选工具（Config / Smooth / Average）决定。

> ⚠️ 共点端落在网格线的 start 端还是 end 端，决定 `headspace` / `tailspace` 怎么传，**必须先判端再传参**。

## 执行步骤

1. **获取各分组网格面 ID**：
   1. 调 `GetAllSpitAssemblyGroupProperty()`，按组名取 `domain[].ids`（`{prefix}LeadingEdge`、`{prefix}SideSurface`、结合部件、梢面）。
   任一返回为空 → 失败停止。

2. **识别前缘线**：通过前缘面与侧面网格面 ID 求公共边（两组共有的网格线），得到所有前缘线 ID 列表。
   - 与结合部件（fuselage 或 finshaft）网格面有共点的前缘线 → **类型一**（根部端）
   - 与梢部（`wingTip` / `finTop`）网格面有共点的前缘线 → **类型二**（梢部端）；`wingTip` 缺失（三角翼）时，取前缘线最外侧端点（与侧面自由梢边相邻）所在的那根
   - 前缘线列表为空 → 失败停止。
   - 不在上述两端的其他前缘线 → **中间段，跳过不处理**。

3. **读取原分布值**：对类型一、类型二各调用 `GetConnectorsStartAndEndUnitLenth`（connector_ids）→ `{"connectors":[{"id","start","end"}]}`。
   > 必须在 `UGReDimensionConfigDistribution` **之前**读取，否则原值会被覆盖。

4. **计算间距**：`edgeSpacing` = 0.008 × 当地弦长（根部端用根部弦长，梢部端用梢部弦长）；`mindValue` = 翼/舵面尺寸。

5. **组装参数并设置增长分布**（类型一、类型二各执行一次）：
   - 共点端 = start 端 → `headspace` = `edgeSpacing`，`tailspace` = `e0`
   - 共点端 = end 端 → `headspace` = `s0`，`tailspace` = `edgeSpacing`
   - 调用 `UGReDimensionConfigDistribution`。

6. **对边匹配**（如需）：`UGReDimensionMatch`。

7. **平滑过渡**（如需）：`UGReDimensionSmoothDistribution(ids, headspace, tailspace, params, mindValue)`。

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