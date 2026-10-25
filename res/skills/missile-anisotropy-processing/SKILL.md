---
name: missile-anisotropy-processing
description: 引导用户通过 MCP 工具完成导弹弹翼各向异性网格处理的一体化流程。当用户提到导弹各向异性、弹翼前缘加密、弹翼后缘加密、翼身结合、翼梢加密、弹翼展向加密、弹翼弦向加密时必须使用。替代原先的 missile-chordwise-direction / missile-spanwise-direction / missile-root-junction 三个子 Skill。
aliases: [导弹各向异性, 弹翼各向异性, missile anisotropy, 弹翼前缘加密, 弹翼后缘加密, 翼身结合, 翼梢加密, 弹翼展向, 弹翼弦向]
tags: [CFD, 网格, 各向异性, 导弹, 弹翼, 前缘, 后缘, 厚度]
category: CFD
version: 2.0.0
author: GridStarAgent
allowed-tools: []
---

# 导弹弹翼各向异性网格处理工作流

本 Skill 封装了基于 **MissileWingAnisotropicProcessor** 的导弹弹翼各向异性网格处理一体化流程。调用 `MissileWingAnisoProcessWingAnisotropy()` 一次性完成全部处理，无需分别执行弦向/展向/结合处三个子步骤。

> 🔴 **2026-10-22 工具替换**：原一站式 `MissileAnisoProcessMissileAnisotropy`（基于 `MissileAnisotropicProcessor`）已被 `MissileWingAnisoProcessWingAnisotropy`（基于 `MissileWingAnisotropicProcessor`）**取代**，旧工具已从后端与 MCP 注册中移除，**不得再调用**。
> ⚠️ **新工具不含弹尾加密**（旧工具的 step6 已随之弃用）。弹尾各向异性由后续独立工具处理——**本 Skill 处理完弹翼即结束**，不要在此期待或补做弹尾加密。

## 前置条件

必须在以下步骤完成后才能进入本 Skill：

1. 数模已导入（`ImportCADFile`）
2. 导弹 AI 分部件已完成（`ClassifyMissile` 或等效手分组），以下分组已存在：
   - `fuselage`（弹体）
   - `nose`（弹头）
   - `tail`（尾部）
   - `wingSideSurfaceN`（弹翼主体大面）
   - `wingLeadingEdgeN`（弹翼前缘面）
   - `wingTrailingEdgeN`（弹翼后缘面）
   - `wingTipN`（翼梢面，**仅梯形翼存在——按 M2 运行时探测**）
3. 🔴 **`GetMissileDimensions` 已调用**——尺寸写回后端 `SPIDER::Data`。未调用 → 弹翼按对数组为空 → 本工具第 1 步守卫 `failed`
4. 表面网格已生成（`MissileSurfMeshGenerate`）

> 🔴 **推荐顺序**：`ClassifyMissile` → `GetMissileDimensions` → `MissileSurfMeshGenerate` → `MissileWingAnisoProcessWingAnisotropy`

> ⚠️ **N = 第几对弹翼（1 / 2 / 3）**：后端**只产出这套带序号子组名，不存在 `wing` 主组** —— 子组名即定位键，**无需 `UGSpitAssemblyCreateNewGroup` 归并**，也无"子面区分丢失"问题。
> ⚠️ 弹翼对数从现有分组自动检测（检测 `wingSideSurface1` ~ `wingSideSurface3`），最多支持 3 对弹翼。

## 一体化入口

直接调用 **`MissileWingAnisoProcessWingAnisotropy()`** 即可。该工具无参数，内部依次执行：

| 内部步骤 | 功能 | 说明 |
|---|---|---|
| 1 | 尺寸与对数 | 按对取该对弹翼的**根弦长**与**厚度**（读 `SPIDER::Data` 的按对数组） |
| 2 | 短边 / 长边分类 | 每对 4 类短边（含 4 类交点）+ 7 类长边 |
| 3 | 分布设置 | 短边：首尾端间距＝`0.1×厚度`，双曲正切；长边：首尾端间距＝`0.1×厚度`，增长分布，中间值＝`0.02×根弦长` |
| 4 | 创建各向异性线组 | type=1，每对**两组**（组A 窄面 `wingNarrowN` / 组B 宽面 `wingBroadN`），首层高度＝`0.1×厚度`，控制层数 50 |
| 5 | 生成各向异性域 | type=0，generateType=2（四边形各向异性），**含翼后缘面**；组A 增长率 1、组B 增长率 1.2 |

> 🔴 **尺寸基准是「翼厚度」，不再是「加密弦长」**：首尾端间距与各向异性首层高度一律 = `0.1 × 该对弹翼厚度`（`wings[N−1].thickness`，翼后缘处厚度）；长边分布中间值 = `0.02 × 该对翼根弦长`。**旧文档的 `0.005×当地弦长` 加密尺寸口径已作废。**

> 🔴 **翼后缘面在步骤 5 一并处理，无独立步骤、无翼型分支**：后缘面（`wingTrailingEdgeN`）已在步骤 5 的组A 面集内，增长率按**窄面取 1**。**三角翼与梯形翼同款 —— 只要是翼后缘就做各向异性**。

🔴 **参数与逐对处理基准见 `missile-cfd-meshing-workflow` 的 `references/missile-anisotropic-mesh.md` §「各位置参数」**（本表与之一致）：组A（窄面）增长率 1、组B（侧面/结合处）增长率 1.2；**N=1/2/3 逐对按各自的厚度与根弦长处理**。

### 🔴 返回后不做核验（只看 `status`）

