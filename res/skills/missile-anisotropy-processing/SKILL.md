---
name: missile-anisotropy-processing
description: 引导用户通过 MCP 工具完成导弹弹翼各向异性网格处理的一体化流程。当用户提到导弹各向异性、弹翼前缘加密、弹翼后缘加密、翼身结合、翼梢加密、弹翼展向加密、弹翼弦向加密时必须使用。替代原先的 missile-chordwise-direction / missile-spanwise-direction / missile-root-junction 三个子 Skill。
aliases: [导弹各向异性, 弹翼各向异性, missile anisotropy, 弹翼前缘加密, 弹翼后缘加密, 翼身结合, 翼梢加密, 弹翼展向, 弹翼弦向]
tags: [CFD, 网格, 各向异性, 导弹, 弹翼, 前缘, 后缘, MAC]
category: CFD
version: 1.1.0
author: GridStarAgent
allowed-tools: []
---

# 导弹弹翼各向异性网格处理工作流

本 Skill 封装了基于 **MissileAnisotropicProcessor** 的导弹弹翼各向异性网格处理一体化流程。调用 `MissileAnisoProcessMissileAnisotropy()` 一次性完成全部处理，无需分别执行弦向/展向/结合处三个子步骤。

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
3. 表面网格已生成（通过 `UGSur` 或 `GenerateSurMeshBySpitAssemblyGroupProperty`）

> ⚠️ **N = 第几对弹翼（1 / 2 / 3）**：后端**只产出这套带序号子组名，不存在 `wing` 主组** —— 子组名即定位键，**无需 `UGSpitAssemblyCreateNewGroup` 归并**，也无"子面区分丢失"问题。
> ⚠️ 弹翼对数从现有分组自动检测（检测 `wingSideSurface1` ~ `wingSideSurface3`），最多支持 3 对弹翼。

## 一体化入口

直接调用 **`MissileAnisoProcessMissileAnisotropy()`** 即可。该工具无参数，内部依次执行：

| 内部步骤 | 功能 | 说明 |
|---|---|---|
| 1 | 获取弹体尺寸 | 读取弹体长度、弹径，从 fuselage 面坐标推算 |
| 2 | 弹翼分类与 MAC 计算 | 自动检测弹翼对数，计算每对弦长/半展长/MAC |
| 2b | 分类五类网格线 | 前缘线、后缘长线、后缘短线、翼身交线、翼梢交线 |
| 3 | 分布设置 | 平滑分布：首层＝**加密尺寸 0.005×当地弦长**（窄面/侧面/结合处/弹尾一律），中间值＝**翼面尺寸 0.02×当地弦长** |
| 4 | 创建各向异性线组 | type=1，首层高度＝加密尺寸，增长率**窄面 1 / 侧面·结合处·弹尾 1.2**，控制层数 50 |
| 5 | 生成各向异性域 | type=0，generateType=2（四边形各向异性），**含翼后缘面**，增长率同步骤 4 |
| 6 | 弹尾加密 | 对 `tail` 分组做各向异性（type=0, generateType=2），取**弦长最大的那一对翼**的参数 |

> 🔴 **翼后缘面在步骤 5 一并处理，无独立步骤、无翼型分支**：后缘面（`wingTrailingEdgeN`）已在步骤 5 的各向异性面集内（`CollectPairAnisoDomains` 含 `wp.trailingEdge`），增长率按**窄面取 1**。**三角翼与梯形翼同款 —— 只要是翼后缘就做各向异性**。

🔴 **参数与逐对处理基准见 `missile-cfd-meshing-workflow` 的 `references/missile-anisotropic-mesh.md` §「各位置参数」**（本表与之一致）：窄面增长率 1、侧面/结合处/弹尾 1.2；弹尾加密与翼一致，**多对翼取弦长最大的那一对**；**N=1/2/3 逐对按各自弦长处理**。

### 🔴 返回后必做两查（`status="success"` 亦然）

🟡 本工具**无参数**——`step1/step2` 的**弹体**尺寸由**后端读取 `GetMissileDimensions` 写回的 `SPIDER::Data`**，**弹翼**尺寸由后端**按对直读尺寸缓存 `wings` 数组**（2026-10-16 起改按对；2026-10-17 起该缓存由分割实算写入、不再写死）。**`success` 只证明流程跑通，不证明尺寸可用**。

