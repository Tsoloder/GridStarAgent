---
name: missile-cfd-meshing-workflow
description: 导弹 CFD 非结构网格生成全流程路由 — CAD 导入、水密性消除、导弹 AI 分部件、碎面合并（条件触发）、导弹几何参数 (D / R / 弦长)、表面网格、各向异性 (可选，弹翼 + 弹身)、弓形外场、空间网格、边界条件、质量检查（面网格最小角）、保存与导出。当用户提到导弹、导弹网格、弹翼、弹头 / 弹体 / 尾部, 各向异性, 后缘加密, 弹身加密, 外场, 边界条件, 质量检查, 导出网格时必须使用。
aliases: [导弹网格, 网格生成, 导弹部件分割, 碎面合并, 表面网格, 空间网格, 边界条件, 质量检查, 体网格质量, 各向异性, 后缘加密, 弹身各向异性, 弓形外场, 水密性, 继续生成网格]
tags: [导弹, CFD, 网格, 工业软件, 各向异性, 弓形外场]
category: CFD
version: 2.5.0
author: GridStarAgent
allowed-tools: []
---

# 导弹 CFD 非结构网格生成工作流

> 🔴 **零记忆可独立运行（硬性原则）**：本 Skill（含全部 references 与子 Skill）是**唯一运行依据**，**不依赖、不读取、不写入任何记忆层**。缺失知识以**实时 Schema / 现场计算 / 实时探测 / 用户输入**为准，**不得从记忆中补**。开发期资料（实测值、接口就绪状态、`t.py` 需求文件）**不属于运行依据**，即使存在于 `doc/` 也不得指导运行。

本 Skill 只路由、决策、核查，不承载详细参数。工具清单、参数语义、ID 换算规则等见各阶段 reference 对应 §。**未显式列在本 Skill 表格中的工具**（包括 `cad`、`generation` 组）调用前先以实时 Schema 为准。

## 🔴 事实中心（先读，全部跨文件硬事实的唯一出处）

**`references/missile-calibration-constants.md`**。含：型号事实（部件体系、**翼型运行时探测规则**、**弹尾口径用途**、**表面网格有效弹径规则**）、🔴 **尺寸取值（接口返回值唯一权威，旧固定值已作废）**、外场编码（**方向位映射 + centerCoor 直接用默认包围盒中心**）、幽灵块、通用纪律。**运行中凡涉及这些事实，一律引此文件 #N，禁止复制长注记。**

> 🔴 **`GetMissileDimensions` 是 MCP 工具**（注册在 `missile` 工具分组，调用前先 `enable_tool_group("missile")`）——尺寸**唯一权威来源**。历史文档曾写"不是 MCP 工具、不可调用"，**该说法已作废**。
> 🔴 **2026-10-14 用户裁定**：接口返回值可用，**D / 各对弹翼弦长 `wings[]` / 半翼展等一律直接用接口值**；旧"固定值覆盖接口"的做法**已全部作废**。🔴 **接口返值单位就是 mm，直接传参，禁止再 ×1000**。
> 🔴 **2026-10-17 更新**：尺寸**恢复为几何实算**——由 `ClassifyMissile` 第 10 步 `CalculateDimensions()` 算出并缓存，`GetMissileDimensions()` 读同一份缓存。**必须先跑分割再取尺寸**（未跑分割返回 `{}`，不再是固定数值）。skill 侧始终不记数值。

> 🔴 **2026-10-16 起弹翼按对取值**：弹翼尺寸是**数组** `wings[]`（下标 = 对数−1，每项 `root_chord` / `tip_chord` / **`thickness`**）——第 N 对翼（`wingSideSurfaceN`）一律取 `wings[N−1]`，**不得用某一对的弦长/厚度代跑其他对**。详见事实中心 §2.1。
> 🔴 **2026-10-22 起各向异性尺寸基准是「翼厚度」**：首层高度/端间距 = `0.1 × wings[N−1].thickness`，长边中间值 = `0.02 × wings[N−1].root_chord`。旧口径 `0.005 × 当地弦长`、旧标量键 `wing_tip_chord`/`wing_half_span` **均已作废**。
> 🔴 **2026-10-26 起各向异性分弹身、弹翼两个工具**：`MissileBodyAnisoProcessBodyAnisotropy`（弹身，尺寸取 `m_bodyDiameterTail`）+ `MissileWingAnisoProcessWingAnisotropy`（弹翼）—— **两者都要调，且顺序固定：先弹身、后弹翼**。弹身参数见事实中心 §2.3。

## 认定流程

