---
name: missile-cfd-meshing-workflow
description: 导弹 CFD 非结构网格生成全流程路由 — CAD 导入、水密性消除、导弹 AI 分部件、碎面合并、导弹几何参数 (D / R / 弦长)、表面网格、各向异性 (可选)、翼 / 舵后缘处理、弓形外场、空间网格、边界条件、质量检查、保存与导出。当用户提到导弹、导弹网格、弹翼、舵面、弹头 / 弹体 / 尾部, 各向异性, 后缘处理, 外场, 边界条件, 质量检查, 导出网格时必须使用。
aliases: [导弹网格, 网格生成, 导弹部件分割, 表面网格, 空间网格, 边界条件, 后缘面处理, 各向异性, 弓形外场, 水密性, 继续生成网格]
tags: [导弹, CFD, 网格, 工业软件, 各向异性, 弓形外场]
category: CFD
version: 2.0.0
author: GridStarAgent
allowed-tools: []
---

# 导弹 CFD 非结构网格生成工作流

> 🔴 **零记忆可独立运行（硬性原则）**：本 Skill（含全部 references 与子 Skill）是**唯一运行依据**，**不依赖、不读取、不写入任何记忆层**（工作日志 / 项目记忆 / 会话记忆）。所有运行必需知识——工具清单、参数语义、ID 换算、接口判据、失败处置、坑位清单——**必须在本 Skill 内自包含**；缺失时以**实时 Schema / 现场计算 / 实时探测 / 用户输入**为准，**不得从记忆中补**。同理，以"记忆中有×××"为由跳过任何闸门或步骤一律禁止。开发期资料（实测值、接口就绪状态、`t.py` 需求文件）**不属于运行依据**，即使存在于 `doc/` 目录也不得指导运行。

本 Skill 只路由、决策、核查，不承载详细参数。工具清单、参数语义、ID 换算规则等见各阶段 reference 的对应 §。**未显式列在本 Skill 表格中的工具**（包括 `cad`、`generation` 组）在调用前先以实时 Schema 为准。

## 认定流程

**开工即判：满足导弹部件体系即走导弹流程。** 导弹部件体系：`nose` / `fuselage` / `tail` / `wing` / `fin` / `finshaft` 六类，其中 **wing = 弹翼，fin = 舵，fuselage = 弹体**；中间态（子组归并/`rudder*` 等旧名混用）按 `references/missile-segmentation.md`「分组归并」规则统一。**不满足** 或是纯飞机、纯发动机模型 → **停止，提示用户先用飞机流程。**

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema，每个工具用实时 Schema 构造参数。
2. **只读取当前阶段需要的 reference**（见下表），不要预读后续阶段的文档。
3. 🔴 **开工先做「模型是否已就绪」三层查询（新增，防止"已导入的模型被当成没导入"）**，按顺序执行，**任何一层命中都不得重复导入**：
   - **第 1 层：查数模是否已在工程中** —— 调 `GetAllObjectByType(2)`（数模面。注意：`2`=数模面，`4`=超面，二者都查更稳妥；实时 Schema 为准）：
     - 返回**非空**（如 `{"info":[0,5,12]}`）→ **数模已存在**，**跳过 `ImportCADFile`**，向用户说明"检测到工程中已有 N 个数模面，直接继续"。
     - 返回**空** `{"info":[]}` → 才**引导导入**（`ImportCADFile`，见 `references/missile-cad-to-mesh.md` §1）。
   - **第 2 层：查工程是否已打开/已有超面** —— 调 `GetAllObjectByType(4)`（超面）或 `GetCurrentSelectedIDs` 复核：
     - 若非空且第 1 层为空，说明数模面已提取为超面，同样**不重复导入**，直接继续。
   - **第 3 层：查分组是否已就绪** —— 调 `GetAllSpitAssemblyGroupProperty`：
     - **分组已存在 → 直接跳过 ClassifyMissile**；缺失（且确认数模已在工程中）时才需要 `ClassifyMissile`，并**先向用户索取连接参数**（serverHost/serverPort/outputDir，见 `references/missile-segmentation.md`）。
   - 🔴 **判定原则**：**"工程里有没有模型"与"有没有分组"是两回事**。模型已在（数模面/超面非空）≠ 需要重新导入；分组缺失只代表"没分割/手动分组"，不代表"没导入"。**严禁仅凭"分组不存在"就退回导入数模**。查询只需各调一次，不得反复重试。

## ~~ 流程 A：从 CAD 到网格导出（完整链） ~~

阶段链： CAD 导入与单位设置 → 水密性处理与自由边检查 → 导弹 AI 分部件 (ClassifyMissile) → **分组归并（子 → 主）** → 碎面合并 → 导弹几何参数 → 表面网格生成 → 各向异性 (可选) → 翼 / 舵后缘处理 → 体网格块创建、外场生成 → 空间网格 → 边界条件设置 → 质量检查 → 保存与导出。

