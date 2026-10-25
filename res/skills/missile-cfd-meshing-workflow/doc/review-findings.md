# 复盘教训归档（开发期资料）

> 🔴 **本文件属于 `doc/`（开发期资料），不是运行依据**。运行 Phase 不得读取本文件指导执行——运行只认 `references/` 与各子 Skill 的 SKILL.md（见主 SKILL「零记忆可独立运行」硬性原则）。
> 用途：集中存放 2026-10-05 ~ 2026-10-09 三轮实跑复盘出的"为什么/历史教训/失败案例"，供人阅读、后续维护参考。**运行文档中的规则均以"做什么 + 怎么执行"表述，如想知道某规则的来由，查本节对应条目。**

## 1. ~~为什么"分组归并"必须做~~ —— 🔴 已作废（2026-10-12）

> ❌ **本条已失效，切勿据此执行**。原教训建立在"AI 分割产物是 `wingLeadingEdge` 等无序号子组、需归并到 `wing` 主组"这一前提上，而后端架构已变更：**只产出带序号子组名**（`wingLeadingEdgeN` / `wingSideSurfaceN` / `wingTrailingEdgeN` / `wingTipN`），**不存在 `wing` 主组，也没有归并步骤**（见 `missile-segmentation.md`「子面分组即定位键」、主 SKILL「认定流程」）。
> 仍然有效的部分：**三层查询判定不能过松**——"分组已存在"与"分组内容正确"是两回事，仍需按组名验证带序号子组是否存在。运行规则见 `missile-segmentation.md`。

## 2. 为什么弓形外场"四值全等 = 外场大小"（2026-10-09 复盘）

- 现象：Agent 从默认值 `"20320.4,20320.4,20320.4,20320.4"`（≈4×全弹长）等比反推 1×弹长，能推对但浪费时间。
- 教训：此类能直接写死的推断不要留给 Agent 现场做。
- 运行规则见 `missile-farfield-and-volume.md` §6.3。
- ⚠️ 倍数规则后来多次改定（最终 1.2× 弹体长度）——本条留作"为什么写死"的教训，倍数以 `missile-farfield-and-volume.md` §6.2 为准。

## 3. 为什么结合处"禁止全量取平均"（2026-10-09 复盘，38 边技术债）

- 现象：结合处 38 条共享边逐条读原值不可行 → 上轮全量取平均 ≈10.36/11.04，最小 0.14 的短过渡边被改成 ~11（放大 79 倍），疑似面网格最小角 0.0057° 帮凶。
- 教训：物理分组不能平均抹平；用三分组聚类（<1→0.3 / 1~10→5 / >10→15）。
- 运行规则见 `missile-root-junction/SKILL.md`「聚合近似规则」。

## 4. 为什么 groupProperty 要改 domain **和 line** 两类尺寸（2026-10-09 复盘 + 🔴 2026-10-19 修正）

> 🔴 **2026-10-19 重要修正**：本条原写作"只需改 3 个字段（`targetSize`/`minSize`/`angle`），`line` 一律不动"。**该结论是错的，且已造成三次实机卡死**——`line[].targetSize`/`minSize` 返回时恒为 0 且 `line[].ids` 非空，保留 0 会让表面网格生成**永久挂死**（工具永不返回，只能用户手动中断）。正确规则：**`domain[]` 与 `line[]` 的尺寸都要改**，且两者取同一组计算值。下文"现象/教训"部分保留为问题溯源记录。

- 现象（原始问题）：Agent 手工重建整个 groupProperty JSON（7 组 × domain/line），同份长 JSON 在 3-4 段思考里反复完整粘贴，且凭记忆重建 ids 无法保证一致。
- 现象（2026-10-19 新增）：Agent 严格遵守"`line` 不动"后，`line` 的 0 被后端写入网格线尺寸 → 生成阶段死循环。实跑三次卡死（2026-10-18 ×2、2026-10-19 ×1，**跨不同导弹型号**），共同点均为 `line[].targetSize=0 / minSize=0 且 ids 非空`；对照实验：**同型号、同分组 id 的两次调用，line 填了正确尺寸的那次秒过**。参照飞机侧 `cfd-meshing-workflow/references/part-based-surface-mesh.md` §4.2「domain 和 line 均更新」——飞机侧从未出现此问题。
- 教训（仍然有效）：原始返回值先存档 → 在副本上改字段 → 回传存档副本；禁止重建整个 JSON。**但"改哪些字段"= `domain[]` 与 `line[]` 的 `targetSize`/`minSize`/`angle` 共 6 处，`ids`/组名/键名仍不动。**
- 运行规则见 `missile-surface-mesh.md`「groupProperty 修改纪律」。

