# 类型二：吊舱-机身相邻后缘面处理

类型二后缘面由 **6 条网格线** 组成。

> ⚠️ **🚨 类型二有 6 条边！第一条调用必须是 `IdentifyType2Roles`，不是 `MergeEdgesByDomain`！**
>
> `MergeEdgesByDomain` 仅适用于类型一（4 条边），类型二如果调用会失败。

> ⚠️ **全局注意事项**
>
> - `CopyConnectorPointCount` 的 `sourceId` 是点数来源（拷贝**源**），`targetIds` 是接收点数的目标组，切勿搞反。
> - 工具返回 `success` 为 `false` 时**立即停止**，报告失败。
> - `SetConnectorPointCount`、`SetConnectorAverageDistribution`、`SetConnectorSmoothDistribution`、`CopyConnectorPointCount` 返回格式为 `{"success":true}`，**操作后网格线 ID 不变**，无需追踪新 ID。
> - manual 模式下，中间步骤直接执行，**不输出 `tool_params`**。仅 `DeleteDomain` 通过 `options` 请求确认。
> - `IdentifyType2Roles` 返回失败时（`status` 为 `"failed"`），立即停止，报告失败原因。

**间距参数**：
- `bodySpacing` = 0.01489（靠近机身边）
- `rootSpacing` = 0.0718（有共点的网格线侧/中间值）
- `params` = `"1.2,10,1.2,10"`

---

## 角色定义

| 角色 | 类型 | 属性 |
|------|------|------|
| A | 短边 | 连接 engine（吊舱），与 E、D 共点；属于下表面（`jiyi_wing_lower_surface`） |
| B | 短边 | 与 fuselage（机身）共边，与 D、F 共点 |
| C | 短边 | 连接 E 和 F；与类型一的面共边 |
| D | 长边 | 属于下表面（`jiyi_wing_lower_surface`），与 A、B 共点 |
| E | 长边 | 属于下表面（`jiyi_wing_lower_surface`），与 A、C 共点 |
| F | 长边 | 属于上表面（`jiyi_wing_upper_surface`），与 B、C 共点 |

环顺序：**A — E — C — F — B — D — (回到 A)**

---

## 步骤

1. **识别角色**：调用 `IdentifyType2Roles`（后缘面 ID、**步骤0 `ClassifyTrailingEdgeDomains` 返回的 `engine_domains`** 逗号分隔、**步骤0 返回的 `fuselage_domains`** 逗号分隔、机翼上表面分组 ID 逗号分隔、机翼下表面分组 ID 逗号分隔），得到 6 条网格线的角色 ID 和端点信息：
   - `A`（短边，连 engine）、`B`（短边，连 fuselage）、`C`（短边，连 E 和 F）
   - `D`（长边，下表面，连 A 和 B）、`E`（长边，下表面，连 A 和 C）、`F`（长边，上表面，连 B 和 C）
   - 各线首尾点 ID：`A_start`/`A_end`、`B_start`/`B_end`、`C_start`/`C_end`、`D_start`/`D_end`、`E_start`/`E_end`、`F_start`/`F_end`
     - 通过端点 ID 对比判断共点关系：相同 ID 即为相连
   - `assembly_order`（装配顺序，如 `"A,E,C,F,B,D"`）
   - `status`：成功为 `"success"`，失败为 `"failed"`

2. **A、B、C 设点数 + 平均分布**（逐一执行），点数固定为 5：
   - 先调用 `GetPointCount`（当前线 ID）获取当前点数。
   - 若点数 != 5：
     - `SetConnectorPointCount`（当前线 ID, 5）
     - `SetConnectorAverageDistribution`（当前线 ID）
   - 若点数 == 5：跳过操作。

3. **读取 A 的端部间距**：
   - `GetConnectorStartAndEndUnitLenth`（A）→ 得到 `start` 和 `end`

4. **E 平滑分布**：
   - 对比 `E_start`/`E_end` 与 `A_start`/`A_end`，判断 E 哪端连 A：
     - 若 `E_start` 等于 `A_start` 或 `A_end` → E 的 start 端连 A
     - 若 `E_end` 等于 `A_start` 或 `A_end` → E 的 end 端连 A
   - 取 A 对应端的间距值 `aSpacing`：
     - E 的 start 端连 A → `aSpacing` = A 的 start 间距
     - E 的 end 端连 A → `aSpacing` = A 的 end 间距
   - 靠近 A 的那端 → headspace=`aSpacing`，另一端 → tailspace=`rootSpacing`
     - 若 E 的 start 端连 A → headspace=`aSpacing`, tailspace=`rootSpacing`
     - 若 E 的 end 端连 A → headspace=`rootSpacing`, tailspace=`aSpacing`
   - `SetConnectorSmoothDistribution`（E, headspace, tailspace, params, rootSpacing）（rootSpacing 作为总长参数）

5. **D 平滑分布**：
   - 对比 `D_start`/`D_end` 与 `A_start`/`A_end`，判断 D 哪端连 A
   - 取 A 对应端的间距值 `aSpacing`
   - 靠近 A 的那端 → headspace=`aSpacing`，另一端 → tailspace=`bodySpacing`
     - 若 D 的 start 端连 A → headspace=`aSpacing`, tailspace=`bodySpacing`
     - 若 D 的 end 端连 A → headspace=`bodySpacing`, tailspace=`aSpacing`
   - `SetConnectorSmoothDistribution`（D, headspace, tailspace, params, rootSpacing）

6. **F 设点数**
   应该先获取E、A、D的点数，然后设置F的点数为E、A、D点数之和-2
   （拷贝 E、A、D 的点数到 F）：
   - **必须使用 `UGReDimensionCopy`**
   - `UGReDimensionCopy`（`"E,A,D"`, `F`）
   - 参数说明：`ids` = E、A、D 三条线的 ID 逗号分隔，`targetId` = F 的 ID

7. `DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）

8. `AssembleConnectorsToDomain`（6 条网格线按 `assembly_order` 顺序排列，逗号分隔）
   - `assembly_order` 由 `IdentifyType2Roles` 返回，如 `"A,E,C,F,B,D"`
   - `IdentifyType2Roles` 返回的角色 ID（A、B、C、D、E、F）即为最新 ID，因为操作后 ID 不变。
   - 若 `assembly_order` 字段不存在，使用默认顺序 `A,E,C,F,B,D`
