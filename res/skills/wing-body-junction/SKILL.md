---
name: wing-body-junction
description: 支架与翼身结合处各向异性网格线分布处理。该位置不修改两端分布，使用增长分布（增长率 1.2，层数 50），中间值与翼面尺寸一致。当总 Skill anisotropic-mesh-processing 路由到支架与翼身结合处处理时加载本 Skill。
aliases: [翼身结合, 支架结合, 翼身结合处, 支架网格, junction, wing-body]
tags: [CFD, 网格, 各向异性, 翼身结合, 支架]
category: CFD
version: 1.0.0
author: QtChatWidget
allowed-tools: []
---

# 支架与翼身结合处各向异性网格线分布

## 适用位置

- 支架与机翼的结合处
- 翼身结合处（wing-body junction）
- 曲率变化较大、需要局部加密的结合区域

## 前置条件

- 已获取**半展长**（1/2 b），用于尺寸计算
- 已获取翼面尺寸（中间值用）

## 尺寸参数

| 参数 | 值 |
|---|---|
| 首端间距 | 不修改两端分布 |
| 尾端间距 | 不修改两端分布 |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 分布类型（disFunc） | 0（双曲正切） |
| 中间值（mindValue） | 翼面尺寸一致 |
| 支架段 | 保证平滑分布 |

## 网格线分布设置

### 设置步骤

1. 获取支架与翼身结合处的网格线 ID。
2. 调用 `UGReDimensionConfigDistribution` 设置分布参数：

| 参数 | 值 |
|---|---|
| `headspace` | **不修改**（保持原分布值） |
| `tailspace` | **不修改**（保持原分布值） |
| `headRate` | 1.2 |
| `headLayer` | 50 |
| `tailRate` | 1.2 |
| `tailLayer` | 50 |
| `disFunc` | 0 |
| `mindValue` | 翼面尺寸 |

> **关键区别**：翼身结合处不修改首端和尾端的间距，仅通过增长率控制内部点分布。这是与弦向/展向处理最大的不同点。

3. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution`。
4. 若存在支架段，需保证与机翼区域的分布平滑衔接。

### 注意事项

- **严禁修改两端分布**——本位置的核心约束。
- 支架段若存在，需调用 `UGReDimensionSmoothDistribution` 做平滑处理。
- 保证与相邻位置（如弦向、展向分布区域）的过渡自然。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`，且两端分布未被修改。
- 支架段（若存在）平滑过渡无误。
- 任一步骤失败时停止，不继续后续步骤。
