---
name: wing-anisotropy-processing
description: 引导用户通过 MCP 工具完成机翼各向异性网格处理。当用户提到机翼各向异性、前缘加密、展向加密、弦向加密、MAC计算、翼段检测、交线识别、各向异性域生成时必须使用。替代原先的 anisotropic-mesh-processing / chordwise-direction / spanwise-direction / nacelle-streamwise / wing-body-junction 五个子 Skill。
aliases: [机翼各向异性, 各向异性处理, wing anisotropy, 前缘分布, 弦向分布, 展向分布, MAC, 翼段, 交线识别, 各向异性域]
tags: [CFD, 网格, 各向异性, 机翼, 前缘, MAC]
category: CFD
version: 1.0.0
author: GridStarAgent
allowed-tools: []
---

# 机翼各向异性网格处理工作流

本 Skill 封装了基于 **WingAnisotropicProcessor** 的机翼各向异性网格处理。处理对象为飞行器机翼区域的表面网格，提供从尺寸计算到各向异性域生成的端到端一站式操作。

## 前置条件

必须在以下步骤完成后才能进入本 Skill：

1. 数模已导入（`ImportCADFile`）
2. 自动部件分割已完成，以下分组已存在：
   - `wingUpperSurface`（机翼上表面）
   - `wingLowerSurface`（机翼下表面）
   - `wingTip`（翼梢）
   - `wingTrailingEdge`（后缘面）
   - `fuselage`（机身）
   - `engine` / `engine_*`（引擎/吊舱，若存在）
3. 表面网格已生成（通过 `UGSur` 或 `GenerateSurMeshBySpitAssemblyGroupProperty`）
4. 所属流程中只要上述机翼子组存在就必须执行本 Skill；分组缺失时才可跳过，并在阶段 `note` 中说明原因

## 处理流程（由工具内部自动完成）

调用 `WingAnisoProcessWingAnisotropy()` 后，内部依次执行：

1. **获取机翼尺寸并计算 MAC**：读取机身长度、半展长、翼根/翼尖弦长，计算平均气动弦长
2. **检测翼段**：判断机翼由几段翼构成
3. **识别交线**：自动获取五类相交网格线（翼身/翼梢/翼段/前缘/引擎），排除后缘线
4. **第一组平滑分布**：翼身/翼梢/翼段/引擎交线统一设置平滑分布（0.1%×MAC，增长率1.2）
5. **前缘线平滑分布**：前缘线机身端和翼梢端设展向平滑分布（0.1%×半展长，增长率1.2）
6. **创建各向异性线组**：五组线创建各向异性线组（首层高度0.1%×MAC或0.1%×半展长，增长率1.2，层数50）
7. **生成各向异性域**：对机翼网格面执行各向异性网格生成

## 执行步骤

### 调用入口

直接调用 `WingAnisoProcessWingAnisotropy()` 即可。该工具无参数，返回汇总 JSON 包含全部子步骤的状态。

### 返回格式示例

```json
{
  "status": "success",
  "mac": 3.59,
  "wing_segments": 1,
  "group1_connector_ids": [12, 34, 56],
  "leading_edge_ids": [101, 102, 103],
  "trailing_edge_excluded_ids": [201, 202],
  "group1_distribution": "success",
  "leading_edge_distribution": "success",
  "message": "..."
}
```

### 失败处理

- `status` 为 `"failed"` 时停止，检查 `message` 字段获取具体原因
- 常见失败原因：部件分割未完成、表面网格未生成、分组缺失
- 新返回 `"false"` 时，检查 WingAnisotropicProcessor 前置条件是否满足

## 完成标准

- `WingAnisoProcessWingAnisotropy` 返回 `status` 为 `"success"`
- MAC 值有效（非零）
- 任一步骤失败时停止
