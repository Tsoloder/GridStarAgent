# 导弹各向异性网格参考

## 适用位置

| 位置 | 说明 |
|------|------|
| 翼前缘 | 前缘弧形面加密 |
| 翼后缘 | 后缘面加密（🔴 一律做各向异性 `generateType=2`，三角翼/梯形翼同款、无分支） |
| 翼梢部 | 梢部端面加密（仅梯形翼有 `wingTipN` 面） |
| 翼侧面 | 上下表面加密 |
| 翼结合处 | 翼与弹体连接处加密 |
| 弹尾 | 尾部加密，**与翼一致**（多对翼取弦长最大的那一对） |

## 🔴 各位置参数（需求基准，回退路径与结果核验一律以此表为准）

“加密尺寸” = `0.005 × 该对当地弦长`；“翼面尺寸” = `0.02 × 该对当地弦长`。🔴 **"该对"= 该组名末位数字 N 对应的那一对**（`wingSideSurface2` → 第 2 对，取 `wings[1].root_chord`），**不得用某一对的弦长代跑其他对**。当地弦长取值见 `missile-geometry-parameters.md` §「当地弦长使用规则」（C_root(N) 取**接口值 `wings[N−1].root_chord`**；三角翼梢部端按事实中心 §2.2.2 钳制）。

| 位置 | 首层高度 | 增长率 | 层数 | 中间值 |
|---|---|---|---|---|
| 翼前缘面（`wingLeadingEdgeN`） | 加密尺寸 | **1** | 50 | 翼面尺寸 |
| 翼后缘面（`wingTrailingEdgeN`） | 加密尺寸 | **1** | 50 | 翼面尺寸 |
| 翼梢面（`wingTipN`，**仅梯形翼有，按 M2 探测**） | 加密尺寸 | **1** | 50 | 翼面尺寸 |
| **翼侧面（`wingSideSurfaceN`）· 全部** | **加密尺寸** | **1.2** | 50 | 翼面尺寸 |
| **翼身结合处（绕翼一圈）** | **加密尺寸** | **1.2** | 50 | 翼面尺寸 |
| **弹尾结合处（弹尾与弹体/弹头共享的那一圈）** | **加密尺寸** | **1.2** | 50 | 翼面尺寸 |

> 🔴 **翼身结合处与翼侧面参数完全一致**（首层高度、增长率、层数、中间值四项全同）——结合处是"翼根那一圈的侧面"，不是另一套参数。
> 🔴 **弹尾结合处与翼侧面同套参数**（2026-10-14 修）——弹尾与弹体交接那一圈的目视密度必须与翼身结合处一致，后端已补 type=1 线组写入同套 Range 参数（`tailJunction` 组）。
> 🔴 **前缘 / 翼梢 / 后缘三个窄面统一增长率 = 1、层数 = 50**；只有**侧面 / 结合处 / 弹尾结合处**用增长率 1.2。
> 🔴 **同一根网格线在窄面上与在宽面上参数不同**：Range 是（网格线, 网格面）二元组，故一条窄面交接线**各写两份**——写在窄面自己上的那份 `growthRate=1`，写在翼侧面上的那份 `growthRate=1.2`，**两份的 `firstHigh` 完全相同**（都取窄面钳制后的值）。这样交接处两侧同高（需求 4），而翼侧面侧能循增长率放大（2026-10-15 修）。
> ⚠️ **历史错法（已废）**：曾把窄面组的可写面集直接给成"翼全部子面（含翼侧面）"，于是翼侧面那份也拿走了 `growthRate=1` —— 增长率 1 即"不放大"，首层 5 之后 50 层恒为 5，翼侧面靠交接那一半全铺成恒定密网格。**宽面组"排除窄面交接线"指的是通用宽面组（`sideSurface`/`bodyJunction`）不得写它，而不是翼侧面侧不该有 Range**（缺 Range 会回落构造函数默认值 firstHigh 0.01 / growRatio 1.15 / 层 10）。
> 🔴 **多对翼（N=1/2/3）逐对处理，每对按各自的当地弦长算参数**——不得用某一对的参数代跑其他对。翼面尺寸/加密尺寸都随该对弦长变。
> 🔴 **翼前缘厚度方向网格点数 ≥ 7**：由"加密尺寸首层 + 增长率 1 + 层数 50"共同保证，作为完成判据核验，不单独传参。
> 🔴 **窄面首层受带宽上限约束**：窄面组首层高度还须满足 `首层 ≤ 带宽/(7−1)`（厚度方向至少 7 点），取 `min(加密尺寸, 带宽/6)`。