一站式返回 `status="success"` **即为本阶段结束**，**直接进入下一阶段**。🔴 **不得做任何返回后核验**——不读 `step1_dimensions` / `step2_classify` / `step3_distribution` / `step4_aniso_line_groups` / `step5_aniso_domains`，不查尺寸是否为正值、不比对各对 `root_chord` / `thickness`、不比对增长率。这些检查在此处既不产生决策、又纯耗轮次，已全部删除。

> 🔴 **只有 `status="failed"` 触发分步回退**（见下「分步回退」）；接口恒定值/空（不可用）→ `skipped` + `note`，不回退。

### 返回格式示例

```json
{
  "status": "success",
  "steps": {
    "step1_dimensions": {"status":"success","pair_count":2,
                         "pairs":[{"pair":1,"root_chord":<值>,"thickness":<值>},
                                  {"pair":2,"root_chord":<值>,"thickness":<值>}]},
    "step2_classify":   {"status":"success","pair_count":2,
                         "pairs":[{"pair":1,"short_edge_count":<n>,"long_edge_count":<n>}, ...]},
    "step3_distribution":{"status":"success","pair_count":2,
                          "pairs":[{"pair":1,"end_space":<值>,"middle_value":<值>,
                                    "short_success":true,"long_success":true}, ...]},
    "step4_aniso_line_groups":{"status":"success","group_count":4,"total_pairs":<n>,
                               "groups":[{"name":"wingNarrow1","first_high":<值>,
                                          "growth_rate":1,"control_layer":50,...}, ...]},
    "step5_aniso_domains":{"status":"success","domains_a_count":<n>,"domains_b_count":<n>,
                           "growth_a":1,"growth_b":1.2}
  }
}
```

> 🔴 上例**只示意字段结构**（`<值>` 为占位符，示例按 2 对翼给出 `pairs[]` 两项）——🔴 **`steps` 字段仅用于 `status="failed"` 时定位失败点，不作返回后核验判据**。
> 🔴 返回里**没有 `step6_tail`**（新工具不做弹尾），这是正常现象，不是失败。

### 失败处理

- `status` 为 `"failed"` 时 → **自动回退到分步流程**（见下「分步回退」），**不停止、不询问用户**。检查 `message` 字段确认卡在哪一步，回退时优先复用它已算出的上下文。
- 常见失败原因：弹翼**带序号子面分组**缺失（未检测到 `wingSideSurfaceN`）、表面网格未生成、**`GetMissileDimensions` 未调用导致按对尺寸数组为空**
- 若某一步返回 `"failed"`，后续步骤不再执行，检查前置条件是否满足

## 分步回退（`status="failed"` 时自动进入）

一站式 `MissileWingAnisoProcessWingAnisotropy()` 是**首选**。仅当它返回 `status="failed"`（接口可调通、内部某步失败）时，**自动**按主 Skill `missile-cfd-meshing-workflow` 的 `references/missile-anisotropic-mesh.md` §「分步回退」的映射表分步重做，**无需用户确认**（用 `read_skill_resource` 读取，skill 参数填 `missile-cfd-meshing-workflow`）。

**按位置加载对应回退子 Skill**，各自完成其「网格线分类 + 分布设置」两步：

| 失败位置 | 加载子 Skill |
|---|---|
| 翼窄面（前缘/后缘/翼梢面）弦向 | `missile-chordwise-direction` |
| 翼前缘线（前缘面 ∩ 侧面公共边）展向 | `missile-spanwise-direction` |
| 翼身结合处（绕翼一圈） | `missile-root-junction` |
| 翼侧面（`wingSideSurfaceN`） | 无独立子 Skill —— 按 `missile-anisotropic-mesh.md` §「各位置参数」**翼侧面行**取值（首层＝`0.1×厚度`、增长率 1.2、层数 50、中间值＝翼面尺寸），直接由下述 `GenerateANisoDomainGrid` 下发 |

> 🔴 **逐对处理**：上述各位置对 N=1/2/3 **各跑一遍**，每对用**该对的厚度与根弦长**算首层高度/端间距/中间值。
> 🔴 **回退不含弹尾加密** —— 弹尾由后续独立工具处理，回退流程到此为止。

**AI 自动补回退中缺失的两步**（子 Skill 不覆盖）：

- **创建各向异性线组 + 各向异性域生成** → `GenerateANisoDomainGrid(anisoGroupsJson)` 一次下发全部组（构造见 missile-anisotropic-mesh.md §「分步回退」；`firstHigh` 一律取 `0.1×该对厚度`，`growthRate` 组A 1 / 组B 1.2）。🔴 **翼后缘面一并纳入**（窄面，增长率 1），**不调用 `GenerateLongAndNarrowFaceGrid`**
- **尺寸**：🔴 **先调 `GetMissileDimensions`**（MCP，需 `enable_tool_group("missile")`）取弹体 D / D_tail / D_surface / R，🔴 **弹翼按对取 `wings[N−1]`**（`root_chord` / `thickness`，下标 = 对数−1，**不得各对共用**），🔴 **接口返回毫米 mm，直接传参，禁止 ×1000**

**终点**：分步仍失败 → 该阶段 `skipped` + `note`（记录卡在哪一步），**不重试、不换参重试**；接口返回恒定值/空（不可用）→ **不回退**，直接 `skipped` + `note`。

## 完成标准

- `MissileWingAnisoProcessWingAnisotropy` 返回 `status` 为 `"success"`（🔴 **只看 `status`，不做任何返回后核验**）
- 步骤 4–5 均返回 `success`
- 弹翼各向异性网格已生成
- **翼后缘面已完成各向异性**（步骤 5 内，`generateType=2`、增长率 1）—— 三角翼/梯形翼一视同仁，无须任何额外后缘处理
- ⚠️ **弹尾（`tail`）各向异性不属本 Skill 的完成标准**——由后续独立工具处理
