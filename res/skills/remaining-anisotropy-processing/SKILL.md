---
name: remaining-anisotropy-processing
description: 引导用户通过 MCP 工具完成其余网格面各向异性处理。当用户提到剩余面各向异性、其余面处理、机身各向异性、机尾各向异性、剩余网格处理时必须使用。
aliases: [其余面各向异性, 剩余网格处理, 机身各向异性, remaining anisotropy, fuselage anisotropic]
tags: [CFD, 网格, 各向异性, 剩余面, 机身]
category: Aircraft
version: 1.0.0
author: GridStarAgent
allowed-tools: []
---

# 其余面各向异性网格处理工作流

本 Skill 封装了基于 **RemainingAnisotropicProcessor** 的其余网格面各向异性处理。调用 `RemainingAnisoProcessRemainingAnisotropy()` 一次性完成处理。

## 前置条件

必须在以下步骤完成后才能进入本 Skill：

1. 机翼各向异性处理已完成（`WingAnisoProcessWingAnisotropy`）
2. 吊舱各向异性处理已完成（`NacelleAnisoProcessNacelleAnisotropy`，若存在）
3. 尾翼各向异性处理已完成（`TailAnisoProcessTailAnisotropy`，若存在）
4. 表面网格已生成

## 处理逻辑

自动收集所有未被机翼/吊舱/尾翼各向异性处理器覆盖的网格面（如机身、机尾、弹头等），执行：

```
[UGNewANisoDomain][0][剩余面ID][0][1][1][1.2]
```

### 已排除的分组

| 来源 | 分组名 |
|---|---|
| 机翼各向异性 | wingUpper, wingLower, wingTip |
| 吊舱各向异性 | engineInner, engineOuter, enginePylonInner, enginePylonOuter, enginePylonDefault |
| 尾翼各向异性 | verticalTailRight, verticalTailLeft, verticalTailTip, horizontalTailUpper, horizontalTailLower, horizontalTailTip |

### 返回格式

```json
{
  "status": "success|failed",
  "domain_count": 42
}
```

## 完成标准

- `RemainingAnisoProcessRemainingAnisotropy` 返回 `status` 为 `"success"`
- 剩余网格面已生成各向异性网格