## 弹尾加密

弹尾加密与**弹翼一致**：首层高度用加密尺寸、增长率 1.2、层数 50、中间值用翼面尺寸（与翼侧面同套参数）。

> 🔴 **多对翼时，弹尾取"弦长最大的那一对翼"的参数**（各对弦长可差 5 倍以上，取最大对保证弹尾不过疏）。最大对由一站式步骤 2 返回的 `pairs[].mac` / 弦长比较得出；回退路径由 Agent 比较各对 `wings[N−1].root_chord` 得出。

## 工具

各向异性使用**一站式工具** `MissileAnisoProcessMissileAnisotropy()`。该工具无参数，内部自动依次执行：

1. **尺寸数据获取** — 弹体尺寸（弹径 D / 弹尾口径 / 球头半径 / 全弹长）读 `GetMissileDimensions` 写回 `SPIDER::Data` 的值（mm）；🔴 **弹翼尺寸（C_root / C_tip / 半翼展）按对直读尺寸缓存 `wings` 数组**（`GetWingPairDims`，2026-10-16 起不再经 `SPIDER::Data` 中转）。🔴 **未调接口 → 本步失败 → 工具整体 `failed`**（弹体尺寸守卫）
2. **弹翼分类** — 自动检测弹翼对数（`wingSideSurface1` ~ `wingSideSurface3`），**按对**取该对弦长/展长、算该对 MAC（第 N 对取 `wings[N−1]`，取不到回退第 1 对）
3. **网格线分类** — 前缘线 / 后缘长线 / 后缘短线 / 翼身交线 / 翼梢交线
4. **分布设置** — SafeSmoothDist 设平滑分布（**窄面首层＝加密尺寸、增长率 1；侧面/结合处首层＝加密尺寸、增长率 1.2；中间值＝翼面尺寸**，见上表）。🔴 宽面组**排除窄面交接线**，避免覆盖窄面分布
5. **创建各向异性线组** — UGNewANisoDomain type=1；🔴 一条窄面交接线**写两份**（窄面自己 `growthRate=1` / 翼侧面 `growthRate=1.2`，`firstHigh` 同为钳制后的窄面值）；宽面组排除窄面交接线
6. **各向异性域生成** — UGNewANisoDomain type=0, generateType=2（**含翼后缘面**，三角翼/梯形翼一视同仁）
7. **弹尾加密** — 对 `tail` 分组做各向异性：先建 `tailJunction` type=1 线组（**与翼侧面同套参数**），再 type=0 generateType=2，面集含共边的邻接弹体面；取**弦长最大的那一对翼**的参数

> 🔴 **翼后缘面不单独处理、不按翼型分支**：后缘面（`wingTrailingEdgeN`）已包含在步骤 6 的各向异性面集内（`CollectPairAnisoDomains` 含 `wp.trailingEdge`），增长率按窄面取 **1**。三角翼与梯形翼同款 —— **只要是翼后缘就做各向异性**。
> ⚠️ **不存在独立的"后缘处理"阶段**：流程表中后缘面不单独占位，也不做碎边合并/删面/装配（历史 `missile-trailing-edge-processing` 流程已废止）。

> ✅ **一站式工具内部实现（2026-10-14 修 / 2026-10-15 修）**：上表是**需求基准**，后端现已按此实现（尺寸取 `GetMissileDimensions` 接口值、窄面增长率 1、宽面 1.2、后缘含短线、弹尾结合处建 type=1 线组、窄面交接线在翼侧面上单写一份 `growthRate=1.2` 的 Range）。若返回的 `steps` 中首层高度/增长率与上表不符，按上表判定为**结果不合规**，在 `note` 记录并报告后端，但**不因此改走回退**（回退只由 `status="failed"` 触发）。

## 🔴 一站式结果核验（返回 `status="success"` 后必做，两查）

🟡 **前提**：`MissileAnisoProcessMissileAnisotropy()` **无参数**——Agent 侧尺寸**传不进去**。弹体尺寸由**后端读取 `GetMissileDimensions` 写回的 `SPIDER::Data`**，**弹翼尺寸由后端按对直读尺寸缓存 `wings`**（缓存由分割流水线实算写入，2026-10-17 起不再写死）。因此 `status="success"` **只证明流程跑通，不证明尺寸可用**，按下两查核验后才可宣告本阶段成功。

