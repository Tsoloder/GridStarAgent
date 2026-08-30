# 飞行器后缘面处理

用户提及"后缘面处理"、"后缘网格"、"trailing edge"时进入此流程。该流程对飞行器机翼后缘分部件网格面进行碎边合并与结构网格面装配。

> ⚠️ **全局注意事项**
>
> - `CopyConnectorPointCount` 的 `sourceId` 是点数来源（拷贝**源**），`targetIds` 是接收点数的目标组，切勿搞反。
> - **`SetConnectorPointCount`、`SetConnectorAverageDistribution`、`SetConnectorSmoothDistribution`、`CopyConnectorPointCount` 这四个工具**的返回格式为：
>   `{"success":true,"new_id":136}`（成功）或 `{"success":false,"new_id":-1}`（失败）。
>   `new_id` 即为该操作创建的新网格线 ID，**后续步骤直接使用 `new_id`，无需额外调用 `GetNewConnectorId`**。
> - 所有工具返回 `success` 为 `false`，或返回 `status` 为 `"failed"` 时，均**立即停止**，报告失败的工具名称与返回内容。**禁止**在工具失败后改用其他工具或换一种方法重试。
> - 自动模式下不允许向用户发起确认性提问。
> - manual 模式下，后缘面处理流程的中间步骤（`SetConnectorPointCount`、`SetConnectorAverageDistribution`、`SetConnectorSmoothDistribution`、`CopyConnectorPointCount`、`AssembleConnectorsToDomain` 等）直接按步骤串行执行，**不输出 `tool_params`**。仅 `DeleteDomain` 作为破坏性操作时，通过 `options` 请求确认。流程开始时输出 `phase_plan` 展示进度。
> - 交线判定失败（类型一中后缘面与翼梢面无公共短边）时立即停止，报告失败原因。

## 前置条件

1. 调用`GetAllSpitAssemblyGroupProperty`,已存在 `jiyi_trailing_edge`、`jiyi_wing_tip`、`engine`、`fuselage` 分组。
2. 通过`GetModelParameters`,返回值中的参数均为有效值
3. 通过`GetAllObjectByType`(6),返回值中有网格面
4. 上述任一条件不满足时停止。

## 默认参数

- 半展长：586.10
- 当地弦长：144.74

## 间距设置

| 位置 | 间距 |
| --- | --- |
| 类型一靠近翼梢端、类型二靠近机身边 | `bodySpacing` = 0.5861（0.1% × 586.10） |
| 有共点的网格线侧（翼根侧）、中间值（mindValue） | `rootSpacing` = 2.8948（2% × 144.74） |
| 分布参数 params | `"1.2,10,1.2,10"`（增长率默认 1.2） |

## 执行步骤

**处理顺序：先处理所有类型一的后缘面，再处理所有类型二的后缘面。** 每个后缘面独立判定类型并处理。

### 第0步：类型判定

1. 调用 `ClassifyTrailingEdgeDomains`，得到：
   - 所有分组 ID 列表（`te_domains`、`wing_tip_domains`、`engine_domains`、`fuselage_domains`）
   - 每个后缘面的 `classifications`：`domain_id`、`type`（1 或 2）、`wing_tip_id`（仅 type=1）
2. 将后缘面按类型分为两组：类型一列表（type=1）和类型二列表（type=2）。
3. **先逐个处理所有类型一的后缘面**，全部完成后，**再逐个处理所有类型二的后缘面**。

### 类型一：翼梢相邻

> ⚠️ **必须按步骤 1→8 顺序执行，禁止跳过任何步骤。** 不允许在步骤 2 完成后直接跳到其他未列出的工具。

1. 调用 `MergeEdgesByDomain`（当前后缘面 ID），得到 `longids`（L1、L2，两条长边）和 `shortids`（S1、S2，两条短边）。
   - 若返回为空或缺少 `longids`/`shortids` → 失败停止。
2. **验证交线位置**：后缘面与翼梢面的公共线（交线）必须在短边中。
   - 调用 `GetConnectorsByDomain`（后缘面 ID）和 `GetConnectorsByDomain`（翼梢面 ID），取两者返回的 connector ID 交集。
   - 交线必须等于 S1 或 S2，否则 → 失败停止。
3. **短边设点数 + 平均分布**（S1、S2 各一遍），短边为厚度方向，点数固定为 5，无需根据模型尺寸调整：
   - `SetConnectorPointCount`（当前短边 ID, 5）→ 得到 `new_id`（S1 新 ID）
   - `SetConnectorAverageDistribution`（S1 新 ID）→ 得到 `new_id`（S1 最终 ID）
   - S2 同理。
4. **判定 L1、L2 的方向**（哪端靠近翼梢）：
   - 调用 `DetermineDirectionForType1`（后缘面 ID、翼梢面 ID、L1 ID、L2 ID），得到 `l1_tip_end` 和 `l2_tip_end`。
   - `l1_tip_end` / `l2_tip_end` 值为 `"start"` 或 `"end"`，表示该长边的哪一端靠近翼梢。
   - 靠近翼梢端用 `bodySpacing`，另一端用 `rootSpacing`。
     - `l1_tip_end` = "start" → headspace = `bodySpacing`, tailspace = `rootSpacing`
     - `l1_tip_end` = "end" → headspace = `rootSpacing`, tailspace = `bodySpacing`