**满足导弹部件体系即走导弹流程**：`nose` / `fuselage` / `tail` + **弹翼带序号子组**（`wingLeadingEdgeN` / `wingSideSurfaceN` / `wingTrailingEdgeN` / `wingTipN`，N=1/2/3；fuselage=弹体）。🔴 **后端只产出子组，不存在 `wing` 主组** —— 子组名即定位键，**无归并步骤、无旧名混用问题**（见 `references/missile-segmentation.md`「子面分组即定位键」）。**不满足**或是纯飞机/发动机模型 → **停止，提示用户先用飞机流程。**

🔴 **翼型不写死，运行时探测**：**`wingTipN` 存在 = 梯形翼；不存在 = 三角翼**（正常缺失，非分组不全）。三角翼与梯形翼在 C_tip、拐角表现形式、梢部定位上有差异——**两分支都必须能跑**（拐角数两型**都是 4 处**，差异在 ② 翼梢是尖点还是端面），差异见事实中心 M2「翼型影响」。探测结果写入阶段 `note`。🔴 **后缘不在差异之列**——翼后缘一律做各向异性，与翼型无关。

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema，每个工具用实时 Schema 构造参数。
2. **只读取当前阶段需要的 reference**（见下表），不要预读后续阶段。
3. 🔴 **开工先做「模型是否已就绪」三层查询**（防止"已导入的模型被当成没导入"），按顺序执行，**任何一层命中都不得重复导入**：
   - **第 1 层：查数模是否已在工程** —— `GetAllObjectByType(2)`（数模面）或 `(4)`（超面；都查更稳妥，实时 Schema 为准）：非空 → 跳过 `ImportCADFile`；空 → 才引导导入（`missile-cad-to-mesh.md` §1）。
   - **第 2 层：查工程是否已打开/已有超面** —— `GetAllObjectByType(4)` 或 `GetCurrentSelectedIDs` 复核；非空且第 1 层空 → 数模面已提取为超面，不重复导入。
   - **第 3 层：查分组是否已就绪** —— `GetAllSpitAssemblyGroupProperty`：分组已存在 → 跳过 `ClassifyMissile`；缺失（且数模已在）才需要分割，并**先向用户索取连接参数**（`missile-segmentation.md`）。🔴 **注意尺寸缓存**：尺寸由 `ClassifyMissile` 第 10 步实算并缓存（阶段 5 也走阶段 3，见流程 A 表）——若跳过了分割导致 `GetMissileDimensions()` 返回 `{}`，需补跑一次分割才能取到尺寸。
   - 🔴 **判定原则**："工程里有没有模型"与"有没有分组"是两回事。模型已在（数模面/超面非空）≠ 需要重新导入；分组缺失只代表"没分割/手动分组"，不代表"没导入"。**严禁仅凭"分组不存在"就退回导入数模**。查询只需各调一次，不得反复重试。

## 🔴 上下文治理纪律（全链强制，违反即视为流程违规）

1. **阶段事实卡**：每阶段结束写一段 ≤8 行的"事实卡"（本阶段全部已确认参数：尺寸/分组名/返回值）；下一阶段只读事实卡，不回头翻旧对话/旧 JSON。🔴 **面 ID 不入事实卡**——碎面合并会改超面集合，面 ID 每次进入各向异性/后缘阶段前按组名重查（事实中心 G2）。
2. **不重复输出**：工具返回的长 JSON/清单只改必要字段再回传（尤其 `groupProperty`——🔴 **必要字段 = `domain[]` 与 `line[]` 的 `targetSize`/`minSize`/`angle` 共 6 处，两者必须取同一组计算值；`line` 的 0 若不改会使表面网格生成永久卡死**，见 `missile-surface-mesh.md`「groupProperty 修改纪律」）；不重述已办/待办清单（连续同类调用前只写"当前动作"）；每步思考流只写"结论 + 决策理由 + 下一步"，不推演备选方案、不自我复述约束。
3. **查询减量与批量**：只读查询能合并就合并、能抽样就抽样（每类 ≤10 次）；**一次批量拿全，绝不逐条轮询**——`GetConnectorsByDomains` 等支持一次传多个 `domain_ids`，禁止"对分组内每个面逐个调"的循环写法（各子 Skill 中的 `for id in groupId` 循环均以本条为准）。禁止全量逐条。
4. **规划/建组工具一次传全参数**：`update_plan` / `plan` / 建组类工具发起前先核对实时 Schema，**一次传全必填字段**（尤其 `title`），不靠反复重试。
5. 🔴 **省时纪律（2026-10-23 用户裁定）**：**工具返回成功即进入下一阶段，不做"返回后核验"**。已删除的核验动作包括：外场建后包围盒/锥顶校验与目视复核（`CreateMissileFarFieldBlock` 返回 `status="success"` 即止）、各向异性 `steps` 逐项比对、表面网格生成后的规模/密度自检。这些检查**既不产生决策、又纯耗轮次**。🔴 **唯一例外是失败路径**：返回 `"false"` / `status="failed"` 时才按各阶段失败处理排查。🔴 **删核验 ≠ 删闸门**——水密性自由边 = 0、边界条件首次铺底 `-10`、分部件分组存在性等**硬闸门仍必须执行**。
6. 🔴 **ID 类型不混用**：`nose`/`fuselage`/`wing*`/`tail` 分组返回的是**超面 ID**；端点/网格线查询（`GetStartAndEndPointByConnectors` 等）吃的是**网格面/网格线 ID**。凡"取分组几何"一律先 `GetAllSpitAssemblyGroupProperty` 取 `domain[].ids`（网格面 ID）再查，**不得拿超面 ID 直接去查端点**（2026-10-23 实测踩过，白跑一轮）。🔴 **例外：`CreateMissileFarFieldBlock`**——它在后端直接读分组网格面包围盒，Agent 无需、也不要为它去查分组 ID。

