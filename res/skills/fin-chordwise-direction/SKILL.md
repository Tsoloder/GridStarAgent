---
name: fin-chordwise-direction
description: 导弹翼/舵弦向（chordwise direction）各向异性网格线分布处理。处理翼/舵前缘到后缘方向的网格线加密，靠近前缘处和后缘处的分布均为 0.008×当地弦长，中间值与翼/舵面尺寸一致。适用于弹翼(fin)和舵(rudder)两类部件。当总 Skill missile-anisotropic-mesh 路由到弦向处理时加载本 Skill。
aliases: [翼弦向分布, 舵弦向分布, 弦向加密, fin chordwise, rudder chordwise, 前缘加密, 后缘加密]
tags: [CFD, 网格, 各向异性, 弦向, 导弹, 翼, 舵]
category: CFD
version: 2.0.0
author: Tsolodancer
allowed-tools: []
---

# 导弹翼/舵弦向各向异性网格线分布

## 适用位置

- 导弹翼/舵弦向方向（前缘至后缘的流向）
- 适用于翼/舵的上表面、下表面的弦向网格线
- 两种部件：弹翼(`fin`) 和 舵(`rudder` / 即 `tailRudder`)

## 前置条件

- 已通过 `GetMissileModelParameters` 获取弦长参数
- 翼用 `finRootChord`(C_root) / `finTipChord`(C_tip)
- 舵用自身弦长（服务端按 `tailRudder` 分组测量）
- 已获取翼/舵面尺寸（中间值用）
- 表面网格已生成

## 部件分组名对照

| 部件类型 | 分组前缀 | 上表面 | 下表面 | 后缘面 | 梢面 |
|---------|---------|--------|--------|--------|------|
| 弹翼(fin) | `fin` | `finUpperSurface` | `finLowerSurface` | `finTrailingEdge` | `finTip` |
| 舵(rudder) | `rudder` | `rudderUpperSurface` | `rudderLowerSurface` | `rudderTrailingEdge` | `rudderTip` |

## 尺寸参数

| 参数 | 值 |
|---|---|
| 前缘端间距（首层高度） | 0.008 × 当地弦长 |
| 后缘端间距（尾端高度） | 0.008 × 当地弦长 |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 分布类型（disFunc） | 0（双曲正切） |
| 中间值（mindValue） | 翼/舵面尺寸一致（0.02 × 当地弦长） |

## 网格线分布设置

### 设置步骤

1. **获取弦向网格线 ID**：根据当前处理的部件类型，用对应分组前缀获取上/下表面的弦向网格线：

```
# 例：弹翼(fin)上表面
upperDomains = GetSpliteAssemlyDomains("finUpperSurface")   # → {"domains": [101, 102, ...]}
chordwiseConnectors = []
for id in upperDomains["domains"]:
    result = GetConnectorsByDomain(id)                       # → {"ids": [201, 202, ...]}
    chordwiseConnectors.extend(result["ids"])

# 下表面同理，用 "finLowerSurface" 或 "rudderLowerSurface"
```

2. 调用 `UGReDimensionConfigDistribution` 设置分布参数：

| 参数 | 值 |
|---|---|
| `headspace` | 0.008 × 当地弦长 |
| `tailspace` | 0.008 × 当地弦长 |
| `headRate` | 1.2 |
| `headLayer` | 50 |
| `tailRate` | 1.2 |
| `tailLayer` | 50 |
| `disFunc` | 0 |
| `mindValue` | 翼/舵面尺寸（0.02 × 当地弦长） |

3. 如需对边匹配，调用 `UGReDimensionMatch`。
4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution`。

### 注意事项

- 弦向分布**只控制内部点分布**，不更改边界分布。
- 若根部和梢部的当地弦长不同（`C_root` ≠ `C_tip`），需沿展向分段分别计算前缘/后缘间距。
- 前缘点数 ≥ 7，梢部点数 ≥ 7（对齐 t.py 表面网格参数要求）。
- **翼和舵各需执行一次**，分组名不同但逻辑相同。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 对边匹配无误。
- 任一步骤失败时停止，不继续后续步骤。