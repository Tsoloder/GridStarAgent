# 导弹各向异性网格参考

## 适用位置

| 位置 | 说明 |
|------|------|
| 翼前缘 | 前缘弧形面加密 |
| 翼后缘 | 后缘面加密（🔴 一律做各向异性 `generateType=2`，三角翼/梯形翼同款、无分支） |
| 翼梢部 | 梢部端面加密（仅梯形翼有 `wingTipN` 面） |
| 翼侧面 | 上下表面加密 |
| 翼结合处 | 翼与弹体连接处加密 |
| ~~弹尾~~ | 🔴 **2026-10-22 起不在弹翼各向异性范围内** —— 由后续独立工具处理，本参考不再给出弹尾参数 |

## 🔴 各位置参数（需求基准；分步回退路径一律以此表为准）

> 🔴 **2026-10-22 口径变更**：尺寸基准改为**翼厚度**（`wings[N−1].thickness`，翼后缘处厚度）——
> 首尾端间距与各向异性首层高度 = **`0.1 × 该对厚度`**；长边分布中间值 = **`0.02 × 该对根弦长`**。
> **旧的 `0.005 × 当地弦长`「加密尺寸」口径已作废**，不得再用于弹翼。

🔴 **"该对" = 该组名末位数字 N 对应的那一对**（`wingSideSurface2` → 第 2 对，取 `wings[1]`），
**不得用某一对的厚度/弦长代跑其他对**。尺寸按对读后端 `SPIDER::Data` 的数组
（`m_wingRootChords[N−1]` / `m_wingThicknesses[N−1]`），由 `GetMissileDimensions` 接口写回。

| 位置 | 首层高度 | 增长率 | 层数 | 中间值 |
|---|---|---|---|---|
| 翼前缘面（`wingLeadingEdgeN`） | `0.1 × 厚度` | **1** | 50 | `0.02 × 根弦长` |
| 翼后缘面（`wingTrailingEdgeN`） | `0.1 × 厚度` | **1** | 50 | `0.02 × 根弦长` |
| 翼梢面（`wingTipN`，**仅梯形翼有，按 M2 探测**） | `0.1 × 厚度` | **1** | 50 | `0.02 × 根弦长` |
| **翼侧面（`wingSideSurfaceN`）· 全部** | `0.1 × 厚度` | **1.2** | 50 | `0.02 × 根弦长` |
| **翼身结合处（绕翼一圈）** | `0.1 × 厚度` | **1.2** | 50 | `0.02 × 根弦长` |

> 🔴 **翼身结合处与翼侧面参数完全一致**（首层高度、增长率、层数、中间值四项全同）——结合处是"翼根那一圈的侧面"，不是另一套参数。
> 🔴 **前缘 / 翼梢 / 后缘三个窄面统一增长率 = 1、层数 = 50**；只有**侧面 / 结合处**用增长率 1.2。
> 🔴 **同一根网格线在窄面上与在宽面上参数不同**：Range 是（网格线, 网格面）二元组，故一条窄面交接线**各写两份**——写在窄面自己上的那份 `growthRate=1`，写在翼侧面上的那份 `growthRate=1.2`，**两份的 `firstHigh` 完全相同**（都取 `0.1×厚度`）。这样交接处两侧同高（需求 4），而翼侧面侧能循增长率放大。
> ⚠️ **历史错法（已废）**：曾把窄面组的可写面集直接给成"翼全部子面（含翼侧面）"，于是翼侧面那份也拿走了 `growthRate=1` —— 增长率 1 即"不放大"，首层之后 50 层恒定，翼侧面靠交接那一半全铺成恒定密网格。**宽面组"排除窄面交接线"指的是通用宽面组不得写它，而不是翼侧面侧不该有 Range**。
> 🔴 **多对翼（N=1/2/3）逐对处理，每对按各自的厚度与根弦长算参数**——不得用某一对的参数代跑其他对。
> 🔴 **翼前缘厚度方向网格点数 ≥ 7**：由"首层 = 0.1×厚度 + 增长率 1 + 层数 50"共同保证，作为完成判据核验，不单独传参。

## 工具

各向异性使用**一站式工具** `MissileWingAnisoProcessWingAnisotropy()`（后端 `MissileWingAnisotropicProcessor`）。
该工具无参数，内部自动依次执行：

1. **尺寸与对数获取** — 按对读后端 `SPIDER::Data` 的 `m_wingRootChords[N−1]` / `m_wingThicknesses[N−1]`（mm）。
   🔴 **弹翼尺寸经 `SPIDER::Data` 中转**（由 `GetMissileDimensions` 接口写回）——
   **未调该接口 → 数组为空 → 本步失败 → 工具整体 `failed`**。
   🔴 旧文档所述「直读尺寸缓存 `wings`、不经 `SPIDER::Data` 中转」（`GetWingPairDims`）**已作废**。
