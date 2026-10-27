---
name: missile-anisotropy-processing
description: 引导用户通过 MCP 工具完成导弹各向异性网格处理的一体化流程 —— 弹翼（前缘/后缘/翼梢/侧面/结合处）与弹身（弹尾∩弹身、弹头∩弹身两处结合处）。当用户提到导弹各向异性、弹翼前缘加密、弹翼后缘加密、翼身结合、翼梢加密、弹翼展向加密、弹翼弦向加密、弹身加密时必须使用。替代原先的 missile-chordwise-direction / missile-spanwise-direction / missile-root-junction 三个子 Skill。
aliases: [导弹各向异性, 弹翼各向异性, 弹身各向异性, missile anisotropy, 弹翼前缘加密, 弹翼后缘加密, 翼身结合, 翼梢加密, 弹翼展向, 弹翼弦向]
tags: [CFD, 网格, 各向异性, 导弹, 弹翼, 弹身, 前缘, 后缘, 厚度]
category: CFD
version: 3.0.0
author: GridStarAgent
allowed-tools: []
---

# 导弹各向异性网格处理工作流

🔴 **本阶段有两个一站式工具，都要调用，🔴 顺序固定：先弹身、后弹翼 —— 漏一个该部分就没有各向异性**：

| 顺序 | 对象 | 工具 | 处理分组 |
|---|---|---|---|
| **①** | 🔴 **弹身** | `MissileBodyAnisoProcessBodyAnisotropy()`（后端 `MissileBodyAnisotropicProcessor`） | `fuselage`（结合 `tail` / `nose`） |
| **②** | **弹翼** | `MissileWingAnisoProcessWingAnisotropy()`（后端 `MissileWingAnisotropicProcessor`） | `wing*N` 各组 |

> 🔴 **顺序不可颠倒**：一律**先弹身、后弹翼**。不得交换，也不得只调其中一个。
> 两者各自动态读取分组与分组几何。

> 🔴 **2026-10-22 工具替换**：原一站式 `MissileAnisoProcessMissileAnisotropy`（基于 `MissileAnisotropicProcessor`）已被 `MissileWingAnisoProcessWingAnisotropy`（基于 `MissileWingAnisotropicProcessor`）**取代**，旧工具已从后端与 MCP 注册中移除，**不得再调用**。
> 🔴 **2026-10-26 新增弹身工具**：`MissileBodyAnisoProcessBodyAnisotropy`（基于 `MissileBodyAnisotropicProcessor`）。
> ⚠️ **两个工具都不含弹尾独立加密**（旧工具的 step6 已随之弃用）。返回里**没有 `step6_tail`** 属正常现象，不是失败。弹尾各向异性由后续独立工具处理——**本 Skill 处理完弹翼与弹身即结束**。

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
3. 🔴 **`GetMissileDimensions` 已调用**——尺寸写回后端 `SPIDER::Data`。未调用 → 弹翼按对数组为空 / 弹尾口径为 0 → **两个工具均在第 1 步守卫 `failed`**
4. 表面网格已生成（`MissileSurfMeshGenerate`）

> 🔴 **推荐顺序（顺序固定，不可颠倒）**：`ClassifyMissile` → `GetMissileDimensions` → `MissileSurfMeshGenerate` → **`MissileBodyAnisoProcessBodyAnisotropy`（先弹身）** → **`MissileWingAnisoProcessWingAnisotropy`（后弹翼）**

> ⚠️ **N = 第几对弹翼（1 / 2 / 3）**：后端**只产出这套带序号子组名，不存在 `wing` 主组** —— 子组名即定位键，**无需 `UGSpitAssemblyCreateNewGroup` 归并**，也无"子面区分丢失"问题。
> ⚠️ 弹翼对数从现有分组自动检测（检测 `wingSideSurface1` ~ `wingSideSurface3`），最多支持 3 对弹翼。
> 🔴 **弹身工具不依赖弹翼子组**：其定位键是 `fuselage` / `tail` / `nose`（分割的基本部件组，恒存在），故**弹翼子组缺失时弹身工具照常调用**。

## 一体化入口

### (a) 🔴 第 1 步 · 弹身：`MissileBodyAnisoProcessBodyAnisotropy()`

该工具无参数，内部依次执行：

| 内部步骤 | 功能 | 说明 |
|---|---|---|
| 1 | 尺寸 | 读 `SPIDER::Data::m_bodyDiameterTail`（弹尾口径，mm）；🔴 为 0 → 本步 `failed` |
| 2 | 弹身线分类 | 五类 A~E（见下） |
| 3 | 分布设置 | A / B / E 三类平滑增长分布 |
| 4 | 创建各向异性线组 | type=1，**两组**：`bodyTailJunction` / `bodyNoseJunction`，增长率 1.2、层数 50 |
| 5 | 生成各向异性域 | type=0，generateType=2，对两组面分别下发，增长率 1.2 |

**弹身网格线分类（"属于弹身"即 `fuselage` 组）**：

