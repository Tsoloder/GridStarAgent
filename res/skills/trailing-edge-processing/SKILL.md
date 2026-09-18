---
name: trailing-edge-processing
description: 引导用户通过 MCP 工具完成飞行器机翼后缘分部件网格面的碎边合并与结构网格面装配。当用户提到后缘面处理、后缘网格、trailing edge、后缘分布、翼梢交线时必须使用。
aliases: [后缘面处理, 后缘网格, trailing edge, 后缘分布, 翼梢交线]
tags: [CFD, 网格, 后缘面, 机翼]
category: CFD
version: 1.0.0
author: GridStarAgent
allowed-tools: []
---

# 飞行器后缘面处理工作流

本 Skill 独立处理飞行器机翼后缘面的碎边合并与结构网格面装配。处理流程分为**类型一（翼梢相邻）**和**类型二（吊舱-机身相邻）**两种，两者步骤完全不同，**严禁混淆**。

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema。
2. 确认以下分组已存在：`wingTrailingEdge`、`wingTip`、`enginePylonTrailingEdge`、`fuselage`。
3. 读取 `references/type1.md` 和 `references/type2.md`，了解两种类型的完整步骤。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，已存在 `wingTrailingEdge`、`wingTip`、`enginePylonTrailingEdge`、`fuselage` 分组。
2. 通过 `GetModelParameters`，返回值中的参数均为有效值。
3. 通过 `GetAllObjectByType`(6)，返回值中有网格面。
4. 上述任一条件不满足时停止。

## 默认参数

- 半展长：14.89
- 当地弦长：3.59

## 间距设置

> ⚠️ 以下默认值基于 `GetModelParameters` 返回的实际模型参数计算。若实际模型参数不同，请按公式重新计算：
> - `bodySpacing` = 0.1% × 半展长
> - `rootSpacing` = 2% × 当地弦长

| 位置 | 间距 |
| --- | --- |
| 类型一靠近翼梢端、类型二靠近机身边 | `bodySpacing` = 0.01489（0.1% × 14.89） |
| 有共点的网格线侧（翼根侧）、中间值（mindValue） | `rootSpacing` = 0.0718（2% × 3.59） |
| 分布参数 params | `"1.2,10,1.2,10"`（增长率默认 1.2） |

## 执行步骤

### 第0步：优先尝试全自动批量处理

1. 调用 `GetModelParameters` 获取 `wing_half_span`（半展长）和 `mac`（当地弦长）
2. 按公式计算间距参数：
   - `bodySpacing` = 0.1% × 半展长
   - `rootSpacing` = 2% × 当地弦长
3. 调用 `ProcessAllTrailingEdges(halfSpan, localChord, bodySpacing, rootSpacing, params)`
4. 检查返回的 `results` 数组：
   - 如果所有条目均为 `status=success` → **处理完成**，输出汇总报告
   - 如果有 `status=failed` 或 `status=skipped` 的条目 → **回退到下方第1步的分步流程**，仅对失败/跳过的后缘面逐个处理
5. **批量处理失败不回退到另一个批量处理**。回退后按原流程先处理所有类型一，再处理所有类型二。

### 第1步（回退）：类型判定

> ⚠️ 仅当批量处理（第0步）有失败/跳过结果时执行以下步骤。

1. 调用 `GetModelParameters` 获取半展长和当地弦长，按公式计算 `bodySpacing` 和 `rootSpacing`
2. 调用 `ProcessAllTrailingEdges(halfSpan, localChord, bodySpacing, rootSpacing, params)`
3. 检查返回结果：
   - 如果全部返回 `status=success` → **完成**，输出处理报告
   - 如果有 `status=failed` 或 `status=skipped` → **回退到下方第1步的分步流程**，对失败/跳过的后缘面单独处理
4. **批量处理失败不回退到另一个批量处理**，而是逐面分步处理

### 处理类型一

类型一（翼梢相邻）有 4 条网格线（2 长边 + 2 短边），使用 `MergeEdgesByDomain`。

> 读取 `references/type1.md` 获取完整步骤。

处理完所有类型一后，**在处理类型二之前**：

> ⚠️ **关键提醒：先调用 `read_skill_resource("trailing-edge-processing", "references/type2.md")` 重新加载类型二的参考文件。** 类型二有 6 条边，与类型一完全不同，**严禁使用 `MergeEdgesByDomain`**。角色判定使用 `IdentifyType2Roles` 获取角色线 ID 和端点信息，失败时回退到分组网格线求交集 + 端点 ID 对比（见 type2.md 步骤 1 方式二）。调用 `ProcessTrailingEdgeType2` 前必须先调用 `IdentifyType2Roles` 获取参数。

### 处理类型二

类型二（吊舱-机身相邻）有 6 条网格线，**严禁调用 `MergeEdgesByDomain`**。

> 读取 `references/type2.md` 获取完整步骤。

## 完成标准

- 所有工具调用返回 `success` 为 `true`。
- 任一步骤失败（`success` 为 `false`）时停止，不继续后续步骤。
- 类型一：4 条网格线已装配为结构面；类型二：6 条网格线已装配为结构面。
