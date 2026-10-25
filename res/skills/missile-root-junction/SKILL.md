---
name: missile-root-junction
description: 【回退专用】弹翼与弹体结合处各向异性网格线分布处理。该位置不修改两端分布，使用增长分布（增长率 1.2，层数 50），中间值与翼面尺寸一致。适用于弹翼(wing)连弹体(fuselage)的结合场景。🔴 仅在 `MissileWingAnisoProcessWingAnisotropy` 一站式返回 `status="failed"` 时由 `missile-anisotropy-processing` 自动回退加载；一站式正常时**不得**主动加载本 Skill。
aliases: [翼身结合, 翼体结合, wing-fuselage, wing-junction]
tags: [CFD, 网格, 各向异性, 结合处, 导弹, 翼]
category: CFD
version: 2.2.0
author: Tsolodancer
allowed-tools: []
---

# 弹翼与弹体结合处各向异性网格线分布

## 适用位置

- 弹翼（wing）与弹体的结合处，**绕翼一圈**（前缘根部 + 侧面根部 + 后缘根部三段取并集）
- 根部附近曲率变化较大、需要局部加密的结合区域

## 结合类型

| 部件 | 结合对象 | 分组名 |
|------|---------|--------|
| 弹翼(wing) | 弹体 | `fuselage` |

> 弹翼的**子面前缀**为 `wing`（如 `wingSideSurfaceN` / `wingLeadingEdgeN` / `wingTrailingEdgeN`，N = 第几对弹翼）；结合处取其子面与结合对象（`fuselage`）的共享边，三段取**并集**（绕翼一圈）。
> 🔴 后端只产出带序号子组名，**不存在 `wing` 主组**——子组名即定位键，无需归并、无子面区分丢失问题。

## 前置条件

- 已获取**当地弦长与厚度**（🔴 **根弦长 `wings[N−1].root_chord`、厚度 `wings[N−1].thickness` 均按对取 `GetMissileDimensions` 接口值**，事实中心 §2，**不得各对共用**；🔴 C_tip(N) **按对取并按翼型分叉**（M2）：三角翼（无 `wingTipN`）≈0 正常，禁止 0 代入；梯形翼（有 `wingTipN`）按接口值。**结合处全在根部，一律用该对 C_root(N)**）
- 已获取翼面尺寸（中间值用，**`0.02 × 该对根弦长`**）
- 表面网格已完成
- `UGReDimensionConfigDistribution` 系列工具参数已就绪
- **链路接口可用**：`GetConnectorsByDomains` 能返回真实网格线 ID（若返回兜底值，整个各向异性阶段按主 Skill「工具可用性判定」标记 `skipped`）
- 🔴 **定位方式（按子组名，事实中心 G1/G2）**：弹翼**带序号子面分组**（`wingSideSurfaceN` / `wingLeadingEdgeN` / `wingTrailingEdgeN`，N = 第几对弹翼）即定位键，经 `GetAllSpitAssemblyGroupProperty` 按组名取 `domain[].ids`，三者分别与 `fuselage` 取共享边后取并集。🔴 碎面合并会改超面集合，**进入本阶段前重查一次分组**。子组名缺失 → `skipped` + `note`。

## 尺寸参数

| 参数 | 值 |
|---|---|
| 首层高度（各向异性域 `firstHigh`） | **`0.1 × 该对翼厚度`**（`wings[N−1].thickness`，与翼侧面一致） |
| 增长率（headRate / tailRate） | 1.2（默认） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 中间值（mindValue） | **`0.02 × 该对根弦长`**（`wings[N−1].root_chord`） |
| 根部段 | 保证平滑分布 |

> 🔴 **2026-10-22 口径变更**：旧口径「加密尺寸 `0.005×当地弦长`」**已作废**——结合处首层高度改用**翼厚度**基准。
> 🔴 **翼身结合处参数 = 翼侧面参数**（需求）：首层高度＝`0.1×该对翼厚度`、增长率 1.2、层数 50、中间值＝`0.02×该对根弦长`——四项全同。结合处就是"翼根那一圈的侧面"，不是另一套参数。基准表见 `missile-anisotropic-mesh.md` §「各位置参数」。
> 🔴 **多对翼（N=1/2/3）逐对处理**，每对按该对厚度与根弦长算首层高度与中间值。
>
> ⚠️ 本节首层高度用于**各向异性域生成**（`GenerateANisoDomainGrid` 的 `firstHigh`）；而下方「网格线分布设置」里 `UGReDimensionConfigDistribution` 的 `headspace`/`tailspace` 指的是**该网格线原有的两端间距**，两者不是同一个量——原有两端间距仍按「严禁修改两端分布」处理。

> ⚠️ 上表是**语义值**，不是参数名。真实形参只有 5 个：`UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)`——增长率与层数打包进 `params` 字符串（`"1.2,50,1.2,50"`）；**没有** `headRate` / `headLayer` / `disFunc` 等独立形参，分布类型由所选工具（Config / Smooth）决定。

## 🔴 批量处理时的聚合近似规则

**语义**：本位置"不修改两端分布"是核心约束。结合处共享边可能几十条，**逐条传原值需几十次调用不可行**；但**全量取平均会把物理分组抹平**（短过渡边会被放大几十倍，可能制造畸形单元）。**禁止用全量平均**。

**正确做法（三分组聚类批量）**：
- 读一次全部结合边原值（一条查询拿全，不要逐条）
- 按值域分 3 组批量设置（**每组合并传一个代表性值**，每组 1 次调用，最多 3 次覆盖全部）：
  | 组 | 原值域 | 传参代表值 |
  |---|---|---|
  | 短过渡边 | < 1 | **0.3** |
  | 中段边 | 1 ~ 10 | **5** |
  | 长分布边 | > 10 | **15** |
