---
name: missile-cfd-meshing-workflow
description: 导弹 CFD 非结构网格生成全流程路由：CAD 导入、水密性处理、导弹 5 类 AI 分割、碎面合并、导弹几何参数(弹径/弦长/球头半径)、表面网格、各向异性、鳍后缘处理、弓形外场、空间网格、边界条件、质量检查、保存与导出。当用户提到导弹、导弹网格、弹翼、舵面、鳍、导弹导入模型、导弹部件分割、导弹表面/体/空间网格、导弹各向异性、导弹后缘、导弹外场、导弹边界条件、导弹质量检查、导弹导出网格时必须使用。
aliases: [导弹CFD, 导弹网格, 导弹网格生成, 导弹导入模型, 导弹部件分割, 导弹表面网格, 导弹空间网格, 导弹边界条件, 导弹质量检查, 导弹后缘面处理, 导弹各向异性, 弹径, 球头半径, 导弹水密性, 导弹继续生成网格, missile meshing]
tags: [导弹, missile, CFD, CAD, 网格, 工业软件, 质量检查, 边界条件, 弹径, 各向异性, 附面层, 球头, 弓形外场]
category: CFD
version: 1.0.0
author: Tsolodancer
allowed-tools: []
---

# 导弹 CFD 非结构网格生成工作流

[新增 Skill] 导弹专用 CFD 网格生成全流程路由。使用导弹专用工具函数和参数体系。公式参数以 `t.py` 知识库为权威源。

本 Skill 只决定"当前任务走哪条业务路径、需要哪类对象、按什么顺序做、读哪份 reference"。工具审批、对象范围解析、安全确认、错误重试以及 `options`、`tool_params`、`workflow` 的格式，始终遵守基础 system prompt。

## 开始任务前

1. 从实时 MCP 工具列表识别可用工具和 Schema，不凭本文示例虚构工具；每个工具用实时 Schema 构造参数。
2. 按下面的路由表选流程。**用户同时提到"表面网格已生成/已完成/已有"和"空间网格/体网格/继续生成"时，一律走流程 B**。
3. 读取对应 reference 后再规划步骤，不要凭记忆补参数。
4. auto 模式调用内置工具 `update_plan` 维护阶段计划：每次传入**完整**计划（全量替换语义）、复用同一计划 `id`、在阶段 `note` 中记录关键事实。计划状态只能由实际返回结果更新。
5. 只收集当前阶段缺少的信息，避免一次询问大量后续参数。
6. 领域参考值只能作为建议；导入、网格生成、对象或属性查询失败时，不继续依赖它的后续步骤。

## 流程 A：从 CAD 到网格导出（完整链）

阶段链：CAD 导入与单位设置 → 水密性处理与自由边检查 → 导弹 5 类 AI 分割 → 碎面合并 → 导弹几何参数计算 → 表面网格生成 → 各向异性网格处理（可选）→ 翼/舵后缘面处理 → 弓形外场创建与体网格块创建 → 空间网格生成 → 边界条件设置 → 网格质量检查 → 保存工程与网格导出。

**表面网格生成之后的顺序固定为：各向异性处理（可选）→ 翼/舵后缘面处理 → 体网格块创建（`UGBlockCreate` 或 `CreateMissileFarField`）→ 空间网格生成。** 各向异性处理与后缘面处理是否执行，由导弹 5 类 AI 分割结果是否包含对应前置分组决定（见"硬闸门"第 5 条）。

