---
name: anisotropic-mesh-processing
description: 各向异性网格处理的总编排 Skill。负责判断需要处理哪些位置（机翼弦向、机翼展向、支架与翼身结合处、吊舱流向方向、后缘），读取对应子 Skill 的完整流程并串联执行，最后做全局一致性检查。当用户提到各向异性、网格线分布、前缘加密、后缘加密、翼梢加密、翼根加密、展向分布、弦向分布、流向加密时须优先加载本 Skill。
aliases: [各向异性, 网格线分布, 各向异性网格, 展向分布, 弦向分布, 流向加密, 翼梢加密, 翼根加密]
tags: [CFD, 网格, 各向异性, 网格线分布, 机翼, 吊舱]
category: CFD
version: 1.0.0
author: QtChatWidget
allowed-tools: []
---

# 各向异性网格处理总 Skill

本 Skill 是各向异性网格处理的总编排入口，负责：

1. 判断用户需要处理哪些位置。
2. 读取对应子 Skill 的完整流程并执行。
3. 所有位置处理完成后，做全局一致性检查。

## 前置条件

**必须在以下步骤完成后方可进入本 Skill：**

- 表面网格已生成
- 关键几何参数已计算（当地弦长、半展长、MAC）
- 若用户对网格数量没有要求，可不做各向异性处理

## 位置判断与路由

根据用户输入或模型特征，判断需要各向异性处理的位置列表。每个位置对应一个独立子 Skill，通过 `read_skill_resource(skill_name, "SKILL.md")` 获取完整流程。

| 位置 | 子 Skill 名称 | 触发关键词 |
|---|---|---|
| 机翼弦向（前缘↔后缘） | `chordwise-direction` | 弦向、弦长方向、前缘后缘、chordwise |
| 机翼展向（翼根↔翼梢） | `spanwise-direction` | 展向、翼根、翼梢、半展长、spanwise |
| 支架与翼身结合处 | `wing-body-junction` | 翼身结合、支架结合、junction |
| 吊舱流向方向（前缘↔后缘） | `nacelle-streamwise` | 吊舱、nacelle、发动机短舱、流向 |
| 后缘（狭长面） | `trailing-edge-processing` | 后缘面、trailing edge（已有独立 Skill） |

## 执行流程

### 步骤一：读取公共参数

```plaintext
各子 Skill 共用的参数默认值：
- 增长率（headRate / tailRate）：1.2
- 层数（headLayer / tailLayer）：50
- 分布类型（disFunc）：0（双曲正切）
- 核心原则：各向异性只控制内部点分布，不更改边界分布
```

### 步骤二：按位置逐项处理

对判定需要处理的每个位置：

1. 用 `read_skill_resource("对应子Skill名", "SKILL.md")` 读取完整流程。
2. 按子 Skill 中的步骤执行工具调用。
3. 记录每个位置的处理结果（成功/失败）。

> **优先级规则**：先处理**弦向/流向方向**（前缘后缘加密），再处理**展向/结合处**（翼根翼梢加密），最后处理**后缘**（狭长面处理）。

### 步骤三：全局一致性检查

所有位置处理完成后，检查以下内容：

1. **边界一致性**：各子 Skill 修改的内部点分布不能影响边界分布（核心原则）。
2. **相邻区域过渡**：相邻位置的网格线分布是否平滑过渡。若不满足，使用 `UGReDimensionSmoothDistribution` 做平滑处理。
3. **对边匹配**：涉及狭长面的区域，需保证对边的点数及分布相同。

## 完成标准

- 所有位置处理完成，工具调用均返回 `success` 为 `true`。
- 全局一致性检查通过。
- 任一步骤失败时停止，不继续后续步骤。

## 参考

- 子 Skill `chordwise-direction`：机翼弦向网格线分布
- 子 Skill `spanwise-direction`：机翼展向网格线分布
- 子 Skill `wing-body-junction`：支架与翼身结合处网格线分布
- 子 Skill `nacelle-streamwise`：吊舱流向网格线分布
- 子 Skill `trailing-edge-processing`：后缘狭长面处理（已有）