| 类 | 定义 | 分布设置 | 归属 |
|---|---|---|---|
| **A** | 与【弹尾】和【弹翼后缘】同时共点的弹身线 | 首尾端间距 = 共点弹翼后缘线的端间距；增长 1.2/1.2、中间值 `0.05×尾口径` | 组1 |
| **B** | 与【弹翼前缘】和【弹翼后缘】同时共点的弹身线 | 前缘相邻端取前缘线端间距、后缘相邻端取后缘线端间距；同上中间值 | 组1 |
| **C** | 弹尾 ∩ 弹身 交线 | 不经步骤 3 | 组1 |
| **D** | 弹头 ∩ 弹身 交线 | 不经步骤 3；其**首尾端间距平均值**即组2 首层高度 | 组2 |
| **E** | 与【弹头】共点的弹身线 | 共点端 = D 的首尾端间距平均值、**另一端不变**；增长 1.2/1.2、中间值 `0.05×尾口径` | 组2 面集内 |

> 🔴 **A/B 与 E 的中间值比例相同，都是 `0.05 × 弹尾口径`**（2026-10-26 由旧的 `0.025×`/`0.04×` 统一改定，旧比例作废）。

**两个各向异性线组**：

| 组名 | 线 | 面 | 首层高度 |
|---|---|---|---|
| `bodyTailJunction` | A ∪ B ∪ C | 与弹尾相交的弹身面 + 弹尾面 | A 类线端间距 |
| `bodyNoseJunction` | D | 弹头面 + **其余未做各向异性处理的弹身面** | D 类线首尾端间距平均值 |

> 🔴 首层高度取不到正值 → **兜底退 `0.05×尾口径`**，不报错、不失败。
> ⚠️ 弹身工具做的是【弹尾∩弹身】与【弹头∩弹身】两处**结合处**各向异性，**不是**旧一站式 step6 的"弹尾加密"。

🔴 **弹身尺寸基准与完整说明见事实中心 §2.3**（`missile-cfd-meshing-workflow` 的 `references/missile-calibration-constants.md`）。

### (b) 🔴 第 2 步 · 弹翼：`MissileWingAnisoProcessWingAnisotropy()`

> 🔴 **必须在 (a) 弹身之后调用。**

该工具无参数，内部依次执行：

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

🔴 **两个工具都**返回 `status="success"` **即为本阶段结束**，**直接进入下一阶段**。🔴 **不得做任何返回后核验**——不读 `step1_dimensions` / `step2_classify` / `step3_distribution` / `step4_aniso_line_groups` / `step5_aniso_domains`，不查尺寸是否为正值、不比对各对 `root_chord` / `thickness`、不比对增长率与首层高度。这些检查在此处既不产生决策、又纯耗轮次，已全部删除。

> 🔴 **只有 `status="failed"` 触发分步回退**（见下「分步回退」）；接口恒定值/空（不可用）→ `skipped` + `note`，不回退。

### 返回格式示例（弹翼）

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

### 返回格式示例（弹身）

```json
{
  "status": "success",
  "steps": {
    "step1_dimensions": {"status":"success","body_diameter_tail":<值>,
                         "body_diameter":<值>,"middle_value":<值>},
    "step2_classify":   {"status":"success",
                         "body_tail_te_count":<n>,"body_le_te_count":<n>,
                         "tail_body_count":<n>,"nose_body_count":<n>,
                         "body_nose_touch_count":<n>,
                         "nose_body_first_high":<值>,"first_high_a":<值>},
    "step3_distribution":{"status":"success","body_tail_te_ok":<n>,
                          "body_le_te_ok":<n>,"body_nose_touch_ok":<n>,
                          "nose_end_space":<值>,"middle_value":<值>,
                          "nose_middle_value":<值>},
    "step4_aniso_line_groups":{"status":"success","group_count":2,"growth_rate":1.2,
                               "control_layer":50,
                               "groups":[{"name":"bodyTailJunction","first_high":<值>,...},
                                         {"name":"bodyNoseJunction","first_high":<值>,...}]},
    "step5_aniso_domains":{"status":"success","tail_body_domain_count":<n>,
                           "nose_body_domain_count":<n>,"growth_rate":1.2}
  }
}
```

> 🔴 上两例**只示意字段结构**（`<值>` 为占位符）——🔴 **`steps` 字段仅用于 `status="failed"` 时定位失败点，不作返回后核验判据**。
> 🔴 返回里**都没有 `step6_tail`**（两个工具都不做弹尾独立加密），这是正常现象，不是失败。

### 失败处理

- `status` 为 `"failed"` 时 → **自动回退到分步流程**（见下「分步回退」），**不停止、不询问用户**。检查 `message` 字段确认卡在哪一步，回退时优先复用它已算出的上下文。
- 常见失败原因：
  - 通用：表面网格未生成、**`GetMissileDimensions` 未调用**（弹翼按对数组为空 / 弹尾口径为 0）
  - 弹翼侧：**带序号子面分组**缺失（未检测到 `wingSideSurfaceN`）
  - 弹身侧：步骤 3 出现 `skip`（A / B / E 类线的共点端间距取不到）→ 该步返 `failed`
