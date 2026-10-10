---
name: wing-anisotropy-processing
description: 引导用户通过 MCP 工具完成机翼各向异性网格处理与后缘面自动化处理的一体化流程。当用户提到机翼各向异性、前缘加密、展向加密、弦向加密、MAC计算、翼段检测、交线识别、各向异性域生成、后缘面处理、后缘网格、trailing edge、后缘分布、翼梢交线时必须使用。替代原先的 anisotropic-mesh-processing / chordwise-direction / spanwise-direction / nacelle-streamwise / wing-body-junction 五个子 Skill，并整合 trailing-edge-processing 的后缘面处理能力。
aliases: [机翼各向异性, 各向异性处理, wing anisotropy, 前缘分布, 弦向分布, 展向分布, MAC, 翼段, 交线识别, 各向异性域, 后缘面处理, 后缘网格, trailing edge, 后缘分布, 翼梢交线]
tags: [CFD, 网格, 各向异性, 机翼, 前缘, MAC, 后缘面]
category: Aircraft
version: 2.1.0
author: GridStarAgent
allowed-tools: []
---

# 机翼各向异性与后缘面一体化处理工作流

本 Skill 封装了基于 **WingAnisotropicProcessor** 的机翼各向异性网格处理与后缘面自动化装配的一体化流程。调用 `WingAnisoProcessWingAnisotropy()` 一次性完成全部处理，无需分别执行各向异性和后缘面两个步骤。

## 前置条件

必须在以下步骤完成后才能进入本 Skill：

1. 数模已导入（`ImportCADFile`）
2. 自动部件分割已完成，以下分组已存在：
   - `wingUpper`（机翼上表面）
   - `wingLower`（机翼下表面）
   - `wingTip`（翼梢）
   - `wingTrailingEdge`（后缘面）
   - `fuselage`（机身）
   - `engine` / `engine_*`（引擎/吊舱，若存在）
3. 表面网格已生成（通过 `UGSur` 或 `GenerateSurMeshBySpitAssemblyGroupProperty`）
4. 所属流程中只要上述机翼子组存在就必须执行本 Skill；分组缺失时才可跳过，并在阶段 `note` 中说明原因

## 一体化入口

直接调用 **`WingAnisoProcessWingAnisotropy()`** 即可。该工具无参数，内部依次执行：