2. **短边/长边分类** — 每对 4 类短边（前缘∩弹身、前缘∩翼梢、翼梢∩后缘、后缘∩弹身）+ 7 类长边
3. **分布设置** — 短边：首尾端间距 = `0.1×厚度`，双曲正切；长边：首尾端间距 = `0.1×厚度`，
   增长分布（首/尾增长率 1.2、层数 10），中间值 = `0.02×根弦长`。🔴 宽面组**排除窄面交接线**
4. **创建各向异性线组** — UGNewANisoDomain type=1，每对**两组**：
   - 组A `wingNarrowN`：短边 + 只属于前缘/翼梢/后缘的长边 → 面 = 前缘面 + 翼梢面 + 后缘面，
     首层 = `0.1×厚度`，增长率 **1**，层数 50
   - 组B `wingBroadN`：前缘/翼梢/后缘∩侧表面 的线 + 侧表面∩弹身 的线 →
     面 = 侧表面 + 与弹身相交处属于弹身的面，首层 = `0.1×厚度`，增长率 **1.2**，层数 50
5. **各向异性域生成** — UGNewANisoDomain type=0, generateType=2（**含翼后缘面**，三角翼/梯形翼一视同仁）：
   组A 的面增长率 **1**、组B 的面增长率 **1.2**

> 🔴 **翼后缘面不单独处理、不按翼型分支**：后缘面（`wingTrailingEdgeN`）已包含在步骤 5 的
> 组A 面集内，增长率按窄面取 **1**。三角翼与梯形翼同款 —— **只要是翼后缘就做各向异性**。
> ⚠️ **不存在独立的"后缘处理"阶段**：流程表中后缘面不单独占位，也不做碎边合并/删面/装配。

> ⚠️ **不含弹尾加密（2026-10-22 变更）**：旧一站式（`MissileAnisotropicProcessor`）的第 7 步
> 弹尾加密已随该工具弃用。返回里**没有 `step6_tail`** 属正常现象，不是失败。
> 弹尾各向异性由后续独立工具处理。

> 🔴 **不做返回后核验**：一站式返回 `status="success"` 即为本阶段结束，**直接进入下一阶段**——
> 不读 `steps` 内的尺寸/分布/线组字段，不查正值、不比对自洽性、不比对各对 `root_chord`/`thickness`、
> 不比对增长率。上表仅供 `status="failed"` 后的分步回退取值使用。

## 判定与路由

| 条件 | 动作 |
|------|------|
| 子面分组存在（`wingSideSurfaceN` 等） | 调用独立 Skill `missile-anisotropy-processing` |
| 子面分组缺失 | 本阶段 `skipped` + `note` |
| 一站式返回 `status="failed"` | **自动回退分步**（见下「分步回退」，无需询问用户） |
| 工具返回接口不可用（恒定 true/false/空） | 本阶段 `skipped` + `note`，**禁止回退**（接口不可用时换分步同样调不通） |

> 🔴 定位源闸门（事实中心 G1）：弹翼**带序号子面分组**（`wingSideSurfaceN` / `wingLeadingEdgeN` / `wingTrailingEdgeN`）即定位键——后端只产出子组名，不存在 `wing` 主组。子组名缺失 → `skipped` + `note`，禁止几何反推。

## 分步回退（一站式 `status="failed"` 时自动进入）

一站式 `MissileWingAnisoProcessWingAnisotropy()` 是**首选**；仅在它返回 `status="failed"`
（工具可调通、但内部某步失败）时**自动**按下表分步重做，**不等用户确认**。
若返回的是恒定值/空（接口根本不可用）→ **不回退**，直接 `skipped` + `note`（换了分步也一样调不通）。

**纪律**：每一步都用实时 Schema 核对参数；参数已由一站式返回的 `message` / `steps` 指出失败点时优先复用其上下文；分步同样失败 → 该阶段 `skipped` + `note` 记录卡在哪一步，**不重试、不换参重试**。