## 流程 A：从 CAD 到网格导出（完整链）

阶段链： CAD 导入与单位设置 → 水密性处理与自由边检查 → 导弹 AI 分部件 (ClassifyMissile) → 碎面合并（条件触发） → 导弹几何参数 → 表面网格生成 → 各向异性 (可选，含弹翼后缘) → 体网格块创建、外场生成 → 空间网格 → 边界条件设置 → 质量检查（面网格最小角） → 保存与导出。

| #  | 阶段 / 触发意图 | 读取 |
|---|---|---|
| 1 | CAD 导入、单位设置（🔴 先查数模是否已在工程，非空则跳过导入，见「开始任务前」三层查询） | `missile-cad-to-mesh.md` §1 |
| 2 | 水密性处理、缝隙、自由边检查 | `missile-cad-to-mesh.md` §2 |
| 3 | 导弹分部件、AI 分割（产出**带序号弹翼子组**，无主组、无归并） | `missile-segmentation.md` |
| 4 | 碎面、合并面、合并碎面（⚠️ 会改超面集合 → **合并后按组名重查分组**，事实中心 G2） | `missile-cad-to-mesh.md` §3.1 |
| 5 | 弹径/弹尾口径/球头半径、**各对弹翼弦长与厚度 `wings[]`**、尺寸测量（🔴 全部**取 `GetMissileDimensions` 接口返回值**，见事实中心 §2；🔴 **须先完成阶段 3 分割**，尺寸由分割实算缓存；接口返 **mm**，禁止 ×1000） | `missile-geometry-parameters.md` |
| 6 | 导弹表面网格生成 + 碎线合并（🔴 用 **`MissileSurfMeshGenerate`**，无入参，后端按导弹口径逐组自动设尺寸并合并网格线；🔴 **前置：`GetMissileDimensions` 已调用**） | `missile-surface-mesh.md` |
| 7 | 各向异性（可选），🔴 **两个一站式工具都要调、🔴 顺序固定：先弹身、后弹翼 —— 漏一个该部分就没做、顺序颠倒即违规**：**(a) 弹身** `MissileBodyAnisoProcessBodyAnisotropy`（🔴 **2026-10-26 新增，必须最先执行**）—— 弹尾∩弹身（`bodyTailJunction`）与弹头∩弹身（`bodyNoseJunction`）两处结合处加密；定位键 `fuselage`/`tail`/`nose`，**不依赖弹翼子组**，故弹翼子组缺失时**照常调用**。**(b) 弹翼** `MissileWingAnisoProcessWingAnisotropy`（🔴 **2026-10-26 起固定排在弹身之后**）—— 前缘・后缘・梢部・侧面・结合处加密（**带序号子面分组存在才做**，事实中心 G1）。🔴 **翼后缘一律做各向异性（`generateType=2`），三角翼与梯形翼同款、无分支**——只要是 `wingTrailingEdgeN` 就做。三角翼无 `wingTipN` 面 → 仅「梢部」一项自然 `skipped`，不影响其余位置。🔴 **两者失败（`status="failed"`）时自动回退分步**（`missile-chordwise-direction` / `missile-spanwise-direction` / `missile-root-junction` + `GenerateANisoDomainGrid`），见 `missile-anisotropic-mesh.md` §「分步回退」；接口恒定值/空则**不回退**、直接 `skipped`。⚠️ **本阶段不含弹尾独立加密** | `missile-anisotropic-mesh.md`（判定 → 路由到独立 Skill `missile-anisotropy-processing`） |
| 8 | 体网格块创建、外场生成（🔴 **一键 `CreateMissileFarFieldBlock()`（无入参）**——方向位推导、`geoParam` 倍数、`centerCoor`、`meshSize` 与 `UGBlockCreate` 调用全在后端完成，见事实中心 #3–#6；🔴 **只看返回 `status`**，`"success"` 即完成，不做建后核验） | `missile-farfield-and-volume.md` §6.1–6.3 |
| 9 | 空间网格 / 体网格 / 附面层（🔴 `UGUGSp` ids 先剔幽灵块，事实中心 #7/§4） | `missile-farfield-and-volume.md` §6.4 |
| 10 | 边界条件、BC、物面、远场、对称 | `missile-farfield-and-volume.md` §7.2 |
| 11 | 网格质量检查：面网格最小角（🔴 **只查面网格**，不查体网格） | `missile-farfield-and-volume.md` §7.1 |
| 12 | 保存工程、导出网格、CGNS | `missile-farfield-and-volume.md` §7.3 |