- 若某一步返回 `"failed"`，后续步骤不再执行，检查前置条件是否满足
- 🔴 **一个工具失败不影响另一个**：顺序仍是「先弹身、后弹翼」，但**弹身失败时弹翼仍照常执行**（反之亦然），不要把两者的成败互相绑定

## 分步回退（`status="failed"` 时自动进入）

两个一站式工具都是**首选**。仅当其中之一返回 `status="failed"`（接口可调通、内部某步失败）时，**自动**按主 Skill `missile-cfd-meshing-workflow` 的 `references/missile-anisotropic-mesh.md` §「分步回退」的映射表分步重做，**无需用户确认**（用 `read_skill_resource` 读取，skill 参数填 `missile-cfd-meshing-workflow`）。🔴 **只回退失败的那个**，另一个已成功的不重做。

**弹翼侧 —— 按位置加载对应回退子 Skill**，各自完成其「网格线分类 + 分布设置」两步：

| 失败位置 | 加载子 Skill |
|---|---|
| 翼窄面（前缘/后缘/翼梢面）弦向 | `missile-chordwise-direction` |
| 翼前缘线（前缘面 ∩ 侧面公共边）展向 | `missile-spanwise-direction` |
| 翼身结合处（绕翼一圈） | `missile-root-junction` |
| 翼侧面（`wingSideSurfaceN`） | 无独立子 Skill —— 按 `missile-anisotropic-mesh.md` §「各位置参数」**翼侧面行**取值（首层＝`0.1×厚度`、增长率 1.2、层数 50、中间值＝翼面尺寸），直接由下述 `GenerateANisoDomainGrid` 下发 |

> 🔴 **逐对处理**：上述各位置对 N=1/2/3 **各跑一遍**，每对用**该对的厚度与根弦长**算首层高度/端间距/中间值。

**弹身侧 —— 无独立回退子 Skill**，按事实中心 §2.3 的 A~E 分类表现做「线分类 + 分布设置」，再用 `GenerateANisoDomainGrid` 下发两组：

| 一站式内部步骤 | 分步实现 | 读取 |
|---|---|---|
| 1. 尺寸 | 🔴 **先调 `GetMissileDimensions`**，取 `body_diameter_tail` | 事实中心 §2.3 |
| 2. 分类 A~E | 按事实中心 §2.3 的定义逐条判定（分组：`fuselage` / `tail` / `nose` / `wing*N`） | 事实中心 §2.3 |
| 3. 分布设置 | `UGReDimensionSmoothDistribution`（参数格式 `"headRate,headLayer,tailRate,tailLayer"` = `"1.2,10,1.2,10"`）；中间值 A/B 与 E **统一** = `0.05×尾口径` | 事实中心 §2.3 |
| 4–5. 线组 + 域生成 | `GenerateANisoDomainGrid(anisoGroupsJson)` 下发两组（`firstHigh` = A 端间距 / D 首尾端间距平均值，`growthRate` **1.2**，`controlLayer` **50**） | 见下 JSON 构造 |

> 🔴 **回退不含弹尾独立加密** —— 弹尾由后续独立工具处理，回退流程到此为止。

**AI 自动补回退中缺失的两步**（子 Skill 不覆盖）：

- **创建各向异性线组 + 各向异性域生成** → `GenerateANisoDomainGrid(anisoGroupsJson)` 一次下发全部组（构造见 missile-anisotropic-mesh.md §「分步回退」；弹翼 `firstHigh` 一律取 `0.1×该对厚度`、`growthRate` 组A 1 / 组B 1.2；弹身 `firstHigh` 见上表、`growthRate` 1.2）。🔴 **翼后缘面一并纳入**（窄面，增长率 1），**不调用 `GenerateLongAndNarrowFaceGrid`**
- **尺寸**：🔴 **先调 `GetMissileDimensions`**（MCP，需 `enable_tool_group("missile")`）取弹体 D / D_tail / D_surface / R，🔴 **弹翼按对取 `wings[N−1]`**（`root_chord` / `thickness`，下标 = 对数−1，**不得各对共用**），🔴 **接口返回毫米 mm，直接传参，禁止 ×1000**

**终点**：分步仍失败 → 该阶段 `skipped` + `note`（记录卡在哪一步），**不重试、不换参重试**；接口返回恒定值/空（不可用）→ **不回退**，直接 `skipped` + `note`。

## 完成标准

- 🔴 **`MissileBodyAnisoProcessBodyAnisotropy`（弹身）与 `MissileWingAnisoProcessWingAnisotropy`（弹翼）都返回 `status` 为 `"success"`**，且**按「先弹身、后弹翼」的顺序调用**（🔴 **只看 `status`，不做任何返回后核验**）—— 两者都做完本 Skill 才算完成，漏一个该部分没有各向异性
- 弹翼各向异性网格已生成
- **翼后缘面已完成各向异性**（步骤 5 内，`generateType=2`、增长率 1）—— 三角翼/梯形翼一视同仁，无须任何额外后缘处理
- **弹身两处结合处已生成各向异性网格**（`bodyTailJunction` / `bodyNoseJunction`，增长率 1.2、层数 50）
- ⚠️ **弹尾（`tail`）独立加密不属本 Skill 的完成标准**——由后续独立工具处理
