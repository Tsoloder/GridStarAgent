# 类型二：吊舱-机身相邻后缘面处理

类型二后缘面由 **6 条网格线** 组成。

> ⚠️ **处理方式选择**
>
> **必须先使用批量处理入口**（`ProcessTrailingEdgeType2`），它一次性完成步骤 2→8 的全部流程。
>
> **调用前必须先通过 `IdentifyType2Roles` 获取 6 条角色线的 ID 和端点信息**，然后将结果作为参数传入 `ProcessTrailingEdgeType2`。`ProcessTrailingEdgeType2` **不会**内部调用 `IdentifyType2Roles`。
>
> 若批量处理返回 `status=failed`，则**回退到分步流程**，按下方步骤 1→8 顺序执行。

> ⚠️ **🚨 类型二有 6 条边！角色判定使用** **`IdentifyType2Roles`** 获取角色线 ID 与端点信息，失败时回退到**分组网格线求交集 + 端点 ID 对比**（见步骤 1 方式二）。
>
> `MergeEdgesByDomain` 仅适用于类型一（4 条边），类型二如果调用会失败。

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
| A  | 短边 | 连接 enginePylonTrailingEdge（吊舱/发动机吊架后缘），与 E、D 共点；属于下表面（`wingLowerSurface`） |
| B  | 短边 | 与 fuselage（机身）共边，与 D、F 共点                               |
| C  | 短边 | 连接 E 和 F；与类型一的面共边                                       |
| D  | 长边 | 属于下表面（`wingLowerSurface`），与 A、B 共点               |
| E  | 长边 | 属于下表面（`wingLowerSurface`），与 A、C 共点               |
| F  | 长边 | 属于上表面（`wingUpperSurface`），与 B、C 共点               |

环顺序：**A — E — C — F — B — D — (回到 A)**

***

## 批量处理入口

**前置步骤**：先调用 `IdentifyType2Roles` 获取角色线信息（详见步骤 1），再将其返回值作为参数传入。

```python
# 第一步：通过 IdentifyType2Roles 获取角色线 ID 和端点信息
# （需要先通过 GetSpliteAssemlyDomainsBatch 获取各分组网格面 ID）
roles = IdentifyType2Roles(
    teDomainId=te_domain_id,
    engineDomainIds="engine_pylon_dom_ids",        # 逗号分隔的 enginePylonTrailingEdge 分组网格面 ID
    fuselageDomainIds="fuselage_dom_ids",    # 逗号分隔的 fuselage 分组网格面 ID
    upperSurfaceDomainIds="upper_dom_ids",   # 逗号分隔的 wingUpperSurface 分组网格面 ID
    lowerSurfaceDomainIds="lower_dom_ids"    # 逗号分隔的 wingLowerSurface 分组网格面 ID
)
# 返回: A, B, C, D, E, F (角色线 ID), A_start, A_end, ..., F_start, F_end (端点 ID), assembly_order

# 第二步：将 IdentifyType2Roles 的返回值传入 ProcessTrailingEdgeType2
ProcessTrailingEdgeType2(
    teDomainId=te_domain_id,
    connectorA=roles["A"], connectorB=roles["B"], connectorC=roles["C"],
    connectorD=roles["D"], connectorE=roles["E"], connectorF=roles["F"],
    aStart=roles["A_start"], aEnd=roles["A_end"],
    bStart=roles["B_start"], bEnd=roles["B_end"],
    cStart=roles["C_start"], cEnd=roles["C_end"],
    dStart=roles["D_start"], dEnd=roles["D_end"],
    eStart=roles["E_start"], eEnd=roles["E_end"],
    fStart=roles["F_start"], fEnd=roles["F_end"],
    assemblyOrder=roles["assembly_order"],
    halfSpan=14.89,              # 半展长（来自 GetModelParameters 的 wing_half_span）
    localChord=3.59,             # 当地弦长（来自 GetModelParameters 的 mac）
    bodySpacing=0.01489,         # 机身边间距
    rootSpacing=0.0718,          # 共点侧间距
    params="1.2,10,1.2,10"       # 分布参数
)
```

返回 `{"status":"success","result":"{\"domain_id\":...,...,\"message\":\"type2 done\"}"}` 表示成功；
返回 `{"status":"failed","message":"..."}` 时回退到下方分步流程。

---

## 分步流程

> 仅在批量处理入口失败时执行以下步骤。

## 步骤