> 各阶段 reference（`missile-*.md`）均位于 `references/` 目录下，表格中省略路径前缀；子 Skill 同理。

## 流程 B：表面网格已完成，继续后续

用户明确表示表面网格已生成时入此流程，跳过 1–6，自动推进 7–12（见流程 A 表，顺序不动）。

## 硬闸门（任何流程都不得违反）

1. **顺序闸门**：表面网格后固定顺序 `各向异性（可选，含弹翼后缘）→ 体网格块创建、外场生成 → 空间网格 → 边界条件 → 质量检查 → 保存导出`，严禁跳级；各向异性跳过时须在 note 说明。
2. **水密闸门**：表面网格生成前必须**实际调用 `DealWatertight` 并验证 `free_edge_count` 为 0**。🔴 不得以"分组已存在 / ClassifyMissile 已运行过 / 记忆中有公差值"为由跳过。🔴 **重试策略**：仍有自由边 → 公差 **×5** 重试，🔴 **最多 15 次重试**（累计 ≤16 次调用），**到自由边为 `0` 即停止**。🔴 **模式差异**：**auto 模式自动重试**（重试循环不打断用户，跑完为止）；**manual 模式**出现非半模线自由边且重试未能消除时**停下来请用户手动修复**。达 15 次上限仍不过 → 停止并报告当前自由边数与所用公差。自由边全部位于半模线位置时可接受（视同成功、不再重试）。详见 `missile-cad-to-mesh.md` §2.2。
3. **边界闸门**：首次铺底**必须统一置 `-10`（无 BC）**，严禁在铺底阶段写入 4/7/8 等实际属性值。🔴 实际属性（物面→粘性固壁、外场→远场、对称→对称）**只能在铺底完成后、按用户明确要求**通过 `BorderConditioConfigProperty` 逐组设置；**用户未明确要求前不写入任何实际属性**。物面可按分部件情况拆成多个物面组。
4. **质量闸门**：🔴 **只查面网格** —— 面网格最小角 > 10°（各向异性单元处不计）。🔴 **不查体网格**：`ExamineBlock(ExamineMaximumIncludeAngle)` 对整套体网格执行时后端**无响应挂死**（四次实跑均卡住，需用户手动停止），导弹流程**不再调用该工具**，体网格质量指标（≤178°/禁止 179.9°）**本流程不检查**。工具与判据见 `missile-farfield-and-volume.md` §7.1。
5. **前置条件闸门**：分割结果必须由分组查询验证；**带序号子面分组**（`wingSideSurfaceN` 等）缺失时不得调用 `MissileWingAnisoProcessWingAnisotropy`（🔴 **弹翼工具**；弹身工具不依赖弹翼子组，照常调用）。🔴 **尺寸前置**：`GetMissileDimensions` 未调用 → 按对尺寸数组为空 / 弹尾口径为 0 → 两个各向异性工具**都会**必 `failed`，须先补调接口。
6. 🔴 **各向异性结果判定闸门（只看 `status`，不做返回后核验）**：一站式 `MissileWingAnisoProcessWingAnisotropy()` **与** `MissileBodyAnisoProcessBodyAnisotropy()` **都**返回 `status="success"` **即为本阶段结束**，**直接进入阶段 8（体网格块创建、外场生成）**。🔴 **不得做任何返回后核验**——不读 `step1_dimensions` / `step2_classify` / `step3_distribution` / `step4_aniso_line_groups` / `step5_aniso_domains`，不查尺寸是否为正值、不比对 `body_diameter_surface` 自洽性、不比对各对 `root_chord`/`thickness`、不比对窄面增长率与首层高度。这些检查在此处既不产生决策、又纯耗轮次，已全部删除。🔴 **只有 `status="failed"` 触发自动分步回退**；接口恒定值/空（不可用）→ `skipped` + `note`，不回退。详见 `missile-anisotropic-mesh.md` §「判定与路由」/「分步回退」。
7. **碎面合并（条件触发）**：用户提到碎面、合并面、合并碎面时，在分部件之后、表面网格之前执行（调用 `UGSurfaceProcessing`）。🔴 用户未提及时不作为必经阶段，标记 `skipped` + `note`；**但若进入表面网格后出现"不封闭"或"网格异常偏密"，须回头补做**（两者同源，见 `missile-mesh-sealing-check.md`「CAD 拓扑体检」）。
8. **几何闸门**：🔴 **`GetMissileDimensions` 返回值是唯一权威来源，一律直接使用**（任一字段非零即用该值计算对应部件目标尺寸；任一字段为 0 → 该部件按全局默认尺寸生成）。**无固定值例外**（旧 D/C_root/半翼展 固定值已作废，事实中心 §2）。🔴 **弹翼按对取**：第 N 对翼（`wingSideSurfaceN`）一律用 `wings[N−1].root_chord`，**不得各对共用**（各对弦长可差 5 倍以上）。🔴 **表面网格口径**：非球头部分取 `body_diameter_surface`（失配 >70% 时为尾口径），**球头公式仍用原始 `body_diameter` + `nose_radius`**（事实中心 §2.2.1）。🔴 **C_tip 按对并按翼型分叉**（事实中心 M2 / §2.2.2）：三角翼（无 `wingTipN`）≈0 正常，**禁止 0 代入，梢部端间距钳制到该对 C_root 计算值**；梯形翼（有 `wingTipN`）按接口值正常使用。🔴 **接口返值单位本就是 mm，禁止再 ×1000**。**若接口返回异常（0/负值）→ 原样记录并在 note 注明、报告后端，不自行替换/反推**。

