# 类型一：梢部相邻后缘面处理

类型一后缘面与梢部面（`wingTip`）相邻，由 4 条网格线（2 条长边 L1/L2、2 条短边 S1/S2）组成。🔴 当前型号无舵（2026-10-08 确认）：仅弹翼（wing）有后缘处理，无 `finTrailingEdge`。

> **弹翼梢部说明（按 需求文件 1.4）**：翼梢面 `wingTip` 只存在于梯形翼，三角翼没有。🔴 **当前型号为三角翼（2026-10-09 确认），`wingTip` 缺失属正常**——后缘面（`wingTrailingEdge`）的梢部端以翼侧面（`wingSideSurface`）的自由梢边为相邻对象，下文所有 `wingTip` 引用在三角翼上以该自由梢边代替；不要因 `wingTip` 缺失误判"分组不全"。

> ⚠️ **必须按步骤 1→8 顺序执行，禁止跳过任何步骤。**（仅当下述「可用性前置检查」判定接口可用时适用；若判定当前后端未实现需跳过/回退，按检查结论执行，不受此约束。）

> ⚠️ **全局注意事项**
>
> - `CopyConnectorPointCount` 的 `sourceId` 是点数来源（拷贝**源**），`targetIds` 是接收点数的目标组，切勿搞反。
> - 工具返回 `success` 为 `false` 时**立即停止**，报告失败。
> - 交线判定失败（后缘面与梢部面无公共短边）时立即停止。
> - `SetConnectorPointCount`、`SetConnectorAverageDistribution`、`SetConnectorSmoothDistribution`、`CopyConnectorPointCount` 返回格式为 `{"success":true}`，**操作后网格线 ID 不变**，无需追踪新 ID。
> - manual 模式下，中间步骤直接执行，**不输出 `tool_params`**。仅 `DeleteDomain` 通过 `options` 请求确认；auto 模式下 `DeleteDomain` 直接执行，不请求确认。

**间距参数**（导弹版）：
- `bodySpacing` = 0.008 × 当地弦长（靠近梢部端）
- `rootSpacing` = 0.02 × 当地弦长（靠近根部端）
- `params` = `"1.2,10,1.2,10"`

> ⚠️ **当地弦长的来源只认 `GetMissileDimensions`（弹翼 `wing_root_chord`/`wing_tip_chord`）或用户直接输入**。**禁止**用分组属性 `targetSize` 反推。该接口不可用时向用户索取；拿不到 → 本阶段 `skipped` + `note`。

---

## 步骤

> ⚠️ **可用性前置检查**（按主 Skill `missile-cfd-meshing-workflow` 的「工具可用性判定」通用规则）：
> - **定位源**：`wingTrailingEdge` 面 ID 从**台账（优先）或子面分组**取（见 `SKILL.md` 前置条件 1）；两者都缺失 → 整体跳过。
> - **后缘面有效面 ≤1**（剔除 0 后仅 1 个面，如 `wingTrailingEdge=[0,18]` → 仅 `18`）→ **整体跳过**，无需执行下方步骤，进入体网格块创建（`note` 记录原因）。
> - **需完整处理时** → 执行下方 8 步。其中步骤 2 / 4 依赖 `GetConnectorsByDomains` 与 `DetermineWingTEDirection`：**任一返回兜底值（恒 `"true"` / 空）时**，该步按步内说明的替代方式处理，**勿重试、勿换参**。
> - 🔴 **步骤顺序：先装配（步骤 7）→ 后删除（步骤 8）**：删除必须晚于装配成功，失败时保留原后缘面。

1. **合并边**：调用 `MergeEdgesByDomain`（当前后缘面 ID），得到 `longids`（L1、L2）和 `shortids`（S1、S2）。
   - 若返回 `"false"` 或缺少 `longids`/`shortids` → 前置状态不满足（未网格化/无后缘面）→ **标记本阶段 `skipped` + `note`，勿重试**。

2. **验证交线位置**：后缘面与梢部面的公共线（交线）必须在短边中。
   - 调用 `GetConnectorsByDomains`（后缘面 ID）和 `GetConnectorsByDomains`（梢部面 ID），取交集。
   - 交线必须等于 S1 或 S2，否则 → 失败停止。
   - ⚠️ 若 `GetConnectorsByDomains` 返回兜底值（恒 `"true"`，不可用）：**该步跳过验证**，直接进入下一步（勿重试、勿换参数名）。

3. **短边设点数 + 平均分布**（S1、S2 各一遍），点数固定为 5：
   - 先调用 `GetPointCount`（当前短边 ID）获取当前点数。
   - 若点数 != 5：
     - `SetConnectorPointCount`（当前短边 ID, 5）
     - `SetConnectorAverageDistribution`（当前短边 ID）
   - 若点数 == 5：跳过操作。
   - **S2 同理**（仍使用 S2 的原始 ID）。

4. **判定 L1、L2 的方向**：
   - 调用 `DetermineWingTEDirection`（后缘面 ID、梢部面 ID、longids），得到 `intersection_connector_id` 和 `longEdge_tip_end`（两条长边各自靠近梢部的端点）。
   - ⚠️ 若 `DetermineWingTEDirection` 返回兜底值（不可用）：跳过该工具，按几何常识判定——梢部端 = 靠近翼梢（`wingTip` 一侧）的端点，另一侧为根部端。
   - `longEdge_tip_end[0]` = `"start"` → L1 的 start 端靠近梢部（headspace=`bodySpacing`, tailspace=`rootSpacing`）
   - `longEdge_tip_end[0]` = `"end"` → L1 的 end 端靠近梢部（headspace=`rootSpacing`, tailspace=`bodySpacing`）
   - L2 同理（`longEdge_tip_end[1]`）。

5. **长边 L1 平滑分布**：
   - `SetConnectorSmoothDistribution`（L1, headspace, tailspace, params, rootSpacing）

6. **长边 L2 设点数 + 平滑分布**：
   - `CopyConnectorPointCount`（L1, L2）
   - `SetConnectorSmoothDistribution`（L2, headspace, tailspace, params, rootSpacing）

7. **装配结构网格面**：`AssembleConnectorsToDomain`（L1, L2, S1, S2，逗号分隔，使用原始 ID，因操作后 ID 不变）。
   - 🔴 **必须先完成本步，才能执行步骤 8 的删除**。
   - 若返回 `"false"`：**不要反复换线序**——线序需端点坐标校验，而 `GetStartAndEndPointByConnectors` 恒定 `"true"`（不可用），无法验证 → **立即停止，保留后缘面，标记阶段 `skipped` + `note`**。

8. **删除原后缘面**（仅在步骤 7 装配成功后执行）：`DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）
   - 🔴 **严禁"先删后装"**：装配失败或未执行时**不得删除**。若按「先删后装」顺序执行，装配失败会导致**后缘面永久丢失**（模型少一个面且无替代）。
