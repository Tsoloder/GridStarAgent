---
name: trailing-edge-processing
description: 引导用户通过 MCP 工具完成飞行器机翼后缘分部件网格面的碎边合并与结构网格面装配。当用户提到后缘面处理、后缘网格、trailing edge、后缘分布、翼梢交线时必须使用。
aliases: [后缘面处理, 后缘网格, trailing edge, 后缘分布, 翼梢交线]
tags: [CFD, 网格, 后缘面, 机翼]
category: CFD
version: 1.0.0
author: QtChatWidget
allowed-tools: []
---

# 飞行器后缘面处理工作流

本 Skill 独立处理飞行器机翼后缘面的碎边合并与结构网格面装配。处理流程分为**类型一（翼梢相邻）**和**类型二（吊舱-机身相邻）**两种，两者步骤完全不同，**严禁混淆**。

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema。
2. 确认以下分组已存在：`jiyi_trailing_edge`、`jiyi_wing_tip`、`engine`、`fuselage`。
3. 读取 `references/type1.md` 和 `references/type2.md`，了解两种类型的完整步骤。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，已存在 `jiyi_trailing_edge`、`jiyi_wing_tip`、`engine`、`fuselage` 分组。
2. 通过 `GetModelParameters`，返回值中的参数均为有效值。
3. 通过 `GetAllObjectByType`(6)，返回值中有网格面。
4. 上述任一条件不满足时停止。

## 默认参数

- 半展长：586.10
- 当地弦长：144.74

## 间距设置

| 位置 | 间距 |
| --- | --- |
| 类型一靠近翼梢端、类型二靠近机身边 | `bodySpacing` = 0.5861（0.1% × 586.10） |
| 有共点的网格线侧（翼根侧）、中间值（mindValue） | `rootSpacing` = 2.8948（2% × 144.74） |
| 分布参数 params | `"1.2,10,1.2,10"`（增长率默认 1.2） |

## 执行步骤

### 第0步：类型判定

1. 调用 `ClassifyTrailingEdgeDomains`，得到：
   - 所有分组 ID 列表（`te_domains`、`wing_tip_domains`、`engine_domains`、`fuselage_domains`）
   - 每个后缘面的 `classifications`：`domain_id`、`type`（1 或 2）、`wing_tip_id`（仅 type=1）
2. 将后缘面按类型分为两组：类型一列表（type=1）和类型二列表（type=2）。
3. **先逐个处理所有类型一的后缘面**，全部完成后，**再逐个处理所有类型二的后缘面**。

### 处理类型一

类型一（翼梢相邻）有 4 条网格线（2 长边 + 2 短边），使用 `MergeEdgesByDomain`。

> 读取 `references/type1.md` 获取完整步骤。

处理完所有类型一后，**在处理类型二之前**：

> ⚠️ **关键提醒：先调用 `read_skill_resource("trailing-edge-processing", "references/type2.md")` 重新加载类型二的参考文件。** 类型二有 6 条边，与类型一完全不同，**严禁使用 `MergeEdgesByDomain`**，第一步必须是 `IdentifyType2Roles`。

### 处理类型二

类型二（吊舱-机身相邻）有 6 条网格线，**严禁调用 `MergeEdgesByDomain`**。

> 读取 `references/type2.md` 获取完整步骤。

## 完成标准

- 所有工具调用返回 `success` 为 `true`。
- 任一步骤失败（`success` 为 `false`）时停止，不继续后续步骤。
- 类型一：4 条网格线已装配为结构面；类型二：6 条网格线已装配为结构面。