5. **长边 L1 平滑分布**：
   - `SetConnectorSmoothDistribution`（L1, headspace, tailspace, params, rootSpacing）→ 得到 `new_id`（L1 新 ID）
6. **长边 L2 设点数 + 平滑分布**：
   - `CopyConnectorPointCount`（L1 新 ID, L2）→ 得到 `new_id`（L2 新 ID，点数与 L1 一致）
   - `SetConnectorSmoothDistribution`（L2 新 ID, headspace, tailspace, params, rootSpacing）→ 得到 `new_id`（L2 最终 ID）
7. `DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）
8. `AssembleConnectorsToDomain`（四条网格线最新 ID：L1、L2、S1、S2，逗号分隔）

### 类型二：吊舱-机身相邻

> ⚠️ **类型二禁止调用 `MergeEdgesByDomain`**。该工具仅适用于类型一（4 条边场景），类型二有 6 条边，必须使用 `IdentifyType2Roles` 进行角色识别。
>
> **`IdentifyType2Roles` 返回失败时（`status` 为 `"failed"` 或 `success` 为 `false`），立即停止该后缘面的处理，报告失败原因。禁止改用 `MergeEdgesByDomain` 或任何其他工具替代。**

1. 调用 `IdentifyType2Roles`（后缘面 ID、**步骤0 `ClassifyTrailingEdgeDomains` 返回的 `engine_domains`** 逗号分隔、**步骤0 返回的 `fuselage_domains`** 逗号分隔），得到 6 条网格线的角色 ID：
   - **严禁使用其他工具返回的分组 ID 替代。必须且只能使用步骤0 `ClassifyTrailingEdgeDomains` 返回的 `engine_domains` 和 `fuselage_domains`。**
   - `A`（与 engine 的公共线）、`B`（与 fuselage 的公共线）
   - `C`（不连 A 也不连 B）、`D`（连 A 又连 B）、`E`（只连 A）、`shortEdge`/`F`（只连 B）
   - 各端点和方向信息：`a_start_id`、`a_end_id`、`b_start_id`、`b_end_id`、`c_near_a_end`、`d_near_a_end`、`e_near_a_end`、`d_a_side`、`e_a_side`

2. **A、B、C 设点数 + 平均分布**（逐一执行）：
   - `SetConnectorPointCount`（当前线 ID, 5）→ 得到 `new_id`（当前线新 ID）
   - `SetConnectorAverageDistribution`（当前线新 ID）→ 得到 `new_id`（当前线最终 ID）

3. **读取 A 的端部间距**：
   - `GetConnectorStartAndEndUnitLenth`（A 最新 ID）→ 得到 `start`（首端间距）和 `end`（尾端间距）

4. **D 平滑分布**：
   - D 连接 A（engine 侧）与 B（fuselage 侧），靠近 A 端的间距须匹配 A 的端部间距，靠近 B 端用 `bodySpacing`。
   - **确定 A 端间距值**：若 `d_a_side` 为 `"start"` 则取步骤 3 的 `start` 值，若为 `"end"` 则取 `end` 值。记此值为 `aSpacing`。
   - **确定 headspace/tailspace 方向**：根据 `d_near_a_end` 判断 D 自身哪一端靠近 A：
     - `d_near_a_end` = `"start"` → headspace = `aSpacing`, tailspace = `bodySpacing`
     - `d_near_a_end` = `"end"` → headspace = `bodySpacing`, tailspace = `aSpacing`
   - `SetConnectorSmoothDistribution`（D, headspace, tailspace, params, rootSpacing）→ 得到 `new_id`（D 新 ID）
5. **E 平滑分布**：
   - E 只连 A（不连 B），靠近 A 端的间距须匹配 A 的端部间距，另一端用 `rootSpacing`。
   - **确定 A 端间距值**：若 `e_a_side` 为 `"start"` 则取步骤 3 的 `start` 值，若为 `"end"` 则取 `end` 值。记此值为 `aSpacing`。
   - **确定 headspace/tailspace 方向**：根据 `e_near_a_end` 判断 E 自身哪一端靠近 A：
     - `e_near_a_end` = `"start"` → headspace = `aSpacing`, tailspace = `rootSpacing`
     - `e_near_a_end` = `"end"` → headspace = `rootSpacing`, tailspace = `aSpacing`
   - `SetConnectorSmoothDistribution`（E, headspace, tailspace, params, rootSpacing）→ 得到 `new_id`（E 新 ID）
6. **F（shortEdge）设点数 + 平滑分布**：
   - 读取 A、D、E 的最新点数：
     - `GetPointCount`（A 最新 ID）→ A 的点数
     - `GetPointCount`（D 新 ID）→ D 的点数
     - `GetPointCount`（E 新 ID）→ E 的点数
   - F 的点数等于 A+D+E 三条线串联后的总点数（串联处共享端点，故减2）
   - `SetConnectorPointCount`（F, totalPoint）→ 得到 `new_id`（F 新 ID）
   - `SetConnectorSmoothDistribution`（F 新 ID, bodySpacing, bodySpacing, params, rootSpacing）→ 得到 `new_id`（F 最终 ID）

7. `DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）
8. `AssembleConnectorsToDomain`（6 条网格线最新 ID：A、B、C、D、E、F，逗号分隔）

## 完成标准

- 所有工具调用返回 `success` 为 `true`。
- 任一步骤失败（`success` 为 `false`）时停止，不继续后续步骤。
- 类型一：4 条网格线已装配为结构面；类型二：6 条网格线已装配为结构面。