| 查 | 比对 | 判据 | 不合规时 |
|---|---|---|---|
| **1. 尺寸可用性** | `step1_dimensions.body_diameter`、`pairs[].root_chord`/`local_chord`、`half_span` 是否为正（🔴 **每对都要查**）；`body_diameter_surface` 是否自洽 | 🔴 **全部 > 0**，且各对 `root_chord` 应各自等于该对 `wings[N−1].root_chord`（不得各对相同）；`\|D−D_tail\| > 0.7×D_tail` 时 `body_diameter_surface` = 尾口径，否则 = D | 🔴 **暂停本阶段**，列实测值问用户"`ClassifyMissile` 是否已跑过、`GetMissileDimensions` 是否已调用（尺寸缓存是否已写入）"；`note` 标记 `dimension_unavailable` + 报告后端，**不得描述为正常完成** |
| **2. 窄面增长率一致性** | `step3_distribution` 与 `step4_aniso_line_groups` 的窄面（前缘/后缘/翼梢）增长率 | 两处**都 = 1** | 🔴 任一处 ≠1 → `note` 标记 `growth_rate_mismatch` + 记实际值 + 报告后端 |

> 🔴 **弦长是主控项**：`firstHigh = 0.005×当地弦长`、`middle = 0.02×当地弦长` —— 弦长错多少，加密尺寸与中间值整体错多少。**两查均不触发分步回退**（回退只由 `status="failed"` 触发）。
> 🔴 **2026-10-14 变更**：旧"与固定值比对、偏差 ≤5%"的查 1 已**作废**（固定值取消，后端改为接口值）——现只查**正值与自洽**。
> 🔴 **2026-10-16 变更**：弹翼尺寸改为**按对直读尺寸缓存 `wings` 数组**（`GetWingPairDims`，不经 `SPIDER::Data` 中转），弹体尺寸仍读 `SPIDER::Data`。因此查 1 新增一条判据：**各对 `root_chord` 应互不相同**——**各对相同即说明回退到了第 1 对**。详见 `missile-calibration-constants.md` §2.1。
> 🔴 **2026-10-17 更新**：尺寸**恢复为几何实算**——缓存由 `ClassifyMissile` 第 10 步 `CalculateDimensions()` 写入，`wings` 由分割出的翼对推出。🔴 **必须先跑分割**，否则缓存为空、本工具因弹体尺寸守卫 `failed`。
> ✅ 查 2 的历史偏离已于 2026-10-13 修复（组名统一为 `trailingEdge`）。保留作为**回归护栏**：再见到 `step4` 窄面增长率 1.2，即为后端回归。

### 返回格式示例

```json
{
  "status": "success",
  "steps": {
    "step1_dimensions": {"status":"success","fuselage_length":<值>,"body_diameter":<值>,
                         "body_diameter_tail":<值>,"body_diameter_surface":<值>,"unit":"mm"},
    "step2_classify": {"status":"success","pair_count":2,"pairs":[{"pair_index":1,"mac":<值>,...},
                                                                 {"pair_index":2,"mac":<值>,...}]},
    "step3_distribution": {"status":"success","pairs":[{"pair_index":1,"group_count":4,...},
                                                       {"pair_index":2,"group_count":4,...}]},
    "step4_aniso_line_groups": {"status":"success","pairs":[{"pair_index":1,"group_count":5,...},
                                                            {"pair_index":2,"group_count":5,...}]},
    "step5_aniso_domains": {"status":"success","pairs":[{"pair_index":1,"domain_count":6},
                                                        {"pair_index":2,"domain_count":6}]},
    "step6_tail": {"status":"success","domain_count":1,"ref_pair_index":1,"first_high":<值>,
                   "growth_rate":1.2,"layer":50,"ring_connector_count":12,"line_group_ok":true}
  }
}
```

> 🔴 上例**只示意字段结构**（`<值>` 为占位符，示例按 2 对翼给出 `pairs[]` 两项）——**不是基准值**，判据只看"是否 > 0 且自洽、各对是否各自等于该对 `wings[N−1].root_chord`"。