1. **识别角色**（优先使用 `IdentifyType2Roles`，失败时回退到手动方式）：
   1. **方式一（优先）**：调用 `IdentifyType2Roles` 获取角色线 ID 与端点信息：
      1. 调用 `GetSpliteAssemlyDomainsBatch`（`group_names` = `["enginePylonTrailingEdge", "fuselage", "wingUpperSurface", "wingLowerSurface"]`），一次获取吊舱/发动机吊架后缘、机身、机翼上表面、机翼下表面各分组的**网格面 ID 列表**。
      2. 调用 `IdentifyType2Roles`（`teDomainId`=当前后缘面 ID，`engineDomainIds`、`fuselageDomainIds`、`upperSurfaceDomainIds`、`lowerSurfaceDomainIds` 分别为第 1 步得到的各分组网格面 ID，逗号分隔），返回：
         - 各角色线 ID：`A`、`B`、`C`、`D`、`E`、`F`
         - 每条线的首尾点 ID：`A_start`/`A_end`、`B_start`/`B_end`、`C_start`/`C_end`、`D_start`/`D_end`、`E_start`/`E_end`、`F_start`/`F_end`
         - 装配顺序 `assembly_order` = `"A,E,C,F,B,D"`
      3. 若 `IdentifyType2Roles` 返回 `success=false` 或缺少角色/端点字段 → **回退到方式二**
   2. **方式二（回退）**：通过**分组网格线求交集 + 端点 ID 对比**手动判断角色：
      1. 调用 `GetSpliteAssemlyDomainsBatch` 获取各分组网格面 ID 列表（若方式一中已调用过则复用结果）
      2. 通过各分组与后缘面的相交关系，结合角色定义表中的属性（A=engine+下表面、B=fuselage、C=类型一邻面、D/E=下表面、F=上表面），逐一匹配各边的角色
      3. 通过端点 ID 对比确认首尾点连接关系（相同 ID 即为共点/相连）
   3. **校验环连接关系**（两种方式均需执行）：
      - 校验环连接关系：`A—E`、`E—C`、`C—F`、`F—B`、`B—D`、`D—A`（即 A — E — C — F — B — D — 回到 A）
      - 若任一连接关系不满足 → 失败停止，报告原因。
2. **A、B、C 设点数 + 平均分布**（逐一执行），点数固定为 5：
   - 先调用 `GetPointCount`（当前线 ID）获取当前点数。
   - 若点数 != 5：
     - `SetConnectorPointCount`（当前线 ID, 5）
     - `SetConnectorAverageDistribution`（当前线 ID）
   - 若点数 == 5：跳过操作。
3. **读取 A 的端部间距**：
   - `GetConnectorsStartAndEndUnitLenth`（`connector_ids` = `[A]`）→ 从返回的 `connectors` 数组中取 `start` 和 `end`
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
   - 先获取 E、A、D 的当前点数，计算并设置 F 的点数：
     - 分别调用 `GetPointCount`（E）、`GetPointCount`（A）、`GetPointCount`（D），得到 `eCount`、`aCount`、`dCount`
     - 计算 `fPointCount = eCount + aCount + dCount - 2`
     - `SetConnectorPointCount`（F, fPointCount）
     - `SetConnectorAverageDistribution`（F）
   - 再拷贝 E、A、D 的分布模式到 F：
     - **必须使用** **`UGReDimensionCopy`**
     - `UGReDimensionCopy`（`"E,A,D"`, `F`）
     - 参数说明：`ids` = E、A、D 三条线的 ID 逗号分隔，`targetId` = F 的 ID
   - 拷贝后，判断 F 与 E、D 的首尾点方向是否一致（通过第一步得到的端点 ID 对比）：
     - 通过端点 ID 对比，判断 F 的哪一端连接 C：
       - 若 `F_start` == `C_start` 或 `F_start` == `C_end` → F 的 start 端连 C
       - 若 `F_end` == `C_start` 或 `F_end` == `C_end` → F 的 end 端连 C
     - 同理判断 E 的哪一端连接 C
     - 同理判断 D 的哪一端连接 B
     - **方向一致的条件**：
       - F 的 start 端连 C（F_start == C 的共点）
       - 且 E 的 start 端也连 C（E_start == C 的共点）
       - 且 D 的 end 端连 B（D_end == B 的共点）
       - 即三条线在环上朝向一致：F 从 C→B、E 从 C→A、D 从 A→B
     - 若任一条件不满足（即 F 的 end 端连 C，方向不一致）：
       - `UGReDimensionInversionDistribution`（F）→ 对 F 的分布进行反向
7. `DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）
8. `AssembleConnectorsToDomain`（6 条网格线按环顺序排列，逗号分隔）
   - 装配顺序沿第 1 步校验过的环顺序：`A,E,C,F,B,D`（A — E — C — F — B — D — 回到 A）
   - 第 1 步判定的角色 ID（A、B、C、D、E、F）即为最新 ID，因为操作后 ID 不变。

