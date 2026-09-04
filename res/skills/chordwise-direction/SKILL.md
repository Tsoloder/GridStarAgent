---
name: chordwise-direction
description: 机翼弦向（chordwise direction）各向异性网格线分布处理。处理机翼前缘到后缘方向的网格线加密，靠近前缘处和后缘处的分布均为 0.1% × 当地弦长，中间值与翼面尺寸一致。当总 Skill anisotropic-mesh-processing 路由到机翼弦向处理时加载本 Skill。
aliases: [弦向分布, 弦向加密, chordwise, 机翼弦向, 前缘加密]
tags: [CFD, 网格, 各向异性, 弦向, 机翼]
category: CFD
version: 1.0.0
author: QtChatWidget
allowed-tools: []
---

# 机翼弦向各向异性网格线分布

## 适用位置

- 机翼弦向方向（前缘至后缘的流向）
- 适用于机翼上翼面、下翼面的弦向网格线

## 前置条件

- 已获取机翼的**当地弦长**（沿展向各截面可能不同）
- 已获取翼面尺寸（中间值用）

## 尺寸参数

| 参数 | 值 |
|---|---|
| 前缘端间距（首层高度） | 0.1% × 当地弦长 |
| 后缘端间距（尾端高度） | 0.1% × 当地弦长 |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 分布类型（disFunc） | 0（双曲正切） |
| 中间值（mindValue） | 翼面尺寸一致 |

## 网格线分布设置

### 设置步骤

1. 获取机翼弦向方向的网格线 ID。
2. 调用 `UGReDimensionConfigDistribution` 设置分布参数：

| 参数 | 值 |
|---|---|
| `headspace` | 0.1% × 当地弦长 |
| `tailspace` | 0.1% × 当地弦长 |
| `headRate` | 1.2 |
| `headLayer` | 50 |
| `tailRate` | 1.2 |
| `tailLayer` | 50 |
| `disFunc` | 0 |
| `mindValue` | 翼面尺寸 |

3. 如需对边匹配，调用 `UGReDimensionMatch`。
4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution`。

### 注意事项

- 弦向分布**只控制内部点分布**，不更改边界分布。
- 若翼根和翼尖的当地弦长不同，需分别计算前缘/后缘间距。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 对边匹配无误。
- 任一步骤失败时停止，不继续后续步骤。