| # | 阶段 / 触发意图 | 读取 |
|---|---|---|
| 1 | CAD 导入、单位设置（导入导弹模型、打开导弹 CAD、开始新项目） | `references/missile-cad-to-mesh.md` §1 |
| 2 | 水密性处理、缝隙、自由边检查 | `references/missile-cad-to-mesh.md` §2 |
| 3 | 导弹分部件、导弹 AI 分割、识别弹头/弹体/弹翼/舵/尾部 | `references/missile-segmentation.md` |
| 4 | 碎面、合并面、合并碎面 | `references/missile-cad-to-mesh.md` §3.1 |
| 5 | 弹径、翼根/翼梢弦长、球头半径、舵底间隙、特征长度、尺寸测量 | `references/missile-geometry-parameters.md` |
| 6 | 导弹表面网格、面网格生成 | `references/missile-surface-mesh.md`；已按部件分组时用 `references/missile-cad-to-mesh.md` §4 |
| 7 | 导弹各向异性、网格线分布、翼前缘/后缘/翼梢加密（可选） | `references/missile-anisotropic-mesh.md` |
| 8 | 翼/舵后缘面处理、后缘网格、trailing edge（翼和舵各处理一次） | 独立 Skill `fin-trailing-edge-processing`（入口说明见 `references/missile-anisotropic-mesh.md`） |
| 9 | 弓形外场、体网格块创建、半模边界线 | `references/missile-cad-to-mesh.md` §6.1–§6.3 |
| 10 | 空间网格、体网格、附面层 | `references/missile-cad-to-mesh.md` §6.4 |
| 11 | 边界条件、BC、物面、远场、对称 | `references/missile-cad-to-mesh.md` §7.4 |
| 12 | 网格质量检查、面网格/体网格质量报告 | 首选 `CheckMissileMeshQuality`（一站式），失败时回退到通用工具 `ExamineDomain(examType="MinmumAngle")` + `ExamineBlock(examType="ExamineMaximumIncludeAngle")`；详见 `references/missile-cad-to-mesh.md` §7.1–§7.2 |
| 13 | 保存工程、导出网格、CGNS | `references/missile-cad-to-mesh.md` §7.5 |

补充入口（不占阶段，可与上表任意阶段并行）：

- 参数速查：导弹部件网格参数表、各向异性尺寸、空间网格参数、外场规则 → `references/missile-surface-mesh.md`、`references/missile-anisotropic-mesh.md`。

## 流程 B：表面网格已完成，继续后续（优先走此流程）

用户明确表示表面网格已生成时进入此流程，跳过流程 A 的阶段 1–6，从各向异性处理开始。阶段 3 的导弹 5 类 AI 分割结果仍是本流程的前置条件（分组已存在），分组缺失时先补齐分组或向用户说明。

**各向异性网格处理（前置条件满足时）→ 翼/舵后缘面处理 → 弓形外场创建与体网格块创建 → 空间网格生成 → 边界条件设置 → 网格质量检查**

1. **各向异性网格处理**：先用 `GetAllSpitAssemblyGroupProperty` 确认弹翼/舵子组存在（`finLeadingEdge`、`finTrailingEdge`、`finTip`、`finSideSurface` 及对应的 `rudder*` 分组）。存在时按流程 A 表第 7 行执行各向异性处理；不存在时将该阶段标记 `skipped` 并在 `note` 中注明原因，再进入后缘面处理。用户对网格数量没有要求时可一并跳过。
2. **翼/舵后缘面处理**：用 `GetSpliteAssemlyDomainsBatch` 获取后缘面、梢部面、结合部件分组的网格面 ID → 先处理弹翼(fin)分组，再处理舵(rudder)分组。读取独立 Skill `fin-trailing-edge-processing` 的步骤资料（调 `read_skill_resource` 时 skill 参数填 `fin-trailing-edge-processing`，路径填 `references/type1.md`），严格按对应类型步骤逐个处理。**导弹仅存在梢部相邻型后缘面（2–4 条边）。**
3. **弓形外场创建**：按流程 A 表第 9 行执行；半模场景先调 `UGHalfModelLine` 设置半模边界线，外场不存在时调 `CreateMissileFarField` 创建弓形外场（1×弹体长度，头部距离 1.5×头部半径）。
4. **空间网格生成**：按流程 A 表第 10 行执行。
5. **边界条件设置**：按流程 A 表第 11 行执行。
6. **网格质量检查**：按流程 A 表第 12 行执行，可调 `CheckMissileMeshQuality` 做综合检查。

