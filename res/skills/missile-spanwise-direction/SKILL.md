---
name: missile-spanwise-direction
description: 【回退专用】导弹翼展向（spanwise direction）各向异性网格线分布处理，处理对象是弹翼前缘线（前缘面与侧面的公共边）。前缘线按与弹体 fuselage 或梢部 tip 是否共点分为两类，只处理最两端两根：共点端取 0.1×该对翼厚度，另一端保留原网格线端分布，中间值与翼面尺寸一致。适用于弹翼(wing)。🔴 翼型运行时探测（M2）：`wingTipN` 存在=梯形翼、缺失=三角翼，两分支均需支持。🔴 仅在 `MissileWingAnisoProcessWingAnisotropy` 一站式返回 `status="failed"` 时由 `missile-anisotropy-processing` 自动回退加载；一站式正常时**不得**主动加载本 Skill。
aliases: [翼展向分布, 展向加密, wing spanwise, 前缘线, 根部加密, 梢部加密]
tags: [CFD, 网格, 各向异性, 展向, 导弹, 翼, 前缘线]
category: CFD
version: 2.2.0
author: Tsolodancer
allowed-tools: []
---

# 导弹翼展向各向异性网格线分布（前缘线处理）

**导弹翼展向各向异性分布就是前缘线的处理**，二者是同一件事，不存在独立的"展向网格线"流程。本 Skill 只有一条流程，即下文的前缘线处理。

## 处理对象与范围

- **前缘线**：分部件分组**前缘面** `{prefix}LeadingEdgeN` 与**侧面** `{prefix}SideSurfaceN` 的**公共边**，`{prefix}` = `wing`，`N` = 第几对弹翼。
- 前缘线通常有**多根**（沿展向分段），**只处理最两端的两根**，中间段一律保持原分布不动。

| 类型 | 判定依据 | 位置 | 共点端间距 | 另一端间距 |
|---|---|---|---|---|
| 类型一 | 与弹体 `fuselage` 分组的网格面有**共点** | 根部端那一根 | 0.1 × 该对翼厚度 | 原分布值 |
| 类型二 | 与梢部（`wingTipN`）分组的网格面有**共点**；`wingTipN` 缺失时以翼侧面自由梢端的边界线为梢部端 | 梢部端那一根 | 0.1 × 该对翼厚度 | 原分布值 |

## 部件分组名对照

| 部件类型 | 子面前缀 | 前缘面 | 侧面 | 梢面 | 结合部件 |
|---------|---------|--------|------|------|---------|
| 弹翼(wing) | `wing` | `wingLeadingEdgeN` | `wingSideSurfaceN` | `wingTipN`（**按 M2 探测**：存在=梯形翼；缺失=三角翼） | `fuselage`（弹体） |

> 🔴 **翼型运行时探测（M2，禁止写死）**：`wingTipN` 存在 = 梯形翼，不存在 = 三角翼。`{prefix}` 恒为 `wing`。三角翼无 `wingTipN`，梢部端以前缘线最外端（与侧面自由边相邻的端点）判定。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，确认对应**带序号子组名**存在（🔴 碎面合并会改超面集合，**进入本阶段前重查一次分组**，事实中心 G2；子组名缺失 → 按「不强行推理纪律」`skipped` + `note`）。
   - 翼：`fuselage`、`wingLeadingEdgeN`、`wingSideSurfaceN`（`wingTipN` **按 M2 探测**——存在=梯形翼，缺失=三角翼，此时梢部端以前缘线最外端判定）
2. 调用 `GetMissileDimensions`，获取弦长与厚度参数（🔴 返回值单位 **mm**，直接用，禁止 ×1000）。弹翼**按对取 `wings[N−1]`**（N 由组名末位数字读出，**不得各对共用**）：**C_root(N) / 厚度 thickness 取接口值**（事实中心 §2），**C_tip(N) 按对取并按翼型分叉**（M2）：三角翼（无 `wingTipN`）≈0 正常，禁止 0 代入，梢部端中间值钳制到**该对** C_root(N) 计算值（`0.02×C_root(N)`，**现算**，事实中心 §2.2.2）；梯形翼（有 `wingTipN`）按接口 C_tip(N) 正常使用；**该对半翼展也取接口值**（`wings[N−1].half_span`）。
3. 表面网格已生成：`GetAllObjectByType`(6) 返回非空。
4. **链路接口可用**：`GetConnectorsByDomains` 能返回真实网格线 ID（若返回兜底值，整个各向异性阶段按主 Skill「工具可用性判定」标记 `skipped`）。
5. 上述任一条件不满足时停止。

## 分布参数