**除此八条外，别无他门。**（第 7 条为条件触发，非"不合并即违规"。）

## 工具与替代路径

- `ClassifyMissile` 失败 → 走分组查询
- 🔴 **各向异性一站式失败**（`MissileWingAnisoProcessWingAnisotropy` 或 `MissileBodyAnisoProcessBodyAnisotropy` 返回 `status="failed"`）→ **自动回退分步**：弹翼侧 `missile-chordwise-direction` / `missile-spanwise-direction` / `missile-root-junction` 各做「线分类+分布设置」，缺失的「线组创建/域生成」用 `GenerateANisoDomainGrid`；弹身侧按事实中心 §2.3 的 A~E 分类取值。映射表见 `missile-anisotropic-mesh.md` §「分步回退」。**接口恒定值/空（不可用）→ 不回退，直接 skip+note**。⚠️ 回退**不含弹尾独立加密**（弹尾由后续独立工具处理）
- `UGSurfaceProcessing` 失败（每次≤1次换参重试）→ `UGDelRedundantDom`，再失败则 skip+note
- 体创建失败（`CreateMissileFarFieldBlock` 返回 `status="failed"`，`message` 含不封闭线清单） → `missile-mesh-sealing-check.md`（排查手册）

## 工具可用性判定（收敛引用）

本链路的工具按返回值分三类处理，**各阶段不得自行定义一套**：

| 返回情况 | 判定 | 处理 |
|---------|------|------|
| 返回正常数据（非空/非恒 true/非恒 false） | 接口可用 | 正常使用 |
| 返回恒定值（恒 `"true"`/`"false"`/`[]` → 语义歧义） | 先抽样确认 → 不可用 | 该工具相关阶段 `skipped` + `note`，**禁止重试、禁止换参重试** |
| 返回空（`""`/`null`/超时） | 接口不可用 | 同上 |

> ⚠️ 特殊说明：**`GetMissileDimensions` 是 MCP 工具**（`missile` 分组，调用前先 `enable_tool_group("missile")`），**属于本表「返回正常数据 → 正常使用」类**。返回 `{}` 表示分组缺失、恒 `"true"` 为兜底值，此时按「返回恒定值」判定。尺寸参数来源见阶段 5 的 `missile-geometry-parameters.md`。

## 完成标准

完成当前用户明确要求的阶段后，简要报告实际工具结果。manual 模式提供下一步可执行的结构化选项；auto 模式根据用户原始目标自动推进。不得把尚未执行的阶段描述为已完成