- 分组后用 `UGReDimensionConfigDistribution` 每组合合调用一次（ids 为该组全部边），3 次调用覆盖全部结合边。
- **能逐条处理时不启用聚合计**（如结合边 ≤8 条，直接逐条传原值最准）。
- `note` 记录分组阈值与代表值（如 `junction: [0.3×12, 5×9, 15×17]`），供换弹体重算。

## 网格线分布设置

### 设置步骤

1. **获取结合处的网格线 ID**（弹翼与结合对象的共享边）：

```
# 例：弹翼(wing)与弹体(fuselage)的结合处（绕翼一圈 = 三段取并集）
# 🔴 第一步：一次调 GetAllSpitAssemblyGroupProperty() 拿全部分组，在返回的
#    list[{组名:{line:[...], domain:[{ids:[...]}]}}] 里按组名取各组的 domain[].ids。
#    ⚠️ 没有 GetGroupDomainIds 这类 helper 工具——那是历史文档的幻觉工具名。
groups = _parse(GetAllSpitAssemblyGroupProperty())   # 内层 result 是 JSON 字符串，需二次 json.loads
def _ids(name): return groups[name]["domain"][0]["ids"]

wingSideDomains = _ids("wingSideSurfaceN")    # N = 第几对弹翼，下同
wingLE_Domains = _ids("wingLeadingEdgeN")
wingTE_Domains = _ids("wingTrailingEdgeN")
fuselageDomains = _ids("fuselage")

# 🔴 批量调用：一次传全部 domain_ids，禁止 for 循环逐个查
wingAllConnectors = set()
for domains in [wingSideDomains, wingLE_Domains, wingTE_Domains]:
    if domains:  # wingLeadingEdgeN/wingTrailingEdgeN may be empty for some models
        result = GetConnectorsByDomains(domain_ids=domains)
        for d in result["domains"]:
            wingAllConnectors.update(d["connector_ids"])

fuselageConnectors = set()
result_f = GetConnectorsByDomains(domain_ids=fuselageDomains)
for d in result_f["domains"]:
    fuselageConnectors.update(d["connector_ids"])

junctionConnectors = wingAllConnectors & fuselageConnectors     # 三段并集 ∩ 弹体 = 绕翼一圈
```

2. **获取每条共享边的原分布值**（不修改两端间距；🔴 结合边多时按「聚合近似规则」读一次全量、按值域分 3 组，勿逐条）：

```
# 对每条结合处网格线，读取当前 headSpace 和 tailSpace
for cid in junctionConnectors:
    spacing = GetConnectorsStartAndEndUnitLenth(cid)      # → {"start": s0, "end": e0}
    # 保留原有间距值传入步骤 3；结合边 >8 条时按「聚合近似规则」分组取代表值
```

3. 调用 `UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)` 设置分布参数（只修改中间值，两端保持原分布）：

| 参数 | 值 |
|---|---|
| `ids` | 结合处共享边 ID（逗号分隔） |
| `headspace` | **不修改**（传原分布值） |
| `tailspace` | **不修改**（传原分布值） |
| `params` | `"1.2,50,1.2,50"`（严格格式 `"headRate,headLayer,tailRate,tailLayer"`） |
| `mindValue` | **`0.02 × 该对根弦长`**（`wings[N−1].root_chord`） |

> ⚠️ 该工具只有上述 5 个形参；没有 `disFunc`、`headRate` 等独立参数（增长率/层数打包进 `params`）。

4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution(ids, headspace, tailspace, params, mindValue)`（参数格式同 `UGReDimensionConfigDistribution`）。

5. 对于根部段（若存在），参照 `missile-chordwise-direction` Skill 的弦向分布做平滑衔接。

### 工具调用表

| 序号 | 工具 | 参数 | 说明 |
|------|------|------|------|
| 1 | `GetAllSpitAssemblyGroupProperty` | 无（解析 `["wingSideSurfaceN"]` 及 `["fuselage"]` 的 `domain[].ids`） | 获取结合处涉及的分组网格面 ID |
| 2 | `GetConnectorsByDomains` | `domain_ids`: **一次传全部面 ID（批量，禁止逐条循环）** | 获取面的网格线集合 |
| 3 | `GetConnectorsStartAndEndUnitLenth` | `connector_ids`: 每条共享边 ID | 获取原有两端间距（保留不修改） |
| 4 | `UGReDimensionConfigDistribution` | `ids`: 结合边 ID; `headspace`: 原值; `tailspace`: 原值; `params`: `"1.2,50,1.2,50"`; `mindValue`: `0.02 × 该对根弦长` | 设置增长分布（不修改两端） |
| 5 | `UGReDimensionSmoothDistribution` | `ids`: 结合边 ID; `headspace`/`tailspace`: 原值; `params`: `"1.2,50,1.2,50"`; `mindValue`: `0.02 × 该对根弦长` | 平滑过渡到相邻区域 |

### 注意事项

- **严禁修改两端分布**——本位置的核心约束。
- **双曲正切分布参数统一**：增长率 1.2、层数最大 50、分布类型 0。
- 🔴 **禁止全量取平均**（见「聚合近似规则」）；结合边多时用三分组聚类。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`，且两端分布未被修改。
- 根部段（若存在）平滑过渡无误。
- 任一步骤失败时停止，不继续后续步骤。