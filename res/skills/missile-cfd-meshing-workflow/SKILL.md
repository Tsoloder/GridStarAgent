---
name: missile-cfd-meshing-workflow
description: 导弹 CFD 非结构网格生成全流程路由 — CAD 导入、水密性消除、导弹 AI 分部件、碎面合并、导弹几何参数 (D / R / 弦长)、表面网格、各向异性 (可选)、弹翼后缘处理、弓形外场、空间网格、边界条件、质量检查、保存与导出。当用户提到导弹、导弹网格、弹翼、弹头 / 弹体 / 尾部, 各向异性, 后缘处理, 外场, 边界条件, 质量检查, 导出网格时必须使用。
aliases: [导弹网格, 网格生成, 导弹部件分割, 表面网格, 空间网格, 边界条件, 后缘面处理, 各向异性, 弓形外场, 水密性, 继续生成网格]
tags: [导弹, CFD, 网格, 工业软件, 各向异性, 弓形外场]
category: Missile
version: 2.2.0
author: GridStarAgent
allowed-tools: []
---

# 导弹 CFD 非结构网格生成工作流

> 🔴 **零记忆可独立运行（硬性原则）**：本 Skill（含全部 references 与子 Skill）是**唯一运行依据**，**不依赖、不读取、不写入任何记忆层**。缺失知识以**实时 Schema / 现场计算 / 实时探测 / 用户输入**为准，**不得从记忆中补**。开发期资料（实测值、接口就绪状态、`t.py` 需求文件）**不属于运行依据**，即使存在于 `doc/` 也不得指导运行。

本 Skill 只路由、决策、核查，不承载详细参数。工具清单、参数语义、ID 换算规则等见各阶段 reference 对应 §。**未显式列在本 Skill 表格中的工具**（包括 `cad`、`generation` 组）调用前先以实时 Schema 为准。

## 步骤 0：自动识别模型领域

**每次开始网格生成任务前，必须先执行以下领域识别流程：**

1. 调用 `CaptureGridStarWindow` 截取 GridStar 主窗口截图。
2. 通过视觉分析截图中的主视口模型，判断当前加载的模型类型：
   - **飞机模型**：有机翼、机身、尾翼、发动机吊舱等部件特征
   - **导弹模型**：有弹体、弹翼（短而窄）、尾翼（舵面）、无发动机吊舱等特征
3. 根据识别结果调用 `set_active_category` 设置正确的分类：
   - 飞机模型 → `set_active_category(category="aircraft")`
   - 导弹模型 → `set_active_category(category="missile")`
   - 无法识别 → `set_active_category(category="")`（不限制）
4. 设置完成后，如果实际领域与本技能不匹配（例如检测到飞机但当前是导弹技能），应调用 `read_skill` 读取正确领域的技能后重新开始。

**领域识别之外的界面操作纪律**：`CaptureGridStarWindow` 按进程名（默认 `gridstar.exe`）定位窗口，不靠标题，返回的 `result.window` 里带 `is_foreground` / `is_iconic` / `input_blocked` 等诚实字段，先读这些字段再决定下一步。GridStar 是 Qt 程序、不暴露 UI 自动化控件，`GetUIElementInfo` 返回 0 个控件是正常现象，`ClickUIElement` / `TypeTextInUIElement` 在它身上用不了；要看某处、点某处时，用 `ClickAtPoint` / `DragAtPoint` / `ScrollAtPoint` 按截图上的 **0-1000 归一化坐标**操作（相对 `capture_rect`，左上 0、右下 1000）。坐标工具只保证光标落到目标像素，是否被响应必须重新截图确认。工具报「更高权限 / 完整性级别 / UIPI」时不要重试，直接告诉用户需要以与 GridStar 相同的权限运行本服务。

## 🔴 事实中心（先读，全部跨文件硬事实的唯一出处）

**`references/missile-calibration-constants.md`**。含：型号事实（无舵/三角翼）、固定值（D=1196.076、C_root=3225）、编码（chooseParam="-1,0,3"）、补偿公式、幽灵块、通用纪律。**运行中凡涉及这些事实，一律引此文件 #N，禁止复制长注记。**

