# 类型二：吊舱-机身相邻后缘面处理

类型二后缘面由 **6 条网格线** 组成。

> ⚠️ **🚨 类型二有 6 条边！角色判定不使用** **`IdentifyType2Roles`（该工具有问题），也不使用** **`MergeEdgesByDomain`！**
>
> `MergeEdgesByDomain` 仅适用于类型一（4 条边），类型二如果调用会失败。
> 角色判定通过 **分组网格线求交集 + 端点 ID 对比** 完成（见步骤 1）。

> ⚠️ **全局注意事项**
>
> - 工具返回 `success` 为 `false` 时**立即停止**，报告失败。
> - `SetConnectorPointCount`、`SetConnectorAverageDistribution`、`SetConnectorSmoothDistribution`、`CopyConnectorPointCount` 返回格式为 `{"success":true}`，**操作后网格线 ID 不变**，无需追踪新 ID。
> - manual 模式下，中间步骤直接执行，**不输出** **`tool_params`**。仅 `DeleteDomain` 通过 `options` 请求确认；auto 模式下 `DeleteDomain` 直接执行，不请求确认。
> - 角色判定返回 `success` 为 `false`、缺少角色/端点字段或环连接校验不通过时，立即停止，报告失败原因。

**间距参数**：

- `bodySpacing` = 0.01489（靠近机身边）
- `rootSpacing` = 0.0718（有共点的网格线侧/中间值）
- `params` = `"1.2,10,1.2,10"`

***

## 角色定义

| 角色 | 类型 | 属性                                                      |
| -- | -- | ------------------------------------------------------- |
| A  | 短边 | 连接 engine（吊舱），与 E、D 共点；属于下表面（`jiyi_wing_lower_surface`） |
| B  | 短边 | 与 fuselage（机身）共边，与 D、F 共点                               |
| C  | 短边 | 连接 E 和 F；与类型一的面共边                                       |
| D  | 长边 | 属于下表面（`jiyi_wing_lower_surface`），与 A、B 共点               |
| E  | 长边 | 属于下表面（`jiyi_wing_lower_surface`），与 A、C 共点               |
| F  | 长边 | 属于上表面（`jiyi_wing_upper_surface`），与 B、C 共点               |

环顺序：**A — E — C — F — B — D — (回到 A)**

***

## 步骤

1. **识别角色**（使用 `IdentifyType2Roles` 一次获取角色线 ID 与端点信息）：
   1. 调用 `GetSpliteAssemlyDomains`（4 次，`groupName` 分别为 `engine`、`fuselage`、`jiyi_wing_upper_surface`、`jiyi_wing_lower_surface`），得到吊舱、机身、机翼上表面、机翼下表面各分组的**网格面 ID 列表**。
   2. 调用 `IdentifyType2Roles`（`teDomainId`=当前后缘面 ID，`engineDomainIds`、`fuselageDomainIds`、`upperSurfaceDomainIds`、`lowerSurfaceDomainIds` 分别为第 1 步得到的各分组网格面 ID，逗号分隔），返回：
      - 各角色线 ID：`A`、`B`、`C`、`D`、`E`、`F`
      - 每条线的首尾点 ID：`A_start`/`A_end`、`B_start`/`B_end`、`C_start`/`C_end`、`D_start`/`D_end`、`E_start`/`E_end`、`F_start`/`F_end`
      - 装配顺序 `assembly_order` = `"A,E,C,F,B,D"`
   3. 通过端点 ID 对比确认首尾点连接关系（相同 ID 即为共点/相连）：
      - 校验环连接关系：`A—E`、`E—C`、`C—F`、`F—B`、`B—D`、`D—A`（即 A — E — C — F — B — D — 回到 A）
      - 若任一连接关系不满足 → 失败停止，报告原因。
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
6. **F 设点数 + 拷贝 E、A、D 的分布**
   1. 分别调用 `GetPointCount`（E）、`GetPointCount`（A）、`GetPointCount`（D），得到三条线的点数。
   2. 计算目标点数：`F点数 = E点数 + A点数 + D点数 - 2`。
   3. 调用 `UGReDimensionSetSpecifiedValue`（F, 目标点数）显式设置 F 的点数。
   4. 用 `UGReDimensionCopy` 把 E、A、D 三条线的**分布**拷贝到 F。
   5. 若拷贝后发现 F 的疏密方向反了（F 的 start 端间距不等于链首端的间距），调用 `UGReDimensionInversionDistribution`（F）翻转分布方向。
7. `DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）
8. `AssembleConnectorsToDomain`（6 条网格线按环顺序排列，逗号分隔）
   - 装配顺序沿第 1 步校验过的环顺序：`A,E,C,F,B,D`（A — E — C — F — B — D — 回到 A）
   - 第 1 步判定的角色 ID（A、B、C、D、E、F）即为最新 ID，因为操作后 ID 不变。