| #  | 阶段 / 触发意图 | 读取 |
|---|---|---|
| 1 | CAD 导入、单位设置（导入导弹模型）｜🔴 **先查数模是否已在工程（`GetAllObjectByType(2)`/`(4)`），非空则跳过导入**（见「开始任务前」三层查询） | `references/missile-cad-to-mesh.md` §1 |
| 2 | 水密性处理、缝隙、自由边检查 | `references/missile-cad-to-mesh.md` §2 |
| 3 | 导弹分部件、AI 分割、分组归并 → 主组 | `references/missile-segmentation.md` |
| 4 | 碎面合并 | `references/missile-cad-to-mesh.md` §3.1 |
| 5 | 弹径/弦长/球头半径/舵底间隙、尺寸测量 | `references/missile-geometry-parameters.md` |
| 6 | 导弹表面网格生成 | `references/missile-surface-mesh.md` |
| 7 | 各向异性（可选）：翼/舵 前缘・后缘狭长面・梢部・侧面・结合处 加密 | `references/missile-anisotropic-mesh.md`（适用位置、工具、参数总览；按位置路由：弦向→`missile-chordwise-direction`、展向→`missile-spanwise-direction`、结合处→`missile-root-junction`；后缘→阶段 8） |
| 8 | 翼/舵后缘面处理 | 独立 Skill `missile-trailing-edge-processing` |
| 9 | 体网格块创建、外场生成 | `references/missile-farfield-and-volume.md` §6.1–6.3 |
| 10 | 空间网格 / 体网格 / 附面层 | `references/missile-farfield-and-volume.md` §6.4 |
| 11 | 边界条件、BC、物面、远场、对称 | `references/missile-farfield-and-volume.md` §7.4 |
| 12 | 网格质量检查、质量报告 | `references/missile-farfield-and-volume.md` §7.1–7.3 |
| 13 | 保存工程、导出网格、CGNS | `references/missile-farfield-and-volume.md` §7.5 |


## 流程 B：表面网格已完成，继续后续

用户明确表示表面网格已生成时入此流程，跳过 1–6，按以下顺序自动推进：

1. 各向异性 （可选：若用户对网格数量没有要求则跳过）〔流程 A 阶段 7〕
2. 翼 / 舵后缘面处理 〔流程 A 阶段 8〕
3. 体网格块创建、外场生成：按流程 A 表第 9 行执行；🔴 失败报不封闭线 = 转排查。 〔流程 A 阶段 9〕
4. 空间网格 / 体网格 / 附面层 （`references/missile-farfield-and-volume.md` §6.4）〔流程 A 阶段 10〕
5. 边界条件设置 （`references/missile-farfield-and-volume.md` §7.4）〔流程 A 阶段 11〕
6. 质量检查 （`references/missile-farfield-and-volume.md` §7.1–7.3）〔流程 A 阶段 12〕
7. 保存 / 导出 （`references/missile-farfield-and-volume.md` §7.5）〔流程 A 阶段 13〕

## 硬闸门 （任何流程都不得违反）

1. **顺序闸门**： 表面网格生成后固定顺序 `各向异性处理（可选）→ 翼/舵后缘处理 → 体网格块创建、外场生成 → 空间网格 → 边界条件 → 质量检查 → 保存导出`，严禁跳级；各向异性为可选，跳过时须在 note 说明。
2. **水密闸门**： 表面网格生成前必须**实际调用 `DealWatertight` 并验证 `free_edge_count` 为 0**。🔴 **不得以"分组已存在 / ClassifyMissile 已运行过 / 记忆中有公差值"为由跳过**（分组存在 ≠ 水密性已完成，分组可来自手动导入或历史工程残留；过往内存中的公差值是参考值，不代表当前模型已水密）。若返回 `free_edge_count > 0`，公差加大 5 倍重试，最多 2 次；仍不通过则停止迭代，提示用户当前自由边数并建议检查模型几何。自由边全部位于半模线位置时可接受。
3. **边界闸门**： 首次铺底必须统一置 `-10`（无 BC），后续逐组设置。
4. **质量闸门**： 面网格除去各向异性单元处最小角 > 10°。
5. **前置条件闸门**： 分割结果必须由分组查询验证；分组缺失时不得调用 `WingAnisoProcessWingAnisotropy` 或后缘面处理工具。
6. **碎面合并闸门**： **每次进入表面网格生成阶段必须先行碎面合并**（调用 `UGSurfaceProcessing`），不得以记忆中出现过为由跳过。
7. **几何闸门**： `GetMissileDimensions` 返回的值**直接使用，不做逻辑检查、不对比、不怀疑**。任一字段非零即用该值计算对应部件的目标尺寸。任一字段为 `0` 时该部件按全局默认尺寸生成（不再分析是否为"不一致"、不额外向用户确认、不记 note 比对分析）。Agent 不得替换、换算、反推任何几何量。

**除此七条外，别无他门。**

## 工具与替代路径

- `ClassifyMissile` 失败 → 走分组查询
- `UGSurfaceProcessing` 失败 → `UGDelRedundantDom`，再失败则 skip+note
- 后缘面处理缺失子组 → 几何反推取回
- 质量检查无 `CheckMissileMeshQuality` → 通用 Examine 工具
- 体创建失败（`UGBlockCreate` 返回 `"false"` 且报不封闭线清单） → `missile-mesh-sealing-check.md`（排查手册）

## 依赖与可选性

- 部件尺寸依赖几何接口 `GetMissileDimensions`；几何接口依赖产品分割（`ClassifyMissile`）或用户输入。
- 导弹几何量为 10 项字段，无 `mac`；各向异性尺寸依赖当地弦长，不依赖 `mac`。
- `GetMissileDimensions` 返回恒定 `"true"` 时向用户索取 D/R/ 弦长。
- 各向异性为可选步骤，用户不提网格数量要求时可跳过，但必须在 note 说明。

## 完成标准

完成当前用户明确要求的阶段后，简要报告实际工具结果。manual 模式提供下一步可执行的结构化选项；auto 模式根据用户原始目标自动推进。不得把尚未执行的阶段描述为已完成。