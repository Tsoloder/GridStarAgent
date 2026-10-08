---
name: missile-trailing-edge-processing
description: 引导用户通过 MCP 工具完成导弹弹翼后缘分部件网格面的碎边合并与结构网格面装配。导弹仅存在 Type1（梢部相邻型，2-4 条边）。适用于弹翼(wing)部件。当用户提到弹翼后缘面处理、trailing edge、后缘分布、梢部交线时必须使用。
aliases: [翼后缘面处理, 后缘网格, trailing edge, 后缘分布, 梢部交线]
tags: [CFD, 网格, 后缘面, 翼, 导弹]
category: CFD
version: 2.1.0
author: Tsolodancer
allowed-tools: []
---

# 导弹弹翼后缘面处理工作流

本 Skill 独立处理导弹弹翼后缘面的碎边合并与结构网格面装配。导弹后缘面仅有 **Type1（梢部相邻型）**一种类型，由 4 条网格线（2 条长边 L1/L2、2 条短边 S1/S2）组成。

## 部件分组名对照

| 部件类型 | 后缘面分组 | 梢面分组 | 结合部件 |
|---------|-----------|---------|---------|
| 弹翼(wing) | `wingTrailingEdge` | `wingTip`（仅梯形翼存在；三角翼无，以翼侧面自由梢端为准） | `fuselage`（弹体） |

> 按 需求文件 1.4：翼梢面只存在于梯形翼，三角翼没有 `wingTip`。三角翼后缘面的梢部端即翼侧面（`wingSideSurface`）自由梢端，Type1 的"梢部相邻"以该自由边为相邻对象。
> 🔴 当前型号无舵、无舵轴（2026-10-08 确认）：不处理 `finTrailingEdge` / `finTop` / `finshaft`。

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema。
2. 确认对应分组已存在（见上表）。
3. 读取 `references/type1.md`，了解 Type1 的完整步骤。

## 后端可用性判定（收敛引用，按返回值动态生效）

本链路接口状态**以主 Skill `missile-cfd-meshing-workflow` 的「工具可用性判定」通用规则为准**：任何工具返回恒定 `true`/`false`/空值即视为不可用 → 跳过/回退 + `note`，禁止重试、禁止换参。

**处理方式（三选一，都在阶段 `note` 记录）**：

1. **跳过**：后缘面分组有效面 ≤1（如 `wingTrailingEdge=[0,18]` 剔除 0 后仅 1 个面，边界本就不碎）→ 标记该阶段 `skipped`，进入体网格块创建。
2. **走完整流程**：`MergeEdgesByDomain` **已实现**→ 按 `references/type1.md` 正常执行。
3. **🔴 装配失败保护**：`AssembleConnectorsToDomain` 返回 `"false"` 时——**不要反复换线序**（线序要靠端点坐标校验，而 `GetStartAndEndPointByConnectors` 恒 `"true"`，无法验证）→ 立即停止，标记该阶段 `skipped` + `note`。

> 🔴 **破坏性操作禁令（强制）**：`DeleteDomain`（删除原后缘面）**必须在结构面装配成功之后执行**，**严禁"先删后装"**。若因步骤顺序是「先删后装」，装配失败导致**后缘面永久丢失**（少一个面且无替代）。装配失败时**不得执行删除**。
>
> 一旦相关接口返回真实数据，恢复 `references/type1.md` 完整 8 步流程。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，已存在对应后缘面、结合部件分组（梢面分组 `wingTip` 仅梯形翼存在，三角翼缺失时不阻塞，按自由梢端处理）。🔴 **只有主组（`wing`）而无 `wingTrailingEdge` 子面分组 → 本阶段直接 `skipped` + `note`，不强行推理**：禁止为定位后缘面做几何反推、禁止对候选面逐个调 `MergeEdgesByDomain` 试探（那会把"没有子面"变成长时间探索）。**后端给了后缘子面分组才做；没给就跳过。**
2. 通过 `GetAllObjectByType`(6)，返回值中有网格面。
3. **当地弦长**：只认 `GetMissileDimensions`（弹翼 `wing_root_chord`/`wing_tip_chord`）**或用户直接输入**。⚠️ **禁止用分组属性 `targetSize` 反推**。该接口不可用（恒 `"true"`）时 → **向用户索取当地弦长**；用户亦无法提供 → 本阶段标记 `skipped` + `note`。
4. 上述任一条件不满足时停止。

## 默认参数

- 当地弦长：**只认 `GetMissileDimensions` 的 `wing_*_chord`，或用户直接输入**。
- ⚠️ **禁用估算**：不得用分组 `targetSize`×10 或 `0.02×弹径` 反推当地弦长。据此推算的弹径 D 可能偏差 **2×**（`targetSize` 的系数与 需求文件 3.2 的 `0.05×D` / `1/10` 对不上），会直接带偏 `bodySpacing` / `rootSpacing`。
- `GetMissileDimensions` 不可用 → 向用户索取；拿不到 → 阶段 `skipped` + `note`（**不得用估算值凑参数**）。

## 间距设置

| 位置 | 间距 |
| --- | --- |
| 靠近梢部端 | `bodySpacing` = 0.008 × 当地弦长 |
| 靠近根部侧、中间值（mindValue） | `rootSpacing` = 0.02 × 当地弦长 |
| 分布参数 params | `"1.2,10,1.2,10"`（增长率默认 1.2） |

## 执行步骤

### 第0步：取后缘面与其相邻面

导弹后缘面**仅有 Type1（梢部相邻型）**，**无需类型判定** —— 不得调用任何后缘类型判定类工具（尤其是其他模型专属的流程类工具，见主 Skill「工具使用范围」）。

用 `GetAllSpitAssemblyGroupProperty` 按组名取 `domain[].ids`：

- **后缘面**：`wingTrailingEdge`
- **梢部面**：`wingTip`（**仅梯形翼存在**；三角翼没有，其"梢部相邻"以翼侧面的自由梢端为相邻对象）
- **结合面**：`fuselage`

> 🔴 当前型号无舵（2026-10-08 确认）：仅对弹翼（wing）执行一次。

### 处理 Type1

Type1（梢部相邻）有 4 条网格线（2 长边 + 2 短边），使用 `MergeEdgesByDomain`。

> `MergeEdgesByDomain` 需**前置满足**（网格已生成 + 后缘面存在）：满足时返回 `{"longids":[…],"shortids":[…]}`，否则返回 `"false"`。返回 `"false"` 即前置不满足，按「工具可用性判定」降级处理，**勿重试、勿换参**。

> 读取 `references/type1.md` 获取完整步骤。

## 完成标准

- 所有工具调用返回 `success` 为 `true`。
- 任一步骤失败（`success` 为 `false`）时，按主 Skill「工具可用性判定」的**通用规则**处理：返回兜底值（恒定 `true`/`false`/空）即视为接口不可用 → 走跳过/回退，**不重试**；其余情况则停止，不继续后续步骤。
- Type1：4 条网格线已装配为结构面（跳过时：阶段已 `skipped` 并在 `note` 记录原因，进入下一阶段）。
- 🔴 **后缘面保全**：装配失败时，**原后缘面必须保持未删除**（`DeleteDomain` 只能在装配成功后执行）。报告阶段结果时须说明后缘面是否完整——若按"先删后装"造成后缘面丢失。