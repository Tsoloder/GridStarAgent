---
name: nacelle-anisotropy-processing
description: 引导用户通过 MCP 工具完成吊舱（含支架）各向异性网格处理的一体化流程。当用户提到吊舱各向异性、发动机短舱、支架前缘、支架后缘、吊舱内侧、吊舱外侧、enginePylon、engineTrailingEdge、吊舱前缘加密时必须使用。
aliases: [吊舱各向异性, 发动机短舱各向异性, nacelle anisotropy, 支架前缘分布, 支架后缘分布, 吊舱内侧分布, 吊舱外侧分布, enginePylon, engineTrailingEdge]
tags: [CFD, 网格, 各向异性, 吊舱, 发动机短舱, 支架, nacelle, pylon]
category: Aircraft
version: 1.0.0
author: GridStarAgent
allowed-tools: []
---

# 吊舱各向异性网格处理工作流

本 Skill 封装了基于 **NacelleAnisotropicProcessor** 的吊舱（含发动机短舱和支架）各向异性网格处理一体化流程。调用 `NacelleAnisoProcessNacelleAnisotropy()` 一次性完成全部处理。

## 前置条件

必须在以下步骤完成后才能进入本 Skill：

1. 数模已导入（`ImportCADFile`）
2. 自动部件分割已完成（`ProcessWithServer` 或等效流程）
3. 发动机子部件分割已完成，以下分组已存在：
   - `engineInner`（吊舱内侧）
   - `engineOuter`（吊舱外侧）
   - `enginePylonInner`（支架内侧）
   - `enginePylonOuter`（支架外侧）
   - `enginePylon`（支架斜平面）
   - `engineTrailingEdge`（吊舱后缘）
   - `enginePylonTrailingEdge`（支架后缘）
4. 表面网格已生成（通过 `UGSur` 或 `GenerateSurMeshBySpitAssemblyGroupProperty`）
5. 机翼各向异性处理已完成（`WingAnisoProcessWingAnisotropy`）

## 一体化入口

直接调用 **`NacelleAnisoProcessNacelleAnisotropy()`** 即可。该工具无参数，内部依次执行：

| 内部步骤 | 功能 | 说明 |
|---|---|---|
| 1 | 获取尺寸计算 MAC | 读取机身长度、半展长、翼根/翼尖弦长 |
| 2 | 分类识别 9 类网格线 | 按拓扑将吊舱网格线分为 outerOnly/innerOnly/leadingEdges/outerPylonInter/... |
| 3.1 | 吊舱外侧线平滑分布 | outerOnly，hs=ts=0.1%×MAC，增长 1.2，mid=4%×MAC |
| 3.2 | 吊舱内侧线平滑分布 | innerOnly，同上 |
| 3.3 | 吊舱前缘线平滑分布 | 三分支（支架斜平面/共点/不共点） |
| 3.4 | 吊舱外侧与支架交线平滑分布 | outerPylonInter，TE 端 0.1%×MAC，另一端不变 |
| 3.5 | 支架斜平面与支架内/外侧交线平滑分布 | pylonSlantInter，同上 |
| 3.6 | 吊舱后缘与外侧/支架斜平面交线平滑分布 | 三分支 |
| 3.7 | 吊舱后缘与内侧交线 | 配对拷贝外侧分布 |
| 3.8 | 只属于吊舱后缘/支架斜平面与支架后缘交线 | 设点数为 5 |
| 3.9 | 支架前缘线平滑分布 | pylonLeadingEdge，间距不变，增长 1.2 |
| 3.10 | 支架后缘线平滑分布 | pylonTELines，取自短边端点间距，增长 1.2 |
| 4 | 创建各向异性线组 | 五组线(type=1)，首层高度 0.1%×MAC 或 0.1%×半展长 |
| 5 | 生成各向异性域 | 对吊舱/支架网格面(type=0)，增长率 1.2 |
| 6 | 狭长面处理 | 对 enginePylonTrailingEdge + engineTrailingEdge 做狭长面处理 |

### 返回格式示例

```json
{
  "status": "success",
  "mac": 3.59,
  "steps": {
    "step1_dimensions": {"status":"success","mac":3.59},
    "step2_classify": {"status":"success","outer_only":[...],"inner_only":[...],...},
    "step3_1_outer_only": {"status":"success","connector_count":12},
    "step3_9_pylon_leading_edge": {"status":"success","connector_count":4},
    "step3_10_pylon_te_line": {"status":"success","connector_count":2},
    "step4_aniso_line_groups": {"status":"success","group_count":5},
    "step5_aniso_domains": {"status":"success","domain_count":8},
    "step6_slender_face": {"status":"success","domain_count":2}
  }
}
```

### 失败处理

- `status` 为 `"failed"` 时停止，检查 `message` 字段获取具体原因
- 常见失败原因：发动机子部件分割未完成、表面网格未生成、分组缺失
- 若某一步返回 `"failed"`，后续步骤不再执行，应检查前置条件是否满足

## 完成标准

- `NacelleAnisoProcessNacelleAnisotropy` 返回 `status` 为 `"success"`
- MAC 值有效（非零）
- 步骤 4-6 均返回 `success`
- 吊舱/支架网格面已生成各向异性网格
- 后缘面已做狭长面处理
