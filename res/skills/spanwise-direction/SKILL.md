---
name: spanwise-direction
description: 机翼展向（spanwise direction）各向异性网格线分布处理，处理对象就是前缘线（机翼上表面与下表面分部件分组的交线）。前缘线按与机身 fuselage / 翼梢 wingTip 是否共点分为类型一、类型二，只处理最两端两根：共点端取 0.1% × 半展长，另一端保留原网格线端分布，中间值与翼面尺寸一致。当总 Skill anisotropic-mesh-processing 路由到机翼展向处理、前缘面处理或前缘线处理时加载本 Skill。
aliases: [展向分布, 展向加密, spanwise, 机翼展向, 翼根加密, 翼梢加密, 前缘面处理, 前缘面, 前缘线]
tags: [CFD, 网格, 各向异性, 展向, 机翼, 前缘面]
category: CFD
version: 2.0.0
author: QtChatWidget
allowed-tools: [IdentifyLeadingEdge]
---

# 机翼展向各向异性网格线分布（前缘线处理）

**机翼展向各向异性分布就是前缘线的处理**，二者是同一件事，不存在独立的"展向网格线"流程。本 Skill 只有一条流程，即下文的前缘线处理。

## 处理对象与范围

- **前缘线**：分部件分组**机翼上表面** **`wingUpperSurface`** 与**机翼下表面** **`wingLowerSurface`** 的**交线**（两组网格面共有的网格线）。
- 前缘线通常有**多根**（沿展向分段），**只处理最两端的两根**，中间段一律保持原分布不动。

| 类型  | 判定依据                              | 位置     | 共点端间距      | 另一端间距 |
| --- | --------------------------------- | ------ | ---------- | ----- |
| 类型一 | 与机身 `fuselage` 分组的网格面有**共点**      | 翼根端那一根 | 0.1% × 半展长 | 原分布值  |
| 类型二 | 与翼梢 `wingTip` 分组的网格面有**共点** | 翼梢端那一根 | 0.1% × 半展长 | 原分布值  |

## 前置条件

1. 调用 `GetAllSpitAssemblyGroupProperty`，返回中存在 `fuselage`、`wingTip`、`wingUpperSurface`、`wingLowerSurface` 四个分组。
2. 调用 `GetModelParameters`，获取机翼半展长与 MAC；返回 0 时回退 `cfd-meshing-workflow/references/geometry-parameters.md` 默认值（半展长 586.10、MAC 144.74）。
3. 表面网格已生成：`GetAllObjectByType`(6) 返回非空。
4. 上述任一条件不满足时停止。

## 分布参数

| 参数                               | 值                                                |
| -------------------------------- | ------------------------------------------------ |
| 共点端间距（`headspace` 或 `tailspace`） | 0.1% × 半展长 = 0.1% × 1/2 b                        |
| 另一端间距                            | **原分布值**，由 `GetConnectorStartAndEndUnitLenth` 读取 |
| `params`                         | `"1.2,50,1.2,50"`（增长率 1.2、层数 50）                 |
| 分布类型（`disFunc`）                  | 0（双曲正切）                                          |
| `mindValue`（中间值）                 | 翼面尺寸（机翼上/下表面分组 `targetSize` = MAC × 2%）          |

> 示例（半展长 586.10、MAC 144.74）：共点端间距 = 0.5861，`mindValue` = 2.8948。
>
> ⚠️ 共点端落在网格线的 start 端还是 end 端，决定 `headspace` / `tailspace` 怎么传，**必须先判端再传参**。

## 执行步骤

1. **获取各分组网格面 ID**：
   1. `GetSpliteAssemlyDomains`（`groupName` = `wingUpperSurface`）→ 上表面网格面 ID 列表字符串（如 `"1,2,3"`）。
   2. `GetSpliteAssemlyDomains`（`groupName` = `wingLowerSurface`）→ 下表面网格面 ID 列表字符串。
   3. `GetSpliteAssemlyDomains`（`groupName` = `fuselage`）→ 机身网格面 ID 列表字符串。
   4. `GetSpliteAssemlyDomains`（`groupName` = `wingTip`）→ 翼梢网格面 ID 列表字符串。
   任一返回为空 → 失败停止。

2. **识别前缘线**：调用 `IdentifyLeadingEdge`（
      `upperSurfaceDomainIds` = 上表面面ID字符串，
      `lowerSurfaceDomainIds` = 下表面面ID字符串，
      `fuselageDomainIds` = 机身面ID字符串，
      `wingTipDomainIds` = 翼梢面ID字符串）
   ↓
   返回 JSON 包含：
   - `leading_edge_ids`：所有前缘线 ID 列表
   - `fuselage_adjacent`：与机身共点的前缘线 ID + 共点端（`start`/`end`）→ **类型一**
   - `wing_tip_adjacent`：与翼梢共点的前缘线 ID + 共点端（`start`/`end`）→ **类型二**
   `leading_edge_ids` 为空或 `fuselage_adjacent`/`wing_tip_adjacent` 缺失 → 失败停止。
   不在上述两端的其他前缘线 → **中间段，跳过不处理**。
5. **读取原分布值**：对类型一、类型二各调用 `GetConnectorStartAndEndUnitLenth`（线 ID）→ `{"start": s0, "end": e0}`。
   > 必须在 `UGReDimensionConfigDistribution` **之前**读取，否则原值会被覆盖。
6. **计算间距**：`edgeSpacing` = 0.001 × 半展长；`mindValue` = 翼面尺寸。
7. **组装参数并设置增长分布**（类型一、类型二各执行一次）：
   - 共点端 = start 端 → `headspace` = `edgeSpacing`，`tailspace` = `e0`
   - 共点端 = end 端 → `headspace` = `s0`，`tailspace` = `edgeSpacing`
   - 调用 `UGReDimensionConfigDistribution`（`ids` = 该线 ID，`headspace`，`tailspace`，`params` = `"1.2,50,1.2,50"`，`mindValue` = 翼面尺寸）。
8. **对边匹配**（如需）：`UGReDimensionMatch`，使前缘线与相邻上/下翼面网格线点数一致。
9. **平滑过渡**（如需）：`UGReDimensionSmoothDistribution`，参数与步骤 7 相同。

## 注意事项

- **只处理最两端两根前缘线**（类型一、类型二各一根），中间段一律不动。
- 共点端取 0.1% × 半展长，另一端保留原分布值，**切勿两端都写** **`edgeSpacing`**。
- 共点判定只比对**网点 ID**（`pointID`），ID 相同即为共点，不要用坐标近似比较。
- 展向分布**只控制内部点分布**，不更改边界分布。
- `params` 严格格式为 `"headRate,headLayer,tailRate,tailLayer"`，不支持空格、中文逗号或科学计数法。
- 任一工具返回 `success` 为 `false` 时立即停止，不继续后续步骤。

## 完成标准

- `IdentifyLeadingEdge` 成功返回，`fuselage_adjacent` 与 `wing_tip_adjacent` 均非空。
- 类型一（与机身共点）与类型二（与翼梢共点）各识别出一根前缘线。
- 两根线的共点端间距为 0.1% × 半展长，另一端为原分布值；中间段未被修改。
- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 任一步骤失败时停止，不继续后续步骤。