### 查 1：尺寸可用性（`step1_dimensions` / `step2_classify`）

🔴 **旧"与固定值比对、偏差 ≤5%"核验已作废**（固定值已随 2026-10-14 裁定取消）。改为**只查正值与自洽性**：

| 后端字段 | 判据 | 为何关键 |
|---|---|---|
| `step1_dimensions.body_diameter` | 🔴 **> 0** | D 是弹体/尾部/二面角加密的尺寸基准 |
| `pairs[].root_chord` / `local_chord` | 🔴 **> 0** | **主控项**——`firstHigh = 0.005×当地弦长`、`middle = 0.02×当地弦长`，弦长为 0 则 Range 参数被判非正、各向异性整段跳过。🔴 **每对都要查**，且应各自等于该对 `wings[N−1].root_chord`（不得各对相同——相同即说明回退到了第 1 对） |
| `pairs[].half_span` | 🔴 **> 0** | 展向分布跨度 |
| `step1_dimensions.body_diameter_surface` | 须与事实中心 §2.2.1 **自洽**：`\|D − D_tail\| > 0.7×D_tail` → = 尾口径；否则 = D | 保证表面网格与各向异性用同一套口径 |

- **全部 > 0 且自洽** → 正常，记录实测值后继续。
- 🔴 **任一 ≤ 0 或 `body_diameter_surface` 不自洽** → **不得静默接受**（此时网格是照错误尺寸生成的，`success` 具误导性）：
  1. **暂停本阶段**，把实测值列给用户并附一句"尺寸来源不可用，请确认 `ClassifyMissile` 是否已跑过、`GetMissileDimensions` 是否已调用（尺寸缓存是否已写入）"。
  2. 本阶段 `note` 标记 **`dimension_unavailable`** + 附实测值，**报告后端**；结果按"尺寸口径不合规"上报，**不得描述为正常完成**。
  3. ⚠️ 一站式**无法用外部参数覆盖**（无参数接口），纠正只能在后端（先跑 `ClassifyMissile` 让流水线实算尺寸，再补调 `GetMissileDimensions`；`wings` 由分割结果的翼对推出，不再有手填固定表）。
- ⚠️ 无论哪种，**均不触发分步回退**（回退只由 `status="failed"` 触发）。

> ✅ **尺寸来源变更（2026-10-16 按对 / 2026-10-17 实算）**：弹翼尺寸为**按对直读尺寸缓存 `wings`**（后端 `GetWingPairDims`），不经 `SPIDER::Data` 中转；缓存由 `ClassifyMissile` 第 10 步 `CalculateDimensions()` 实算写入，`wings` 缺失的对回退第 1 对。因此本查**不再有"基准值"可比**——只查正值与自洽，外加一条：**各对 `root_chord` 应互不相同**（同一型号各对弦长本就不同）。🔴 **先跑 `ClassifyMissile` 才有尺寸**；若从未跑过分割（缓存为空），`GetMissileDimensions` 返回 `{}`、各向异性一站式也会因弹体尺寸守卫直接 `failed`。

### 查 2：窄面增长率一致性（`step3_distribution` vs `step4_aniso_line_groups`）

按**组名后缀**判定，不看角色标签：

| 组名 | 增长率 | 首层高度 |
|---|---|---|
| `leadingEdge` / `trailingEdge` / `tip`（step3、step4 均有） | **1** | 该窄面钳制后的值 |
| `leadingEdgeSide` / `trailingEdgeSide` / `tipSide`（**仅 step4 有**） | **1.2** | **与上一行完全相同** |
| `sideSurface` / `bodyJunction` / `ungrouped` | 1.2 | 加密尺寸 |

- 窄面组（**不带 `Side` 后缀**）在两处**都 = 1** → 通过。
- 🔴 **任一处 ≠ 1** → 该位置 `note` 标记 **`growth_rate_mismatch`** + 记下实际值，**报告后端**。**以上表为准判定为结果不合规**，不得因"返回 success"而放过。
- 🔴 **`*Side` 组必须存在且 = 1.2**（2026-10-15 新增判据）：这是"翼侧面与翼前缘交接处"在翼侧面那一侧的 Range。缺失 → 交接处会回落构造函数默认值（firstHigh 0.01 / growRatio 1.15 / 层 10）→ `note` 标记 `side_range_missing` + 报告后端。
- 🔴 **`*Side` 组的 `first_high` 必须与同名窄面组相等**（交接处两侧同高，需求 4）。不等 → `note` 标记 `junction_height_mismatch` + 记两侧实测值 + 报告后端。
- ⚠️ `trailingEdgeSide` 在三角翼下可能因后缘短线归属而不存在（`pair_count_on_side = 0`），此时按"该窄面与翼侧面不共线"正常处理，不报缺。

