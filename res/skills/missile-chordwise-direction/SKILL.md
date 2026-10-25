---
name: missile-chordwise-direction
description: 【回退专用】导弹翼窄面（前缘/后缘/翼梢）各向异性网格线分布处理。窄面首层高度用 0.1×该对翼厚度，增长率 1，层数 50。侧面增长率 1.2。适用于弹翼(wing)。🔴 仅在 `MissileWingAnisoProcessWingAnisotropy` 一站式返回 `status="failed"` 时由 `missile-anisotropy-processing` 自动回退加载（映射见 `missile-anisotropic-mesh.md` §「分步回退」）；一站式正常时**不得**主动加载本 Skill。
aliases: [翼窄面分布, 窄面加密, wing narrow-face, 前缘加密, 后缘加密, 翼梢加密]
tags: [CFD, 网格, 各向异性, 弦向, 导弹, 翼]
category: CFD
version: 2.2.0
author: Tsolodancer
allowed-tools: []
---

# 导弹翼窄面各向异性网格线分布

## 适用位置

- 导弹翼**窄面**：前缘（`wingLeadingEdgeN`）、后缘（`wingTrailingEdgeN`）、翼梢（`wingTipN`，**仅梯形翼存在，按 M2 探测**）
- 增长率 1，层数 50，首层高度 = **`0.1 × 该对翼厚度`**（`wings[N−1].thickness`）
- 部件：弹翼(`wing`)
- 🔴 **侧面（`wingSideSurfaceN`）不在此 Skill 范围**——侧面增长率 1.2，见 `missile-anisotropic-mesh.md`

## 前置条件

- 已通过 `GetMissileDimensions` 获取弦长与厚度参数（🔴 接口返回**毫米 mm**，直接用，禁止 ×1000）
- 弹翼按对：🔴 **C_root(N) 取 `GetMissileDimensions` 接口值 `wings[N−1].root_chord`、厚度取 `wings[N−1].thickness`**（事实中心 §2，2026-10-14 起**无固定值**；N 由组名末位数字读出，**不得各对共用**）；🔴 C_tip(N) **按对取 `wings[N−1].tip_chord`，并按翼型分叉**（M2，先探 `wingTipN` 是否存在）：**三角翼**（无 `wingTipN`）≈0 正常，**禁止 0 代入，梢部端间距钳制到该对 C_root(N) 计算值（= `0.02×C_root(N)`，现算，事实中心 §2.2.2）**；**梯形翼**（有 `wingTipN`）按接口 C_tip(N) 正常使用
- 已获取翼面尺寸（中间值用）
- 表面网格已生成
- **链路接口可用**：`GetConnectorsByDomains` 能返回真实网格线 ID（若返回兜底值，整个各向异性阶段按主 Skill「工具可用性判定」标记 `skipped`）
- 🔴 **定位方式（按子组名）**：弹翼**带序号子面分组**（`wingLeadingEdgeN` / `wingSideSurfaceN`，N = 第几对弹翼）即定位键，经 `GetAllSpitAssemblyGroupProperty` 按组名取 `domain[].ids`。🔴 碎面合并会改超面集合，**进入本阶段前重查一次分组**（事实中心 G2）。子组名缺失 → 按「不强行推理纪律」`skipped` + `note`。

## 部件分组名对照

| 部件类型 | 子面前缀 | 侧面 | 后缘面 | 梢面 |
|---------|---------|------|--------|------|
| 弹翼(wing) | `wing` | `wingSideSurfaceN` | `wingTrailingEdgeN` | `wingTipN`（**按 M2 探测**：存在=梯形翼；缺失=三角翼，属正常，后缘/梢部按自由梢端处置） |

> 🔴 **N = 第几对弹翼（1 / 2 / 3）**：后端只产出带序号子组名，**不存在 `wing` 主组**——子组名即定位键，**无需归并**。
> 弹翼对数自动检测：查 `wingSideSurface1` ~ `wingSideSurface3` 哪些存在。

## 尺寸参数

