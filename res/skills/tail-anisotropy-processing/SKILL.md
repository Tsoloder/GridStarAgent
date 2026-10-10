---
name: tail-anisotropy-processing
description: 引导用户通过 MCP 工具完成尾翼（平尾 + 立尾）各向异性网格处理的一体化流程。当用户提到尾翼各向异性、平尾、立尾、垂直尾翼、水平尾翼、horizontalTail、verticalTail、尾翼前缘、尾翼后缘加密时必须使用。
aliases: [尾翼各向异性, 平尾各向异性, 立尾各向异性, tail anisotropy, horizontalTail, verticalTail, 尾翼前缘分布, 尾翼后缘分布]
tags: [CFD, 网格, 各向异性, 尾翼, 平尾, 立尾, horizontal tail, vertical tail]
category: Aircraft
version: 1.0.0
author: GridStarAgent
allowed-tools: []
---

# 尾翼各向异性网格处理工作流

本 Skill 封装了基于 **TailAnisotropicProcessor** 的尾翼（平尾 + 立尾）各向异性网格处理一体化流程。调用 `TailAnisoProcessTailAnisotropy()` 一次性完成全部处理。

## 前置条件

必须在以下步骤完成后才能进入本 Skill：

1. 数模已导入（`ImportCADFile`）
2. 自动部件分割已完成（`ProcessWithServer` 或等效流程）
3. 尾翼子部件分割已完成，以下分组已存在：
   - `verticalTailRight`（立尾右表面）
   - `verticalTailLeft`（立尾左表面）
   - `verticalTailTip`（立尾尾梢）
   - `verticalTailTrailingEdge`（立尾后缘）
   - `horizontalTailUpper`（平尾上表面）
   - `horizontalTailLower`（平尾下表面）
   - `horizontalTailTip`（平尾尾梢）
   - `horizontalTailTrailingEdge`（平尾后缘）
4. 表面网格已生成（通过 `UGSur` 或 `GenerateSurMeshBySpitAssemblyGroupProperty`）
5. 机翼各向异性处理已完成（`WingAnisoProcessWingAnisotropy`）
6. 吊舱各向异性处理已完成（`NacelleAnisoProcessNacelleAnisotropy`，若存在）

## 一体化入口

直接调用 **`TailAnisoProcessTailAnisotropy()`** 即可。该工具无参数，内部依次执行：

| 内部步骤 | 功能 | 说明 |
|---|---|---|
| 1 | 获取尺寸计算 MAC | 读取机身长度、半展长、翼根/翼尖弦长 |
| 2 | 获取 10 类尾翼网格线 | 立尾前缘/后缘长/后缘短/机身交/尾梢交 + 平尾对应 5 类 |
| 3 | 分布设置 | 前缘和后缘长线: 0.1%×半展长；其他线: 0.1%×MAC；中间值 2%×MAC |
| 4 | 后缘短边设点数 | 立尾/平尾后缘短边设点数为 5 |
| 5 | 创建各向异性线组 | 8 组线(type=1) |
| 6 | 生成各向异性域 | 对尾翼网格面(type=0)，增长率 1.2 |
| 7 | 狭长面处理 | 对 horizontalTailTrailingEdge + verticalTailTrailingEdge |

### 返回格式示例

```json
{
  "status": "success",
  "steps": {
    "step1_dimensions": {"status":"success","mac":3.59},
    "step2_classify": {"status":"success","vt_leading_edge_count":1,...},
    "step3_distribution": {"status":"success","group_count":8},
    "step4_short_edge_points": {"status":"success"},
    "step5_aniso_line_groups": {"status":"success","group_count":9},
    "step6_aniso_domains": {"status":"success","domain_count":6},
    "step7_slender_face": {"status":"success","domain_count":2}
  }
}
```

### 失败处理

- `status` 为 `"failed"` 时停止，检查 `message` 字段获取具体原因
- 常见失败原因：尾翼子部件分割未完成、表面网格未生成、分组缺失
- 若某一步返回 `"failed"`，后续步骤不再执行

## 完成标准

- `TailAnisoProcessTailAnisotropy` 返回 `status` 为 `"success"`
- 尾翼网格面已生成各向异性网格
- 后缘面已做狭长面处理