## 5. 为什么碎面合并要"唯一决策流"（2026-10-09 复盘，文档自相矛盾）

- 现象：`cad-to-mesh.md` §3.1 把 `UGDamageRepari` 列为首选，但下方替代路径写"改走 `UGDelRedundantDom`"——Agent 花 3 大段自我论证该不该试，最终试了也 false。
- 教训：唯一决策流 = `UGSurfaceProcessing`(换参 1 次) → `UGDelRedundantDom`(1 次) → `UGDamageRepari`(1 次) → 跳过+note，最多 3 次调用。
- 运行规则见 `missile-cad-to-mesh.md` §3.1。

## 6. 为什么 caliperFirst 要"公式 + 经验值表"（2026-10-09 复盘，4 次改值）

- 现象：同一步 Agent 先猜 0.01 → Schlichting 公式推 0.00967 → 改 0.034 → 回 0.01；思考流 1/3 篇幅耗在这。根因是文档只有参数表、没有公式和量级参考。
- 教训：`y+·μ/(ρ·U∞·√(Cf/2))`，算完 ×1000 转 mm；单位陷阱（m 当 mm 差 1000 倍）。
- 运行规则见 `missile-farfield-and-volume.md` §6.4。

## 7. 为什么 UGUGSp 的 ids 必须先剔幽灵块（2026-10-09 实机）

- 现象：`DeleteBlock(1)` 后 `GetAllObjectByType(7)` 仍返回 `[1,2]`；未过滤时 `UGUGSp` 连续 false。且曾误用 `GetConnectorsByDomains` 验证块 ID（面→线查询不接受体块 ID，会把合法块全剔除、ids 传空）。
- 运行规则见 `missile-calibration-constants.md` §4。

## 8. 为什么"建场即试金石"而非逐面统计（2026-09-29 实机教训）

- 现象：预先全量统计（`GetAllObjectByType(6)` + `GetConnectorsByDomains` 全列表 + 引用计数）在 341 线规模耗时 5 分钟+；且幽灵 ID 会被误判成"不封闭线"卡住自己。
- 教训：封闭性最终判定统一由 `UGBlockCreate` 返回真值 + 后端报错清单完成。
- 运行规则见 `missile-mesh-sealing-check.md` §6.0。

## 9. 为什么固定值"无条件生效、不报告确认"（2026-10-09 二次实跑波动）

> 🔴 **§9 全节已于 2026-10-14 作废**（用户裁定接口返回值可用，固定值取消）。保留作为**问题溯源记录**。⚠️ **真因修正（重要）**：本节的"波动/偏小"**并非**后端缺 m→mm 单位换算——缺换算应是 1000 倍差，而实测只差约 9 倍；真因是**弹径交接站位落在锥段上的分组假象**（`body_diameter` 由头锥/弹体交接站位反解，分组缺失或过渡段被切碎时会量到锥段半径），精度更稳的弹尾口径 `body_diameter_tail` 实测 ≈1197.39，与定标值 1196.076 mm 吻合到 0.1%，**量纲本就是 mm**。故后端**不做也不应做**单位换算（曾误加一次 ×1000，读数恰大 1000 倍属二次换算，已移除）；表面网格非球头口径改由 `body_diameter_surface`（失配时回退弹尾口径）处理。运行规则现见 `missile-calibration-constants.md` §2（接口唯一权威）。