| 参数 | 值 |
|---|---|
| 前缘端间距（首层高度） | **`0.1 × 该对翼厚度`**（`wings[N−1].thickness`） |
| 后缘端间距（尾端高度） | **`0.1 × 该对翼厚度`**（同上） |
| 增长率（headRate / tailRate） | 1（窄面专用） |
| 层数（headLayer / tailLayer） | 50（默认） |
| 中间值（mindValue） | **`0.02 × 该对根弦长`**（`wings[N−1].root_chord`） |

> 🔴 **2026-10-22 口径变更**：旧口径 `0.005 × 当地弦长`「加密尺寸」**已作废**，弹翼一律改用**厚度**基准。
>
> ⚠️ 上表是**语义值**，不是参数名。调用时的真实形参只有 5 个：
> `UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)`——
> 增长率与层数（headRate/headLayer/tailRate/tailLayer）**打包进 `params` 字符串**
> （即 `"1,50,1,50"`）；**没有** `headRate` / `headLayer` / `disFunc` 等独立形参。
> 分布类型由所选工具决定（Config / Smooth），不由 `disFunc` 参数决定。

## 网格线分布设置

### 设置步骤

1. **获取弦向网格线 ID**：根据当前处理的部件类型，用对应分组前缀获取侧面的弦向网格线：

```
# 例：弹翼(wing)侧面，N = 第几对弹翼
# 🔴 第一步：调 GetAllSpitAssemblyGroupProperty() 拿全部分组属性，在返回的
#    list[{组名:{line:[...], domain:[{ids:[...]}]}}] 里按组名取 domain[].ids。
#    ⚠️ 没有 GetGroupDomainIds 这类 helper 工具——那是历史文档的幻觉工具名。
groups = _parse(GetAllSpitAssemblyGroupProperty())         # 内层 result 是 JSON 字符串，需二次 json.loads
groupId = groups["wingSideSurfaceN"]["domain"][0]["ids"]   # 该组网格面 ID 列表
# 🔴 批量调用（主 Skill 上下文治理纪律第 3 条）：一次传全部 domain_ids，禁止 for 循环逐个查
result = GetConnectorsByDomains(domain_ids=groupId)   # → {"domains":[{"domain_id":id,"connector_ids":[...]}, ...]}
chordwiseConnectors = []
for d in result["domains"]:                            # 解析时循环合并，不是调用时循环
    chordwiseConnectors.extend(d["connector_ids"])
```

2. 调用 `UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)` 设置分布参数：

| 参数 | 值 |
|---|---|
| `ids` | 弦向网格线 ID（逗号分隔） |
| `headspace` | **`0.1 × 该对翼厚度`**（`wings[N−1].thickness`） |
| `tailspace` | **`0.1 × 该对翼厚度`**（同上） |
| `params` | `"1,50,1,50"`（严格格式 `"headRate,headLayer,tailRate,tailLayer"`） |
| `mindValue` | **`0.02 × 该对根弦长`**（`wings[N−1].root_chord`） |

3. 如需对边匹配，调用 `UGReDimensionMatch(ids, targetId)`。
4. 如需平滑过渡到相邻区域，调用 `UGReDimensionSmoothDistribution(ids, headspace, tailspace, params, mindValue)`（参数格式同 `UGReDimensionConfigDistribution`，同样只有 5 个形参）。

### 注意事项

- 弦向分布**只控制内部点分布**，不更改边界分布。
- 🔴 **首层/端间距一律 `0.1×该对厚度`，与弦长无关**——三角翼/梯形翼的首层高度完全相同。若根部和梢部的当地弦长不同（`C_root(N)` ≠ `C_tip(N)`，**都按该对取**），**只有中间值 `mindValue` 需沿展向分段**。🔴 **三角翼**（无 `wingTipN`，M2）：C_tip(N)≈0 → **不分段**，中间值一律用该对 C_root(N) 计算值（`0.02×C_root(N)`，**现算**）。**梯形翼**（有 `wingTipN`）：按 C_tip(N) 实际值分段计算。🔴 **各对独立**：`wingLeadingEdge2` 用 `wings[1]`，不得沿用第 1 对的弦长/厚度。
- 前缘点数 ≥ 7，梢部点数 ≥ 7。

## 完成标准

- `UGReDimensionConfigDistribution` 调用返回 `success` 为 `true`。
- 对边匹配无误。
- 任一步骤失败时停止，不继续后续步骤。