| 一站式内部步骤 | 分步回退实现 | 读取 |
|---|---|---|
| 1. 尺寸与对数 | 🔴 **先调 `GetMissileDimensions`**（MCP，需 `enable_tool_group("missile")`）取弹体 D / D_tail / D_surface / R（单位 mm，**禁止 ×1000**）；🔴 **弹翼按对取 `wings[N−1]`** 的 `root_chord` / **`thickness`**——每对各用该对的值，**不得各对共用** | 事实中心 §2 |
| 2. 短边/长边分类 | 查 `wingSideSurface1` ~ `wingSideSurface3` 判定弹翼对数；厚度/根弦长由接口值算出 | 本文「各位置参数」 |
| 3. 分布设置 | `UGReDimensionConfigDistribution` / `UGReDimensionSmoothDistribution`（参数格式 `"headRate,headLayer,tailRate,tailLayer"`）；匹配对边用 `UGReDimensionMatch` | 三个子 Skill 各自「尺寸参数」节 |
| 4. 创建各向异性线组（type=1） | `GenerateANisoDomainGrid(anisoGroupsJson)` — 一次传全部组 | 见下 JSON 构造 |
| 5. 各向异性域生成（type=0, generateType=2，**含翼后缘面**） | 同上，`GenerateANisoDomainGrid` 一次下发（后缘面一并纳入，增长率取窄面 1） | 见下 JSON 构造 |
| ~~弹尾加密~~ | 🔴 **不在回退范围**（新工具不含弹尾），由后续独立工具处理 | — |

> ✅ **无「狭长面」步骤**：后缘面只在步骤 5 做各向异性（`generateType=2`），**不调用 `GenerateLongAndNarrowFaceGrid`**，也**不存在独立的「后缘处理」阶段**。

**回退路由表**（步骤 3 各位置分别读对应子 Skill）：

| 位置 | 子 Skill |
|---|---|
| 翼窄面（前缘面 / 后缘面 / 翼梢面）弦向分布 | `missile-chordwise-direction` |
| 翼前缘线（前缘面 ∩ 侧面的公共边）展向分布 | `missile-spanwise-direction` |
| 翼身结合处（绕翼一圈 = 前缘根部 + 侧面根部 + 后缘根部 三段并集） | `missile-root-junction` |
| 翼侧面（`wingSideSurfaceN`）分布 | 按上方「各位置参数」表的**翼侧面行**处理（首层＝`0.1×厚度`、增长率 1.2、层数 50、中间值＝`0.02×根弦长`），或在步骤 5 用 `generateType=2` 覆盖 |

> 🔴 **逐对处理**：以上各位置对 N=1/2/3 **各跑一遍**，每对按该对的厚度与根弦长算首层高度/端间距/中间值。

**`GenerateANisoDomainGrid` 的 `anisoGroupsJson` 构造**（每个各向异性组一个元素，一次传全）：

```json
[{"objects":[{"domainID":<面ID>,"connectorIDs":[<线ID>,...]}],
  "firstHigh":<首层高度>,"growthRate":<增长率>,"controlLayer":50}]
```

- `firstHigh`：**一律取 `0.1 × 该对弹翼厚度`**（窄面、侧面、结合处全部相同）——**不得用 `0.005×当地弦长`**
- `growthRate`：**窄面（前缘/后缘/翼梢）＝ 1**；**侧面 / 结合处 ＝ 1.2**；`controlLayer`：50
- ⚠️ 参数名以**实时 Schema** 为准；若 `GenerateANisoDomainGrid` 不可用，退 `UGReDimensionConfigDistribution` 设分布兜底；仍失败则该阶段 `skipped` + `note`。
> ✅ **不做狭长面**：后缘面不调用 `GenerateLongAndNarrowFaceGrid`，只走 `generateType=2` 各向异性。

## 前置条件

1. 表面网格已生成（推荐 `MissileSurfMeshGenerate`）
2. 弹翼**带序号子面分组**已存在（`wingSideSurfaceN`、`wingLeadingEdgeN`、`wingTrailingEdgeN` 等，N = 1/2/3）
3. 🔴 **`GetMissileDimensions` 已调用**（尺寸写回 `SPIDER::Data`，含**按对**的
   `m_wingRootChords` / `m_wingThicknesses`）——一站式工具第 1 步守卫依赖它，未调用会直接 `failed`
4. 若用户对网格数量没有要求，可不做各向异性处理
5. **链路接口可用**：若 `GetConnectorsByDomains` 返回兜底值，则整个各向异性阶段 `skipped`

## 完成标准

- `MissileWingAnisoProcessWingAnisotropy` 返回 `status` 为 `"success"`（🔴 **只看 `status`，不做任何返回后核验**）
- 弹翼网格面已生成各向异性网格
- 后缘面已做各向异性（`generateType=2`，增长率 1）—— 三角翼/梯形翼一视同仁
- ⚠️ **弹尾各向异性不属本阶段完成标准**——由后续独立工具处理