- 现象：同一 SPD 工程 `GetMissileDimensions` 返回值波动（`body_diameter` 137.39 / `nose_radius` 3.29 / `wing_root_chord` 3135.12 / `wing_tip_chord` 599.15 / `wing_half_span` 1200.68），与已裁定固定值冲突；Agent 反复权衡是否该改固定值，纯耗轮次。
- 最新实测（2026-10-12 再跑）：`body_diameter` 133.008 / `wing_root_chord` 3190.307 / `wing_half_span` 1244.821 / `wing_tip_chord` 578.931 / `nose_radius` 3.320 / `fuselage_length` 5080.092（均 mm，原值 ×1000）。**仍是另一组与固定值不符的值** —— 后端未修，「固定值写死」策略继续生效；`fuselage_length` 5080.092 与文档 1×弹长 ≈5079.986 一致，属可信字段。
- 用户终审（2026-10-09）："不用管，就是这几个值" —— D=1196.076 / C_root=3225 / 半翼展=1655.038 / C_tip≈0 钳制规则为最终准值；接口波动不怀疑、不请求图纸复核，至多 note 一次。
- 🔴 **后续裁定（2026-10-14，取代上条）**：D=1196.076 / C_root=3225 / 半翼展=1655.038 **全部作废**，改为"后面都用接口返回值"。⚠️ **真因修正**：旧实测偏小约 9 倍**不是**单位换算缺失（那会是 1000 倍），而是弹径交接站位落在锥段上的分组假象；坐标系本就是 mm。运行规则见 `missile-calibration-constants.md` §2。
- 🔴 **再后续（2026-10-16）**：弹翼尺寸改为**按对**——接口新增 `wings[]` 数组（下标 = 对数−1），第 N 对翼取 `wings[N−1]`，旧标量键 `wing_root_chord` 等仅 = `wings[0]`。运行规则见 `missile-calibration-constants.md` §2.1。

## 10. 为什么查询要"一次批量拿全"（2026-10-09 二次实跑教训）

- 现象：`GetConnectorsByDomains` 等只读查询被写成"对分组内每个面逐个调"（结合处 38 边逐条、各向异性 N+M 次循环）——大量冗余轮次。
- 教训：一次传多个 domain_ids 批量返回 = 逐条 N 次的 1/N。
- 运行规则见主 SKILL「上下文治理纪律」第 4 条。

## 11. 为什么规划类工具要一次传全参数（2026-10-09 二次实跑教训）

- 现象：`update_plan` 缺 `title` 返回 false 并整体重试一整轮。
- 运行规则见主 SKILL「上下文治理纪律」第 6 条。

## 12. 为什么移除体网格质量检查（2026-10-09 首跑教训 → 2026-10-15 裁定删除）

- 现象（首跑记录，⚠️ **症状描述有误**）：`ExamineBlock(ExamineMaximumIncludeAngle)` 对几十万四面体块执行时被判为"后端进程崩溃（`ConnectionResetError(10054)`）"。
- 🔴 **真因修正（2026-10-15 全量日志核对）**：实际症状**不是崩溃报错，而是后端无响应、调用永久挂死**——既不返回 `"false"`，也不抛异常，只能由用户手动停止本轮对话。该工具在全部会话中共被调用 **4 次**（2026-10-10 / 10-12 / 10-13 ×2），**4 次全部卡住**（3 次用户手动停止、1 次后端断连 `RemoteDisconnected`），**没有一次拿到结果**。工具层 `send_post_request` 默认不限时（`agent/client.py` 的 `timeout=None`），后端算不出来就无限等待。
- 次因：原先写的"崩溃 / 重试同错"防御**与实机表现不符**，因此从未被触发，Agent 每次都照样发起调用、照样卡住；同时 `CheckMissileMeshQuality` 这个"一站式质量检查"后端恒返回 `"true"`（兜底值、无数据），也属「返回恒定值 → 不可用」，却被提示词列为质量检查首选，属多余的绕行步骤。
- 裁定（2026-10-15，用户）：🔴 **导弹流程去掉体网格检查，只查面网格最小角**（`ExamineDomain(examType="MinmumAngle")` > 10°）；`ExamineBlock` 与 `CheckMissileMeshQuality` 均**不得调用**。
- 运行规则见 `missile-farfield-and-volume.md` §7.1（§7.1.1 面网格唯一路径、§7.1.2 体网格为何不执行）。


## 14. ~~为什么后缘 false 要先二确认~~ —— 🔴 已作废（2026-10-13）

> ❌ **本条已失效**：其载体（`missile-trailing-edge-processing` 后缘装配流程：`MergeEdgesByDomain` / `AssembleConnectorsToDomain` / `DeleteDomain`）已整体废止——需求文档未要求狭长面处理、也未要求独立后缘装配阶段，弹翼后缘面改由各向异性阶段一并做 `generateType=2`（与前缘同款，三角翼/梯形翼不分叉）。
> 仍然有效的部分（已并入运行规则）：**false 语义歧义**的教训 —— 工具返回 `false` 时要先做"二确认"（查实际状态）再判定，不得一次 false 就整体跳过。运行规则见主 SKILL「工具可用性判定」。