## 硬闸门（任何流程都不得违反）

1. **顺序闸门**：表面网格生成成功后必须按 翼/舵后缘面处理 → 体网格块创建（`UGBlockCreate` 或 `CreateMissileFarField`）→ 空间网格生成（`UGUGSp`）执行，严禁跳过后缘面处理或体网格块创建直接调用 `UGUGSp`。体网格块创建完成后，manual 模式用 `options` 询问是否创建附面层/设定外场，auto 模式按用户原始目标判断。
2. **水密闸门**：表面网格生成前必须完成水密性处理（自由边检查通过）和导弹 5 类 AI 分割。处理失败时公差加大 5 倍重试，最多 2 次（共 3 次调用）；若仍未处理任何自由边则停止迭代，提示用户模型可能存在缝隙、穿插、孔洞等错误。自由边全部位于半模线位置时可接受。
3. **边界闸门**：首次铺底时 `BorderConditionSaveDataToDomain` 的 `property` **必须统一为 `-10`（无边界条件）**，严禁在挂载阶段写入实际属性值。实际属性（物面→粘性固壁、外场→远场、对称→对称等）只能在铺底完成后、按用户明确要求逐组设置。物面可按导弹分部件拆成多个物面组（弹头、弹体、弹翼、舵、尾部）。
4. **质量闸门**：面网格除去各向异性单元处最小角应大于 10°；体网格最大角不大于 178° 为合格，严格禁止存在 179.9° 单元。
5. **前置条件闸门**：导弹 5 类 AI 分割完成后必须检查分割结果中的分组。存在弹翼/舵子组（`finLeadingEdge`、`finTrailingEdge`、`finTip`、`finSideSurface` 及对应的 `rudder*` 分组）时，表面网格生成后执行各向异性处理与后缘面处理；分组缺失时不得调用后缘面处理工具（`DetermineFinTEDirection` 等），须先补齐分组，无法补齐时向用户说明并在阶段 `note` 中记录跳过原因。

## 依赖与可选性

- 导弹部件网格参数表和各向异性尺寸均依赖弹径 D 和球头半径 R；D 依赖头部锥体与弹体交接处直径或弹尾直径。用户直接提供这些参数时跳过 `GetMissileModelParameters` 查询步骤。
- `GetMissileModelParameters` 从 NNW 接口获取导弹关键参数；若返回参数无效（D==0.0 等），停止后续步骤并告知用户。
- 导弹 5 类 AI 分割结果包含弹翼/舵子组（`finLeadingEdge`、`finTrailingEdge`、`finTip`、`finSideSurface` 及对应的 `rudder*` 分组）时，各向异性处理与后缘面处理是表面网格生成后的必经阶段，不得以"用户未提网格数量要求"为由跳过；仅当分组缺失而无法补齐时才跳过，并在阶段 `note` 中说明原因。
- 导弹标准划分（5 类 AI 分割）：nose(弹头) / body(弹体) / fin(弹翼) / rudder(舵) / tail(尾部)；模型含外场时把外场单独设为一个部件。
- `fin`（弹翼）组按 t.py 1.4 拆分为：`finLeadingEdge` / `finTrailingEdge` / `finTip` / `finSideSurface`
- `rudder`（舵）组按 t.py 1.4 拆分为：`rudderLeadingEdge` / `rudderTrailingEdge` / `rudderTop` / `rudderSideSurface` / `rudderRoot`
- 头部子类型：球头（半径较小球面）vs 尖锐头部（无明显边界），两种需区别处理。
- 舵与翼区分：翼与弹体无缝连接；舵为控制部件，底部与弹体有间隙。

## 完成标准

完成当前用户明确要求的阶段后，简要报告实际工具结果。manual 模式提供下一步可执行的结构化选项；auto 模式根据用户原始目标自动推进，删除、覆盖、导出等不可逆操作同样直接执行，不使用 `options` 请求确认。不得把尚未执行的阶段描述为已完成。