| 内部步骤 | 功能 | 说明 |
|---|---|---|
| 1 | 获取尺寸计算 MAC | 读取机身长度、半展长、翼根/翼尖弦长 |
| 2 | 检测翼段 | 判断机翼由几段翼构成 |
| 2b | 后缘面类型分类 | 自动分类 wingTrailingEdge 面为三种拓扑类型：type 1（quadType，翼梢相邻→Merge4Edges）、type 2（hexType，有吊舱→6边角色识别）、type 3（quadType，无吊舱→Merge4Edges，参考端=fuselage） |
| 3 | 识别五类交线 | 翼身/翼梢/翼段/前缘/引擎，排除后缘线 |
| 4 | 第一组平滑分布 | 翼身/翼梢/翼段/引擎交线设平滑分布 |
| 4b | 后缘面分布设置 | **两趟式处理**：第一趟处理所有 quadType（type 1/3）——Merge4Edges→短边设点→方向判定→长边平滑；第二趟处理 hexType（type 2）——A/B/C设点→D/E平滑（E左端匹配相邻quadType下表面长边间距）→F拷贝 |
| 5 | 前缘线平滑分布 | 前缘线机身端/翼梢端设展向平滑 |
| 6 | 创建各向异性线组 | 五组线(type=1)，首层高度 0.1%×MAC 或 0.1%×半展长 |
| 7 | 生成各向异性域 | 对机翼网格面(type=0)，增长率 1.2 |
| 8 | 再次创建各向异性线组 | 保存面板数据 |
| 8b | 删除原后缘面+装配 | quadType（type 1/3）：4 线装配；hexType（type 2）：6 线按环顺序装配 |

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
  "steps": {
    "step1_dimensions": {...},
    "step2_segments": {...},
    "step2b_te_classification": [{"domain_id":12,"type":1,"wing_tip_id":34}, {"domain_id":15,"type":2,...}, {"domain_id":18,"type":3,"sub_type":"no_engine"}, ...],
    "step3_intersections": {...},
    "step4_group1_distribution": {...},
    "step4b_te_distribution": "done",
    "step5_leading_edge_distribution": {...},
    "step6_aniso_line_groups": {...},
    "step7_aniso_domains": {...},
    "step8_aniso_line_groups_save": {...},
    "step8b_te_delete_assemble": "done"
  }
}
```

### 失败处理

- `status` 为 `"failed"` 时停止，检查 `message` 字段获取具体原因
- 常见失败原因：部件分割未完成、表面网格未生成、分组缺失
- 若一体化入口内部的后缘面处理步骤（step8b_te_delete_assemble）失败，**可回退到下方**的独立后缘面 MCP 工具逐个处理

## 后缘面处理失败回退流程

> ⚠️ **仅在 `WingAnisoProcessWingAnisotropy` 的 `steps` 中 `step8b_te_delete_assemble` 失败**，或返回中 `te_classification_count` > 0 但某些后缘面条目 `status=failed` 时使用。

### 第0步：参数准备

1. 调用 `GetModelParameters` 获取 `wing_half_span`（半展长）和 `mac`（当地弦长）
2. 按公式计算间距参数：
   - `bodySpacing` = 0.1% × 半展长 = 0.001 × wing_half_span
   - `rootSpacing` = 2% × 当地弦长 = 0.02 × mac
   - `params` = `"1.2,10,1.2,10"`
3. 调用 `GetAllSpitAssemblyGroupProperty`，确认 `wingTrailingEdge`、`wingTip`、`enginePylonTrailingEdge`、`fuselage` 分组存在

### 第1步：全自动批量处理

1. 调用 **`ProcessAllTrailingEdges`**（半展长, 当地弦长, bodySpacing, rootSpacing, params）
2. 检查返回的 `results` 数组：
   - 所有条目 `status=success` → **处理完成**
   - 有 `status=failed` 或 `status=skipped` → **逐面回退到下方第2步的分步流程**

### 第2步（回退）：类型判定

调用 `ClassifyTrailingEdgeDomains()`，得到每个后缘面的 `type`（1/2/3）和关联分组信息。

- `type=1`：翼梢相邻的 quadType（参考端 = wingTip）
- `type=2`：有吊舱的 hexType（6 边，下表面被 p1/p2 分成三段）
- `type=3`：无吊舱的 quadType（参考端 = fuselage）

### 第3步（回退）：处理 quadType（type 1 / type 3）

quadType（翼梢相邻或无吊舱）有 4 条网格线（2 长边 + 2 短边），使用 `MergeEdgesByDomain`。

> 读取 `references/type1.md` 获取完整步骤（type 1 和 type 3 共享相同的 quad 处理流程，仅在参考端识别上有差异）。

先逐个处理所有 quadType 的后缘面，全部完成后，再处理 hexType。

### 第4步（回退）：处理 hexType（type 2）

hexType（有吊舱）有 6 条网格线，**严禁调用 `MergeEdgesByDomain`**。

> 读取 `references/type2.md` 获取完整步骤。
>
> **关键提醒**：先调用 `read_skill_resource("wing-anisotropy-processing", "references/type2.md")` 加载类型二的参考文件。
> 角色判定使用 `IdentifyType2Roles` 获取角色线 ID 和端点信息，失败时回退到分组网格线求交集 + 端点 ID 对比（见 type2.md 步骤 1 方式二）。
> 调用 `ProcessTrailingEdgeType2` 前必须先调用 `IdentifyType2Roles` 获取参数。

## 完成标准

- `WingAnisoProcessWingAnisotropy` 返回 `status` 为 `"success"`
- MAC 值有效（非零）
- 后缘面处理回退时：所有工具调用返回 `success` 为 `true`，任一步骤失败时停止
- 回退后 quadType（type 1/3）：4 条网格线已装配为结构面；hexType（type 2）：6 条网格线已装配为结构面
