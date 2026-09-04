---
name: spanwise-direction
description: 机翼展向（spanwise direction）各向异性网格线分布处理。处理机翼翼根到翼梢方向的网格线加密，靠近翼根处和翼梢处的分布均为 0.1% × 半展长，中间值与翼面尺寸一致，其他端使用原网格线端分布。当总 Skill anisotropic-mesh-processing 路由到机翼展向处理时加载本 Skill。
aliases: [展向分布, 展向加密, spanwise, 机翼展向, 翼根加密, 翼梢加密]
tags: [CFD, 网格, 各向异性, 展向, 机翼]
category: CFD
version: 1.0.0
author: QtChatWidget
allowed-tools: []
---

# 机翼展向各向异性网格线分布

## 适用位置

- 机翼展向方向（翼根至翼梢）
- 适用于机翼的展向网格线

## 前置条件

- 已获取机翼的**半展长**（1/2 b）
- 已获取翼面尺寸（中间值用）

## 尺寸参数

| 参数 | 值 |
|---|---|
| 翼根端间距 | 0.1% × 半展长 = 0.1% × 1/2 b |
| 翼梢端间距 | 0.1% × 半展长 = 0.1% × 1/2 b |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 分布类型（disFunc） | 0（双曲正切） |
| 中间值（mindValue） | 翼面尺寸一致 |
| 其他端 | 使用原网格线端分布 |

## 网格线分布设置

### 设置步骤

1. 获取机翼展向方向的网格线 ID。
2. 调用 `UGReDimensionConfigDistribution` 设置分布参数：

| 参数 | 值 |
|---|---|
| `headspace` | 0.1% × 半展长 |
| `tailspace` | 0.1% × 半展长 |
| `headRate` | 1.2 |
| `headLayer` | 50 |
| `tailRate` | 1.2 |
| `tailLayer` | 50 |
| `disFunc` | 0 |
| `mindValue` | 翼面尺寸 |

> **注意**：展向分布的"其他端"（非翼根/翼梢侧）应保持原网格线端分布不变，不要修改。

3. 如需对边匹配，调用 `UGReDimensionMatch`。
4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution`。

### 注意事项

- 展向分布**只控制内部点分布**，不更改边界分布。
- 翼根和翼梢的间距计算均使用同一半展长值，除非用户明确指定不同值。
- 其他端保留原分布，避免过度约束。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 其他端分布未被修改。
- 任一步骤失败时停止，不继续后续步骤。
