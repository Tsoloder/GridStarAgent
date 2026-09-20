---
name: fin-trailing-edge-processing
description: 引导用户通过 MCP 工具完成导弹翼/舵后缘分部件网格面的碎边合并与结构网格面装配。导弹仅存在 Type1（梢部相邻型，2-4 条边）。适用于弹翼(fin)和舵(rudder)两类部件。当用户提到翼后缘面处理、舵后缘面处理、trailing edge、后缘分布、梢部交线时必须使用。
aliases: [翼后缘面处理, 舵后缘面处理, 后缘网格, trailing edge, 后缘分布, 梢部交线]
tags: [CFD, 网格, 后缘面, 翼, 舵, 导弹]
category: CFD
version: 2.0.0
author: Tsolodancer
allowed-tools: []
---

# 导弹翼/舵后缘面处理工作流

本 Skill 独立处理导弹翼/舵后缘面的碎边合并与结构网格面装配。导弹后缘面仅有 **Type1（梢部相邻型）**一种类型，由 4 条网格线（2 条长边 L1/L2、2 条短边 S1/S2）组成。

## 部件分组名对照

| 部件类型 | 后缘面分组 | 梢面分组 | 结合部件 |
|---------|-----------|---------|---------|
| 弹翼(fin) | `finTrailingEdge` | `finTip` | `body`（弹体） |
| 舵(rudder) | `rudderTrailingEdge` | `rudderTip` | `body` 或 `rudderShaft`（舵轴） |

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema。
2. 确认对应分组已存在（见上表）。
3. 读取 `references/type1.md`，了解 Type1 的完整步骤。

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，已存在对应后缘面、梢部、结合部件分组。
2. 通过 `GetMissileModelParameters`，返回值中的参数均为有效值。
3. 通过 `GetAllObjectByType`(6)，返回值中有网格面。
4. 上述任一条件不满足时停止。

## 默认参数

- 当地弦长：由 `GetMissileModelParameters` 返回

## 间距设置

| 位置 | 间距 |
| --- | --- |
| 靠近梢部端 | `bodySpacing` = 0.008 × 当地弦长 |
| 靠近根部侧、中间值（mindValue） | `rootSpacing` = 0.02 × 当地弦长 |
| 分布参数 params | `"1.2,10,1.2,10"`（增长率默认 1.2） |

## 执行步骤

### 第0步：类型判定

1. 调用 `ClassifyTrailingEdgeDomains`，得到：
   - 所有分组 ID 列表（`te_domains`、`tip_domains`、`body_domains`）
   - 每个后缘面的 `classifications`：`domain_id`、`type`（导弹仅返回 1）、`tip_id`
2. 所有后缘面均为 Type1，逐个处理。
3. **翼(fin)和舵(rudder)各执行一次**。

### 处理 Type1

Type1（梢部相邻）有 4 条网格线（2 长边 + 2 短边），使用 `MergeEdgesByDomain`。

> 读取 `references/type1.md` 获取完整步骤。

## 完成标准

- 所有工具调用返回 `success` 为 `true`。
- 任一步骤失败（`success` 为 `false`）时停止，不继续后续步骤。
- Type1：4 条网格线已装配为结构面。