> ✅ **历史不自洽已于 2026-10-13 修复**：曾出现 `step3` 报 `trailingEdge=1` ✅ 而 `step4` 报 `teLong=1.2` ❌（组名不一致导致角色误判）。现已统一组名为 `trailingEdge`（长线 ∪ 短线）。本条保留作为**回归护栏**。
> ✅ **2026-10-15 修复**：曾把窄面组的可写面集给成"翼全部子面（含翼侧面）"，导致翼侧面那份 Range 也拿到 `growthRate=1`（不放大）→ 翼侧面靠交接那一半恒为首层高度。现拆为窄面组 + `*Side` 组，两份 `firstHigh` 相同、增长率各异。`step4` 的 `diag.leadingEdge_pairs[].aligned` 与 `pair_count_on_side` 可直接核验本项（`aligned=true` 表示两侧都写上了）。

## 判定与路由

| 条件 | 动作 |
|------|------|
| 子面分组存在（`wingSideSurfaceN` 等） | 调用独立 Skill `missile-anisotropy-processing` |
| 子面分组缺失 | 本阶段 `skipped` + `note` |
| 一站式返回 `status="failed"` | **自动回退分步**（见下「分步回退」，无需询问用户） |
| 工具返回接口不可用（恒定 true/false/空） | 本阶段 `skipped` + `note`，**禁止回退**（接口不可用时换分步同样调不通） |

> 🔴 定位源闸门（事实中心 G1）：弹翼**带序号子面分组**（`wingSideSurfaceN` / `wingLeadingEdgeN` / `wingTrailingEdgeN`）即定位键——后端只产出子组名，不存在 `wing` 主组。子组名缺失 → `skipped` + `note`，禁止几何反推。

## 分步回退（一站式 `status="failed"` 时自动进入）

一站式 `MissileAnisoProcessMissileAnisotropy()` 是**首选**；仅在它返回 `status="failed"`（工具可调通、但内部某步失败）时**自动**按下表分步重做，**不等用户确认**。若返回的是恒定值/空（接口根本不可用）→ **不回退**，直接 `skipped` + `note`（换了分步也一样调不通）。

**纪律**：每一步都用实时 Schema 核对参数；参数已由一站式返回的 `message` / `steps` 指出失败点时优先复用其上下文；分步同样失败 → 该阶段 `skipped` + `note` 记录卡在哪一步，**不重试、不换参重试**。

| 一站式内部步骤 | 分步回退实现 | 读取 |
|---|---|---|
| 1. 尺寸数据获取 | 🔴 **先调 `GetMissileDimensions`**（MCP，需 `enable_tool_group("missile")`），取弹体 D / D_tail / D_surface / R（单位 mm，**禁止 ×1000**）；🔴 **弹翼按对取 `wings[N−1]`**（`root_chord` / `tip_chord` / `half_span`）——每对各用该对的弦长，**不得各对共用**；C_tip(N) **按 M2 探测结果分叉**——三角翼（无 `wingTipN`）钳制到**该对** C_root(N) 计算值，梯形翼按接口值 | 事实中心 §2 |
| 2. 弹翼分类、MAC | 查 `wingSideSurface1` ~ `wingSideSurface3` 判定弹翼对数；弦长/展长/MAC 由接口值算出 | 本文「尺寸参数」 |
| 3. 网格线分类（五类线） | 由三个回退子 Skill 各自完成（见下路由表） | 三个子 Skill |
| 4. 分布设置 | `UGReDimensionConfigDistribution` / `UGReDimensionSmoothDistribution`（参数格式 `"headRate,headLayer,tailRate,tailLayer"`）；匹配对边用 `UGReDimensionMatch` | 三个子 Skill 各自「尺寸参数」节 |
| 5. 创建各向异性线组（type=1） | `GenerateANisoDomainGrid(anisoGroupsJson)` — 一次传全部组 | 见下 JSON 构造 |
| 6. 各向异性域生成（type=0, generateType=2，**含翼后缘面**） | 同上，`GenerateANisoDomainGrid` 一次下发（后缘面一并纳入，增长率取窄面 1） | 见下 JSON 构造 |
| 7. 弹尾加密 | 同上，`GenerateANisoDomainGrid` 对 `tail` 分组组下发（参数取弦长最大的那一对翼） | 本文「弹尾加密」 |

