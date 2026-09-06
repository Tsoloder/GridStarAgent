---
name: spanwise-direction
description: 机翼展向（spanwise direction）各向异性网格线分布处理。处理机翼翼根到翼梢方向的网格线加密，靠近翼根处和翼梢处的分布均为 0.1% × 半展长，中间值与翼面尺寸一致，其他端使用原网格线端分布。内含前缘面处理流程：前缘线为机翼上表面与下表面分部件分组的交线，按与机身 fuselage / 翼梢 jiyi_wing_tip 是否共点分为类型一、类型二，只处理最两端两根，共点端取 0.1% × 半展长、另一端保留原分布值。当总 Skill anisotropic-mesh-processing 路由到机翼展向处理或前缘面处理时加载本 Skill。
aliases: [展向分布, 展向加密, spanwise, 机翼展向, 翼根加密, 翼梢加密, 前缘面处理, 前缘面, 前缘线]
tags: [CFD, 网格, 各向异性, 展向, 机翼, 前缘面]
category: CFD
version: 1.0.0
author: QtChatWidget
allowed-tools: []
---

# 机翼展向各向异性网格线分布

## 适用位置

- 机翼展向方向（翼根至翼梢）
- 适用于机翼的展向网格线
- **前缘面**（前缘线，见「前缘面处理流程」章节）

## 前置条件

- 已获取机翼的**半展长**（1/2 b）
- 已获取翼面尺寸（中间值用）

## 尺寸参数

| 参数                        | 值                         |
| ------------------------- | ------------------------- |
| 翼根端间距                     | 0.1% × 半展长 = 0.1% × 1/2 b |
| 翼梢端间距                     | 0.1% × 半展长 = 0.1% × 1/2 b |
| 增长率（headRate / tailRate）  | 1.2（默认）                   |
| 层数（headLayer / tailLayer） | 50（默认）                    |
| 分布类型（disFunc）             | 0（双曲正切）                   |
| 中间值（mindValue）            | 翼面尺寸一致                    |
| 其他端                       | 使用原网格线端分布                 |

## 网格线分布设置

### 设置步骤

1. 获取机翼展向方向的网格线 ID。
2. 调用 `UGReDimensionConfigDistribution` 设置分布参数：

| 参数          | 值          |
| ----------- | ---------- |
| `headspace` | 0.1% × 半展长 |
| `tailspace` | 0.1% × 半展长 |
| `headRate`  | 1.2        |
| `headLayer` | 50         |
| `tailRate`  | 1.2        |
| `tailLayer` | 50         |
| `disFunc`   | 0          |
| `mindValue` | 翼面尺寸       |

> **注意**：展向分布的"其他端"（非翼根/翼梢侧）应保持原网格线端分布不变，不要修改。