| 参数 | 值 |
|---|---|
| 共点端间距（`headspace` 或 `tailspace`） | **`0.1 × 该对翼厚度`**（`wings[N−1].thickness`） |
| 另一端间距 | **原分布值**，由 `GetConnectorsStartAndEndUnitLenth` 读取 |
| `params` | `"1,50,1,50"`（增长率 1、层数 50） |
| `mindValue`（中间值） | **`0.02 × 该对根弦长`**（`wings[N−1].root_chord`） |

> 🔴 **2026-10-22 口径变更**：旧口径 `0.008 × 当地弦长`（共点端）/ `0.005×当地弦长` **已作废**——弹翼一律改用**厚度**基准（首层/端间距 = `0.1×厚度`，中间值 = `0.02×根弦长`）。
>
> 🔴 **增长率 = 1**：本 Skill 处理的是**前缘线**（前缘面 ∩ 侧面的公共边），属「前缘」位置——按需求，**前缘/翼梢/后缘增长率一律 1、层数 50**。只有**侧面与翼身结合处**用增长率 1.2（见 `missile-anisotropic-mesh.md` §「各位置参数」）。
> 🔴 **多对翼（N=1/2/3）逐对处理**，每对按该对的厚度与根弦长算间距与中间值。
>
> 注：真实形参只有 5 个——`UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)`；**无 `disFunc` 参数**，也无 `headRate`/`headLayer` 等独立参数（增长率/层数打包进 `params`）。分布类型由所选工具（Config / Smooth / Average）决定。
>
> ⚠️ 共点端落在网格线的 start 端还是 end 端，决定 `headspace` / `tailspace` 怎么传，**必须先判端再传参**。

## 执行步骤

1. **获取各分组网格面 ID**：
   1. 调 `GetAllSpitAssemblyGroupProperty()`，按**带序号子组名**取 `domain[].ids`（`{prefix}LeadingEdgeN`、`{prefix}SideSurfaceN`、结合部件、梢面）。
   任一返回为空 → 失败停止。

2. **识别前缘线**：通过前缘面与侧面网格面 ID 求公共边（两组共有的网格线），得到所有前缘线 ID 列表。
   - 🔴 **用批量调用取公共边（主 Skill 纪律第 3 条）**：`Set(GetConnectorsByDomains(domain_ids=<LeadingEdge 全部面 ID>)…)` 与 `Set(GetConnectorsByDomains(domain_ids=<SideSurface 全部面 ID>)…)` 各**一次批量调用**，再对两个 set 取交集即所有前缘线——禁止对每个面逐个查询。交集线列表为空 → 失败停止。
   - 与结合部件（fuselage）网格面有共点的前缘线 → **类型一**（根部端）
   - 与梢部（`wingTipN`）网格面有共点的前缘线 → **类型二**（梢部端）；`wingTipN` 缺失（三角翼）时，取前缘线最外侧端点（与侧面自由梢边相邻）所在的那根   - 不在上述两端的其他前缘线 → **中间段，跳过不处理**。

3. **读取原分布值**：对类型一、类型二各调用 `GetConnectorsStartAndEndUnitLenth`（connector_ids）→ `{"connectors":[{"id","start","end"}]}`。
   > 必须在 `UGReDimensionConfigDistribution` **之前**读取，否则原值会被覆盖。

4. **计算间距**：`edgeSpacing` = **`0.1 × 该对翼厚度`**（`wings[N−1].thickness`，**与弦长/翼型无关，根部端与梢部端取同一个值**）；`mindValue` = **`0.02 × 该对根弦长`**（三角翼梢部同样钳制 `0.02×C_root(N)`，梯形翼用实际 C_tip(N) 算，事实中心 §2.2.2）。🔴 **各对独立取厚度/弦长**（`wingLeadingEdge2` → `wings[1]`）。

5. **组装参数并设置增长分布**（类型一、类型二各执行一次）：
   - 共点端 = start 端 → `headspace` = `edgeSpacing`，`tailspace` = `e0`
   - 共点端 = end 端 → `headspace` = `s0`，`tailspace` = `edgeSpacing`
   - 调用 `UGReDimensionConfigDistribution`。

6. **对边匹配**（如需）：`UGReDimensionMatch`。

7. **平滑过渡**（如需）：`UGReDimensionSmoothDistribution(ids, headspace, tailspace, params, mindValue)`。

## 注意事项

- **只处理最两端两根前缘线**（类型一、类型二各一根），中间段一律不动。
- 共点端取 **`0.1 × 该对翼厚度`**，另一端保留原分布值，**切勿两端都写** `edgeSpacing`。
- 共点判定只比对**网点 ID**（`pointID`），ID 相同即为共点，不要用坐标近似比较。
- 展向分布**只控制内部点分布**，不更改边界分布。
- `params` 严格格式为 `"headRate,headLayer,tailRate,tailLayer"`。

## 完成标准

- 类型一与类型二各识别出一根前缘线。
- 两根线的共点端间距为 **`0.1 × 该对翼厚度`**，另一端为原分布值；中间段未被修改。
- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。