## 认定流程

**满足导弹部件体系即走导弹流程**：`nose` / `fuselage` / `tail` / `wing` 四类（wing=弹翼，fuselage=弹体；🔴 当前型号无舵、无舵轴，三角翼——事实中心 M1/M2）。中间态（子组归并/旧名混用）按 `references/missile-segmentation.md`「分组归并」统一。**不满足**或是纯飞机/发动机模型 → **停止，提示用户先用飞机流程。**

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema，每个工具用实时 Schema 构造参数。
2. **只读取当前阶段需要的 reference**（见下表），不要预读后续阶段。
3. 🔴 **开工先做「模型是否已就绪」三层查询**（防止"已导入的模型被当成没导入"），按顺序执行，**任何一层命中都不得重复导入**：
   - **第 1 层：查数模是否已在工程** —— `GetAllObjectByType(2)`（数模面）或 `(4)`（超面；都查更稳妥，实时 Schema 为准）：非空 → 跳过 `ImportCADFile`；空 → 才引导导入（`missile-cad-to-mesh.md` §1）。
   - **第 2 层：查工程是否已打开/已有超面** —— `GetAllObjectByType(4)` 或 `GetCurrentSelectedIDs` 复核；非空且第 1 层空 → 数模面已提取为超面，不重复导入。
   - **第 3 层：查分组是否已就绪** —— `GetAllSpitAssemblyGroupProperty`：分组已存在 → 跳过 `ClassifyMissile`；缺失（且数模已在）才需要分割，并**先向用户索取连接参数**（`missile-segmentation.md`）。
   - 🔴 **判定原则**："工程里有没有模型"与"有没有分组"是两回事。模型已在（数模面/超面非空）≠ 需要重新导入；分组缺失只代表"没分割/手动分组"，不代表"没导入"。**严禁仅凭"分组不存在"就退回导入数模**。查询只需各调一次，不得反复重试。

## 流程 A：从 CAD 到网格导出（完整链）

阶段链： CAD 导入与单位设置 → 水密性处理与自由边检查 → 导弹 AI 分部件 (ClassifyMissile) → **分组归并（子 → 主）** → 碎面合并 → 导弹几何参数 → 表面网格生成 → 各向异性 (可选) → 弹翼后缘处理 → 体网格块创建、外场生成 → 空间网格 → 边界条件设置 → 质量检查 → 保存与导出。

| #  | 阶段 / 触发意图 | 读取 |
|---|---|---|
| 1 | CAD 导入、单位设置（🔴 先查数模是否已在工程，非空则跳过导入，见「开始任务前」三层查询） | `missile-cad-to-mesh.md` §1 |
| 2 | 水密性处理、缝隙、自由边检查 | `missile-cad-to-mesh.md` §2 |
| 3 | 导弹分部件、AI 分割、**分组归并（子→主，先记台账）** | `missile-segmentation.md` |
| 4 | 碎面合并（⚠️ 会改超面集合 → 合并后重查分组**更新台账**，事实中心 G2） | `missile-cad-to-mesh.md` §3.1 |
| 5 | 弹径/弦长/球头半径、尺寸测量（固定值 D/C_root 见事实中心 §2，无条件固定） | `missile-geometry-parameters.md` |
| 6 | 导弹表面网格生成 | `missile-surface-mesh.md` |
| 7 | 各向异性（可选）：弹翼前缘・后缘狭长面・梢部・侧面・结合处加密（🔴 无舵、三角翼，仅 wing；台账/子面分组 二者满足其一才做，事实中心 G1） | `missile-anisotropic-mesh.md`（按位置路由：弦向→`missile-chordwise-direction`、展向→`missile-spanwise-direction`、结合处→`missile-root-junction`；后缘→阶段 8） |
| 8 | 弹翼后缘面处理 | 独立 Skill `missile-trailing-edge-processing` |
| 9 | 体网格块创建、外场生成（🔴 不用 `CreateMissileFarField`，统一 `UGBlockCreate`；补偿见事实中心 #6） | `missile-farfield-and-volume.md` §6.1–6.3 |
| 10 | 空间网格 / 体网格 / 附面层（🔴 `UGUGSp` ids 先剔幽灵块，事实中心 #7/§4） | `missile-farfield-and-volume.md` §6.4 |
| 11 | 边界条件、BC、物面、远场、对称 | `missile-farfield-and-volume.md` §7.4 |
| 12 | 网格质量检查、质量报告 | `missile-farfield-and-volume.md` §7.1–7.3 |
| 13 | 保存工程、导出网格、CGNS | `missile-farfield-and-volume.md` §7.5 |