> ✅ **无「狭长面」步骤**：后缘面只在步骤 6 做各向异性（`generateType=2`），**不调用 `GenerateLongAndNarrowFaceGrid`**，也**不存在独立的「后缘处理」阶段**（`missile-trailing-edge-processing` 已废止）。回退路径同样不碰狭长面。

**回退路由表**（步骤 3–4 各位置分别读对应子 Skill）：

| 位置 | 子 Skill |
|---|---|
| 翼窄面（前缘面 / 后缘面 / 翼梢面）弦向分布 | `missile-chordwise-direction` |
| 翼前缘线（前缘面 ∩ 侧面的公共边）展向分布 | `missile-spanwise-direction` |
| 翼身结合处（绕翼一圈 = 前缘根部 + 侧面根部 + 后缘根部 三段并集） | `missile-root-junction` |
| 翼侧面（`wingSideSurfaceN`）分布 | 按上方「各位置参数」表的**翼侧面行**处理（首层＝加密尺寸、增长率 1.2、层数 50、中间值＝翼面尺寸），或在步骤 6 用 `generateType=2` 覆盖 |

> 🔴 **逐对处理**：以上各位置对 N=1/2/3 **各跑一遍**，每对按该对弦长算加密尺寸/翼面尺寸；弹尾加密取弦长最大的那一对翼的参数（见「弹尾加密」）。

**`GenerateANisoDomainGrid` 的 `anisoGroupsJson` 构造**（每个各向异性组一个元素，一次传全）：

```json
[{"objects":[{"domainID":<面ID>,"connectorIDs":[<线ID>,...]}],
  "firstHigh":<首层高度>,"growthRate":<增长率>,"controlLayer":50}]
```

- `firstHigh`：**一律取加密尺寸** `0.005 × 当地弦长`（窄面、侧面、结合处、弹尾全部相同）——**不得用 `0.1%×MAC`**
- `growthRate`：**窄面（前缘/后缘/翼梢）＝ 1**；**侧面 / 结合处 / 弹尾 ＝ 1.2**；`controlLayer`：50
- ⚠️ 参数名以**实时 Schema** 为准；若 `GenerateANisoDomainGrid` 不可用，退 `UGReDimensionConfigDistribution` 设分布兜底；仍失败则该阶段 `skipped` + `note`。
> ✅ **不做狭长面**：后缘面不调用 `GenerateLongAndNarrowFaceGrid`，只走 `generateType=2` 各向异性。

## 前置条件

1. 表面网格已生成
2. 弹翼**带序号子面分组**已存在（`wingSideSurfaceN`、`wingLeadingEdgeN`、`wingTrailingEdgeN` 等，N = 1/2/3）
3. 🔴 **`GetMissileDimensions` 已调用**（弹体尺寸写回 `SPIDER::Data`）——一站式工具后端的弹体尺寸守卫依赖它，未调用会直接 `failed`（弹翼尺寸按对直读尺寸缓存 `wings`，不由本接口写回，但该缓存同样要求先跑过 `ClassifyMissile`）
4. 若用户对网格数量没有要求，可不做各向异性处理
5. **链路接口可用**：若 `GetConnectorsByDomains` 返回兜底值，则整个各向异性阶段 `skipped`

## 完成标准

- `MissileAnisoProcessMissileAnisotropy` 返回 `status` 为 `"success"`
- 🔴 **已通过上方「一站式结果核验」两查**：尺寸全部 > 0 且 `body_diameter_surface` 自洽（或已按 `dimension_unavailable` 上报用户/后端）、窄面增长率两处均为 1（或已按 `growth_rate_mismatch` 上报）
- 弹翼网格面已生成各向异性网格
- 后缘面已做各向异性（`generateType=2`，增长率 1）—— 三角翼/梯形翼一视同仁
- 🔴 **弹尾结合处与翼身结合处目视密度一致**（`tailJunction` 线组已建、同套参数）