1. 如需对边匹配，调用 `UGReDimensionMatch`。
2. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution`。

### 注意事项

- 展向分布**只控制内部点分布**，不更改边界分布。
- 翼根和翼梢的间距计算均使用同一半展长值，除非用户明确指定不同值。
- 其他端保留原分布，避免过度约束。

## 前缘面处理流程

前缘面处理即机翼展向各向异性网格线分布，处理对象为**前缘线**。

### 前缘线定义与处理范围

- **前缘线**：分部件分组**机翼上表面** **`jiyi_wing_upper_surface`** 与**机翼下表面** **`jiyi_wing_lower_surface`** 的**交线**（两组网格面共有的网格线）。
- 前缘线通常有**多根**（沿展向分段），**只处理最两端的两根**，中间段一律保持原分布不动。

| 类型  | 判定依据                              | 位置     | 共点端间距      | 另一端间距 |
| --- | --------------------------------- | ------ | ---------- | ----- |
| 类型一 | 与机身 `fuselage` 分组的网格面有**共点**      | 翼根端那一根 | 0.1% × 半展长 | 原分布值  |
| 类型二 | 与翼梢 `jiyi_wing_tip` 分组的网格面有**共点** | 翼梢端那一根 | 0.1% × 半展长 | 原分布值  |

### 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，返回中存在 `fuselage`、`jiyi_wing_tip`、`jiyi_wing_upper_surface`、`jiyi_wing_lower_surface` 四个分组。
2. 调用 `GetModelParameters`，获取机翼半展长与 MAC；返回 0 时回退 `cfd-meshing-workflow/references/geometry-parameters.md` 默认值（半展长 586.10、MAC 144.74）。
3. 表面网格已生成：`GetAllObjectByType`(6) 返回非空。
4. 上述任一条件不满足时停止。

### 分布参数

| 参数                               | 值                                                |
| -------------------------------- | ------------------------------------------------ |
| 共点端间距（`headspace` 或 `tailspace`） | 0.1% × 半展长 = 0.1% × 1/2 b                        |
| 另一端间距                            | **原分布值**，由 `GetConnectorStartAndEndUnitLenth` 读取 |
| `params`                         | `"1.2,50,1.2,50"`（增长率 1.2、层数 50）                 |
| `mindValue`（中间值）                 | 翼面尺寸（机翼上/下表面分组 `targetSize` = MAC × 2%）          |

> 示例（半展长 586.10、MAC 144.74）：共点端间距 = 0.5861，`mindValue` = 2.8948。
>
> ⚠️ 共点端落在网格线的 start 端还是 end 端，决定 `headspace` / `tailspace` 怎么传，**必须先判端再传参**。

### 执行步骤

1. **求前缘线集合**：
   1. `GetSpliteAssemlyDomains`（`groupName` = `jiyi_wing_upper_surface`）→ 上表面网格面 ID 列表。
   2. `GetSpliteAssemlyDomains`（`groupName` = `jiyi_wing_lower_surface`）→ 下表面网格面 ID 列表。
   3. 对两组返回的每个网格面 ID 调用 `GetConnectorsByDomain`，分别汇总为上表面网格线集合、下表面网格线集合。
   4. 取两集合的**交集** → 前缘线 ID 列表（多根）。交集为空 → 失败停止。
2. **建立机身网点集合** **`P_fus`**：
   1. `GetSpliteAssemlyDomains`（`groupName` = `fuselage`）→ 机身网格面 ID 列表。
   2. 逐面调用 `GetConnectorsByDomain` → 机身网格线 ID 列表。
   3. 逐线调用 `GetStartAndEndPointByConnector` → 收集 `start_point.pointID` 与 `end_point.pointID`，汇总为 `P_fus`。
3. **建立翼梢网点集合** **`P_tip`**：同步骤 2，`groupName` 换为 `jiyi_wing_tip`，得到 `P_tip`。
4. **类型判定 + 共点端判定**：对每根前缘线调用 `GetStartAndEndPointByConnector`，得到 `L_start`、`L_end`：
   - `L_start` 或 `L_end` ∈ `P_fus` → **类型一**，命中的那一端即**共点端**。
   - `L_start` 或 `L_end` ∈ `P_tip` → **类型二**，命中的那一端即**共点端**。
   - 两端均不命中 → 中间段，**跳过不处理**。
   - 同一类型命中多于一根，或没有任何前缘线命中 → 失败停止并报告。
5. **读取原分布值**：对类型一、类型二各调用 `GetConnectorStartAndEndUnitLenth`（线 ID）→ `{"start": s0, "end": e0}`。
   > 必须在 `UGReDimensionConfigDistribution` **之前**读取，否则原值会被覆盖。
6. **计算间距**：`edgeSpacing` = 0.001 × 半展长；`mindValue` = 翼面尺寸。
7. **组装参数并设置增长分布**（类型一、类型二各执行一次）：
   - 共点端 = start 端 → `headspace` = `edgeSpacing`，`tailspace` = `e0`
   - 共点端 = end 端 → `headspace` = `s0`，`tailspace` = `edgeSpacing`
   - 调用 `UGReDimensionConfigDistribution`（`ids` = 该线 ID，`headspace`，`tailspace`，`params` = `"1.2,50,1.2,50"`，`mindValue` = 翼面尺寸）。
8. **对边匹配**（如需）：`UGReDimensionMatch`，使前缘线与相邻上/下翼面网格线点数一致。
9. **平滑过渡**（如需）：`UGReDimensionSmoothDistribution`，参数与步骤 7 相同。

### 注意事项

- **只处理最两端两根前缘线**（类型一、类型二各一根），中间段一律不动。
- 共点端取 0.1% × 半展长，另一端保留原分布值，**切勿两端都写** **`edgeSpacing`**。
- 共点判定只比对**网点 ID**（`pointID`），ID 相同即为共点，不要用坐标近似比较。
- 前缘面处理**只控制内部点分布**，不更改边界分布。
- `params` 严格格式为 `"headRate,headLayer,tailRate,tailLayer"`，不支持空格、中文逗号或科学计数法。
- 任一工具返回 `success` 为 `false` 时立即停止，不继续后续步骤。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 其他端分布未被修改。
- 前缘面处理：类型一（与机身共点）与类型二（与翼梢共点）各识别出一根前缘线；共点端间距为 0.1% × 半展长，另一端为原分布值；中间段未被修改。
- 任一步骤失败时停止，不继续后续步骤。 