> 各阶段 reference（`missile-*.md`）均位于 `references/` 目录下，表格中省略路径前缀；子 Skill 同理。

## 流程 B：表面网格已完成，继续后续

用户明确表示表面网格已生成时入此流程，跳过 1–6，自动推进 7–13（见流程 A 表，顺序不动）。

## 硬闸门（任何流程都不得违反）

1. **顺序闸门**：表面网格后固定顺序 `各向异性（可选）→ 后缘处理 → 体网格块创建、外场生成 → 空间网格 → 边界条件 → 质量检查 → 保存导出`，严禁跳级；各向异性跳过时须在 note 说明。
2. **水密闸门**：表面网格生成前必须**实际调用 `DealWatertight` 并验证 `free_edge_count` 为 0**。🔴 不得以"分组已存在 / ClassifyMissile 已运行过 / 记忆中有公差值"为由跳过。`free_edge_count > 0` → 公差 ×5 重试，最多 2 次；仍不过则停止，提示用户当前自由边数并建议检查模型几何。自由边全部位于半模线位置时可接受。
3. **边界闸门**：首次铺底统一置 `-10`（无 BC），后续逐组设置。
4. **质量闸门**：面网格除去各向异性单元处最小角 > 10°。
5. **前置条件闸门**：分割结果必须由分组查询验证；台账/子面分组皆缺失时不得调用 `WingAnisoProcessWingAnisotropy` 或后缘面处理工具。
6. **碎面合并闸门**：**每次进入表面网格生成阶段必须先行碎面合并**（调用 `UGSurfaceProcessing`），不得以记忆中出现过为由跳过。
7. **几何闸门**：`GetMissileDimensions` 返回的值**默认直接使用**（任一字段非零即用该值计算对应部件目标尺寸；任一字段为 0 → 该部件按全局默认尺寸生成）。🔴 **固定值例外（事实中心 §2，无条件生效、不以接口为准）**：D=`1196.076`、C_root=`3225` 固定取值；其余 5 项字段仍按本闸门执行。**若接口返回异常但固定值未覆盖该字段 → 原样使用返回值并在 note 记录、报告后端，不自行替换/反推**。

**除此七条外，别无他门。**

## 工具与替代路径

- `ClassifyMissile` 失败 → 走分组查询
- `UGSurfaceProcessing` 失败（每次≤1次换参重试）→ `UGDelRedundantDom`，再失败则 skip+note
- 后缘面处理缺子面 → 台账优先；两者皆缺 → 跳过（事实中心 G1）
- 质量检查无 `CheckMissileMeshQuality` → 通用 Examine 工具
- 体创建失败（`UGBlockCreate` 返回 `"false"` 且报不封闭线清单） → `missile-mesh-sealing-check.md`（排查手册）

## 依赖与可选性

- 部件尺寸依赖几何接口 `GetMissileDimensions`；几何接口依赖产品分割或用户输入。
- 导弹几何量 7 项字段，无 `mac`；各向异性尺寸依赖当地弦长，不依赖 `mac`。
- `GetMissileDimensions` 返回恒定 `"true"` 时向用户索取 D/R/弦长。
- 各向异性可选，用户不提网格数量要求时可跳过，须 note 说明。

## 完成标准

完成当前用户明确要求的阶段后，简要报告实际工具结果。manual 模式提供下一步可执行的结构化选项；auto 模式根据用户原始目标自动推进。不得把尚未执行的阶段描述为已完成。