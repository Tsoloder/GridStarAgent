# 类型一：翼梢相邻后缘面处理

类型一后缘面与翼梢面相邻，由 4 条网格线（2 条长边 L1/L2、2 条短边 S1/S2）组成。

> ⚠️ **必须按步骤 1→8 顺序执行，禁止跳过任何步骤。**

> ⚠️ **全局注意事项**
>
> - `CopyConnectorPointCount` 的 `sourceId` 是点数来源（拷贝**源**），`targetIds` 是接收点数的目标组，切勿搞反。
> - 工具返回 `success` 为 `false` 时**立即停止**，报告失败。
> - 交线判定失败（后缘面与翼梢面无公共短边）时立即停止。
> - `SetConnectorPointCount`、`SetConnectorAverageDistribution`、`SetConnectorSmoothDistribution`、`CopyConnectorPointCount` 返回格式为 `{"success":true}`，**操作后网格线 ID 不变**，无需追踪新 ID。
> - manual 模式下，中间步骤直接执行，**不输出 `tool_params`**。仅 `DeleteDomain` 通过 `options` 请求确认；auto 模式下 `DeleteDomain` 直接执行，不请求确认。

**间距参数**：
- `bodySpacing` = 0.01489（靠近翼梢端）
- `rootSpacing` = 0.0718（靠近翼根端）
- `params` = `"1.2,10,1.2,10"`

---

## 步骤

1. **合并边**：调用 `MergeEdgesByDomain`（当前后缘面 ID），得到 `longids`（L1、L2）和 `shortids`（S1、S2）。
   - 若返回为空或缺少 `longids`/`shortids` → 失败停止。

2. **验证交线位置**：后缘面与翼梢面的公共线（交线）必须在短边中。
   - 调用 `GetConnectorsByDomain`（后缘面 ID）和 `GetConnectorsByDomain`（翼梢面 ID），取交集。
   - 交线必须等于 S1 或 S2，否则 → 失败停止。

3. **短边设点数 + 平均分布**（S1、S2 各一遍），点数固定为 5：
   - 先调用 `GetPointCount`（当前短边 ID）获取当前点数。
   - 若点数 != 5：
     - `SetConnectorPointCount`（当前短边 ID, 5）
     - `SetConnectorAverageDistribution`（当前短边 ID）
   - 若点数 == 5：跳过操作。
   - **S2 同理**（仍使用 S2 的原始 ID）。

4. **判定 L1、L2 的方向**：
   - 调用 `DetermineDirectionForType1`（后缘面 ID、翼梢面 ID、L1 ID、L2 ID），得到 `l1_tip_end` 和 `l2_tip_end`。
   - `l1_tip_end` = `"start"` → L1 的 start 端靠近翼梢（headspace=`bodySpacing`, tailspace=`rootSpacing`）
   - `l1_tip_end` = `"end"` → L1 的 end 端靠近翼梢（headspace=`rootSpacing`, tailspace=`bodySpacing`）
   - L2 同理。

5. **长边 L1 平滑分布**：
   - `SetConnectorSmoothDistribution`（L1, headspace, tailspace, params, rootSpacing）

6. **长边 L2 设点数 + 平滑分布**：
   - `CopyConnectorPointCount`（L1, L2）
   - `SetConnectorSmoothDistribution`（L2, headspace, tailspace, params, rootSpacing）

7. `DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）

8. `AssembleConnectorsToDomain`（L1, L2, S1, S2，逗号分隔，使用原始 ID，因操作后 ID 不变）