### 失败处理

- `status` 为 `"failed"` 时 → **自动回退到分步流程**（见下「分步回退」），**不停止、不询问用户**。检查 `message` 字段确认卡在哪一步，回退时优先复用它已算出的上下文。
- 常见失败原因：弹翼**带序号子面分组**缺失（未检测到 `wingSideSurfaceN`）、表面网格未生成、MAC 计算失败
- 若某一步返回 `"failed"`，后续步骤不再执行，检查前置条件是否满足

## 分步回退（`status="failed"` 时自动进入）

一站式 `MissileAnisoProcessMissileAnisotropy()` 是**首选**。仅当它返回 `status="failed"`（接口可调通、内部某步失败）时，**自动**按主 Skill `missile-cfd-meshing-workflow` 的 `references/missile-anisotropic-mesh.md` §「分步回退」的映射表分步重做，**无需用户确认**（用 `read_skill_resource` 读取，skill 参数填 `missile-cfd-meshing-workflow`）。

**按位置加载对应回退子 Skill**，各自完成其「网格线分类 + 分布设置」两步：

| 失败位置 | 加载子 Skill |
|---|---|
| 翼窄面（前缘/后缘/翼梢面）弦向 | `missile-chordwise-direction` |
| 翼前缘线（前缘面 ∩ 侧面公共边）展向 | `missile-spanwise-direction` |
| 翼身结合处（绕翼一圈） | `missile-root-junction` |
| 翼侧面（`wingSideSurfaceN`） | 无独立子 Skill —— 按 `missile-anisotropic-mesh.md` §「各位置参数」**翼侧面行**取值（首层＝加密尺寸、增长率 1.2、层数 50、中间值＝翼面尺寸），直接由下述 `GenerateANisoDomainGrid` 下发 |

> 🔴 **逐对处理**：上述各位置对 N=1/2/3 **各跑一遍**，每对用该对弦长算加密尺寸/翼面尺寸；**弹尾加密与翼一致，取弦长最大的那一对翼的参数**（见 `missile-anisotropic-mesh.md` §「弹尾加密」）。

**AI 自动补回退中缺失的两步**（子 Skill 不覆盖）：

- **创建各向异性线组 + 各向异性域生成** → `GenerateANisoDomainGrid(anisoGroupsJson)` 一次下发全部组（构造见 missile-anisotropic-mesh.md §「分步回退」；`firstHigh` 一律取加密尺寸，`growthRate` 窄面 1 / 侧面·结合处·弹尾 1.2）。🔴 **翼后缘面一并纳入**（窄面，增长率 1），**不调用 `GenerateLongAndNarrowFaceGrid`**
- **尺寸**：🔴 **先调 `GetMissileDimensions`**（MCP，需 `enable_tool_group("missile")`）取弹体 D / D_tail / D_surface / R，🔴 **弹翼按对取 `wings[N−1]`**（`root_chord` / `tip_chord` / `half_span`，下标 = 对数−1，**不得各对共用**），🔴 **接口返回毫米 mm，直接传参，禁止 ×1000**；C_tip(N) 按 M2 探测结果分叉（三角翼钳制到**该对** C_root 计算值，梯形翼按接口值）

**终点**：分步仍失败 → 该阶段 `skipped` + `note`（记录卡在哪一步），**不重试、不换参重试**；接口返回恒定值/空（不可用）→ **不回退**，直接 `skipped` + `note`。

## 完成标准

- `MissileAnisoProcessMissileAnisotropy` 返回 `status` 为 `"success"`
- 🔴 **已完成上方「返回后必做两查」**：尺寸全 > 0 且各对 `root_chord` 各自等于该对 `wings[N−1].root_chord`（或已 `dimension_unavailable` 上报）、窄面增长率两处均 1（或已 `growth_rate_mismatch` 上报）
- MAC 值有效（非零）
- 步骤 4–6 均返回 `success`
- 弹翼各向异性网格已生成
- **翼后缘面已完成各向异性**（步骤 5 内，`generateType=2`、增长率 1）—— 三角翼/梯形翼一视同仁，无须任何额外后缘处理
- 弹尾（`tail`）已完成各向异性加密，参数取弦长最大的那一对翼
