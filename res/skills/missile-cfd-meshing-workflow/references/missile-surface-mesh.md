# 导弹表面网格参数

导弹专用表面网格参数体系。

## 全局设置

- **工具**：`UGSur(type, ids, targetSize, minSize, adaptAngle, way)` `[复用]` 或 `GetGenerateSurMeshDefaultParam()` `[复用]` 获取默认值
- 全局使用默认尺寸，具体以用户输入为准

## 部件网格参数表

Agent 侧根据几何参数计算后传入。

| 部件 | 目标尺寸 | 最小尺寸 | 自适应角 |
|------|---------|---------|---------|
| 头部（球头） | min(0.01×D, 0.25×R) | 目标×1/10 | 10° |
| 头部（尖角） | 0.005×D | 目标×1/10 | 10° |
| 尾部 | 0.05×D | 目标×1/10 | 10° |
| 弹体 | 0.05×D | 目标×1/10 | 10° |
| 翼 | 0.02×**该对** C_root（1/50），全局/前缘/后缘统一 | 目标×1/10 | 10° |
| 外场 | 按场景计算 | 目标×1/10 | 10° |

> 部件行按**实际存在的分组**取用——`GetAllSpitAssemblyGroupProperty` 里有哪组就配哪组，无舵/舵轴即无对应行，不写死型号。
> 弹体二面角加密见下节；翼前缘、梢部除前缘加密外，流向全局尺寸按当前翼，后续由各向异性加密。
>
> 🔴 **翼子面尺寸规则**：
> - **表面网格阶段：按对取用** —— 第 N 对翼的全部子面（`wingLeadingEdgeN` / `wingSideSurfaceN` / `wingTrailingEdgeN` / `wingTipN`）统一用 **`0.02 × C_root(N)`**，其中 `C_root(N)` 取 `wings[N−1].root_chord`（**接口值，现算**），不区分前缘/后缘/梢部/拐角——表面网格阶段不看"拐角"概念；
> - 🔴 **不得用某一对的 C_root 代跑其他对**：各对弦长可差 5 倍以上，混用会让某一对翼面显著偏疏或偏密（**数值随型号变，一律现取接口值，本文件不记**）；
> - 🔴 **前缘面 ≠ 拐角**：前缘面（wingLeadingEdgeN）是整个翼前缘的面，尺寸同该对翼一致（0.02×C_root(N)），只有前缘与弹体/后缘/翼梢交汇的局部小区域才是"拐角";
> - **各向异性阶段**才用 `0.005 × 当地弦长`（**加密尺寸**，现算）做**全部位置**（窄面/侧面/结合处/弹尾）的首层加密；**窄面增长率 1、侧面·结合处·弹尾 1.2**，层数统一 50，中间值＝翼面尺寸；完整表见 `missile-anisotropic-mesh.md` §「各位置参数」；
> - 判定单选题：**表面网格阶段一律 `0.02×C_root(N)`**，Agent 不得在两个系数间裁决。

## 符号说明

| 符号 | 参数名（后端字段） | 来源 |
|------|--------|------|
| D | `body_diameter` | 🔴 **取 `GetMissileDimensions` 接口值**（头锥/弹体交接处口径，事实中心 §2） |
| **D_surface** | `body_diameter_surface` | 🔴 **表面网格非球头部分用的口径**——`\|D − D_tail\| > 0.7×D_tail` 时为弹尾口径，否则 = D（事实中心 §2.2.1） |
| D_tail | `body_diameter_tail` | 弹尾口径，失配判据来源（事实中心 M4） |
| R | `nose_radius` | `GetMissileDimensions` 返回（尖头时为 0）；单位 mm |
| 头部类型 | 由 `nose_radius` 判定 | `> 0` 为球头(ball)，`== 0` 为尖头(sharp) |
| 弹翼当地弦长（第 N 对） | 🔴 **`C_root(N)` = `wings[N−1].root_chord`**，取接口值（事实中心 §2）；🔴 C_tip **按翼型分叉**（M2）：三角翼（无 `wingTipN`）≈0 → **禁止 0 代入**，梢部端间距钳制到该对 C_root 计算值（= `0.005×C_root(N)` / `0.02×C_root(N)`，现算）；梯形翼（有 `wingTipN`）按接口值正常使用 | `GetMissileDimensions` 返回的 `wings` 数组 / 用户输入 |
| 半翼展 b/2（第 N 对） | 🔴 **取接口值** `wings[N−1].half_span`（事实中心 §2） | 接口返回 / 用户输入 |

> 🔴 **弹径选用（2026-10-14 用户裁定，事实中心 §2.2.1）**：表面网格的**非球头部分**用 `body_diameter_surface`（有效弹径）；🔴 **球头公式是例外**——仍用**原始 `body_diameter` + `nose_radius`**，即 `min(0.01×D, 0.25×R)`（尖头 `0.005×D`）。用户原话："除了球头公式，弹径 D 就用弹尾口径替代来生成更合理。球头的网格生成还是用原本的弹径和球头半径参数公式计算。"

> 几何字段口径：弹翼**按对**取 `wings[N−1].root_chord` / `wings[N−1].tip_chord`——**接口返回哪些 `*_chord` 字段就用哪些**，不预设部件种类。上方标量键 `wing_root_chord` / `wing_tip_chord` / `wing_half_span` 即**第 1 对**（= `wings[0]`），只作单对翼时的便捷入口，多对翼一律走 `wings` 数组。

> ⚠️ **上表参数只能来自 `GetMissileDimensions` 或用户直接输入**。**禁止由分组属性 `targetSize`/`minSize` 反推 D / R / 弦长**：分组属性只用于生成 `groupProperty` 传参，不作几何量来源（详见 `missile-geometry-parameters.md`）。取值分两类，不得混用：
>
> 1. **尺寸字段**（`body_diameter` / `body_diameter_tail` / `fuselage_length` / `nose_radius` / `wings[].root_chord` / `wings[].half_span`）→ 🔴 **一律以接口返回值为准**（事实中心 §2，2026-10-14 起**无固定值覆盖**）；不得替换、反推或自行估算；明显不符时原样使用并在 `note` 记录、报告后端。
> 2. **C_tip** → 🔴 **按翼型分叉**（事实中心 M2）：三角翼（无 `wingTipN`）≈0 属正常，**禁止 0 代入**；梯形翼（有 `wingTipN`）按 `wings[N−1].tip_chord` 正常使用。梢部端间距规则见上表与 `missile-geometry-parameters.md`「当地弦长使用规则」。
>
> 🔴 **单位**：接口返值**本就是 mm**（几何坐标系即为 mm，**后端不做任何换算**）——**直接传参，禁止 ×1000**。
>
> `GetMissileDimensions` 返回恒定 `"true"`（兜底值）时：**向用户索取 D / R / 弦长**；**不得用估算值冒充**，也不得反复重试该接口。本阶段若只缺分部件精度要求，可用 `GetGenerateSurMeshDefaultParam` 的全局默认值（`targetSize`/`minSize`/`adaptAngle`）继续；但外场创建等硬依赖见 `missile-farfield-and-volume.md` §6.2。

## 执行步骤：根据头部类型计算头部网格参数

**必须先调 `GetMissileDimensions` 获取参数，再按 `nose_radius` 判断头部类型并分支计算头部目标尺寸：**

```
params = GetMissileDimensions()      # 🔴 返回值单位本就是 mm，直接使用（后端不做换算）
D        = params["body_diameter"]        # 🔴 接口值（头锥/弹体交接处口径）
D_surf   = params["body_diameter_surface"]  # 🔴 表面网格用口径（非球头部分）
D_tail   = params["body_diameter_tail"]     # 弹尾口径
R        = params["nose_radius"]          # 🔴 本就是 mm，禁止再 ×1000

# 🔴 弹翼按对取（下标 0 = 第 1 对）：数组长度 = 后端已填的对数，
#    模型实际对数由分组探测（见 missile-segmentation.md）；取不到的对回退第 1 对
wings    = params["wings"]                # [{"root_chord":C_root1,"tip_chord":C_tip1,"half_span":b2_1}, ...]
for i, w in enumerate(wings):             # i = 0,1,2 → 第 1/2/3 对
    C_root_N = w["root_chord"]            # 🔴 该对翼面尺寸基准（0.02×）
    C_tip_N  = w["tip_chord"]             # 🔴 三角翼≈0 属正常（M2），禁止 0 代入、梢部端间距一律以 C_root_N 计
    b_2_N    = w["half_span"]             # 展向分布跨度（仅此一对）
# ⚠️ 单对翼时可直接用标量键 wing_root_chord / wing_tip_chord / wing_half_span（= wings[0]）

# 🔴 表面网格有效弹径（后端已给，缺失时自判）：
#     |D − D_tail| > 0.7 × D_tail  → 表面网格非球头部分用 D_tail
D_surface = D_surf if D_surf > 0 else (
    D_tail if (D_tail > 0 and abs(D - D_tail) > 0.7 * D_tail) else D)

if R > 0:   # 球头(ball)
    noseTargetSize = min(0.01 * D, 0.25 * R)   # 🔴 例外：球头用【原始 D】+ R
else:       # 尖头(sharp)，R == 0
    noseTargetSize = 0.005 * D

noseMinSize = noseTargetSize * 0.1
```

> 🔴 **弹翼尺寸一律用接口 C_root(N)，按对取**：不得把弹翼降级为"全局默认尺寸"，必须用 `0.02×C_root(N)` / `0.005×C_root(N)` 计算（**现算**，无固定常量）；🔴 **不得用某一对的 C_root 代跑其他对**。

**其他部件直接计算（不依赖头部类型）：**

| 部件 | 目标尺寸 | 最小尺寸 |
|------|---------|---------|
| 尾部 | 0.05 × **D_surface** | 目标 × 0.1 |
| 弹体 | 0.05 × **D_surface** | 目标 × 0.1 |
| 弹翼（第 N 对） | 0.02 × **`wings[N−1].root_chord`** | 目标 × 0.1 |

> 🔴 **头锥（球头/尖角）用原始 D，其余用 D_surface**：用户裁定球头公式例外（见上）。`D_surface` 在弹径与弹尾口径失配 >70% 时等于弹尾口径。

> 🔴 **本阶段的"当地弦长"取该对的 C_root（根部弦长）** —— "当地弦长"定义为**根部**前缘到尾缘的距离。此阶段**不分段**（分组是 `wingSideSurfaceN` 等整块子面，且表面网格阶段不看"拐角"概念），无法按展向分段设不同尺寸；若误取 C_tip，整个翼面会按最小弦长加密（同一对翼的 C_root/C_tip 可差 5 倍以上），网格显著偏密。

**全局参数：** 自适应角统一为 10°。

**传入工具：** 各部件参数拼装后调用 `GenerateSurMeshBySpitAssemblyGroupProperty`：

| 参数 | 值 |
|------|-----|
| `ids` | `"0"`（所有超面）或指定面 ID 字符串 |
| `targetSize` / `minSize` | 全局（兜底）尺寸 —— **取各组计算值中的极值**：`targetSize` = max(各组 targetSize)，`minSize` = min(各组 minSize)。若各组尺寸尚未计算（`GetMissileDimensions` 返回兜底值时），再降级到 `GetGenerateSurMeshDefaultParam()` 的返回值。按部件区分的尺寸一律通过 `groupProperty` 里各组的 **`domain[]` 和 `line[]` 的 `.targetSize` / `.minSize`** 设置（**两者必须同值**） |
| `adaptAngle` | `10` |
| `way` | `0`（组合法），可选：`0`=组合法 `1`=狭长面 `2`=四边形占优 |
| `groupProperty` | **`GetAllSpitAssemblyGroupProperty()` 返回的 JSON 字符串原样回传**（工具 docstring 明示）。如需按部件区分尺寸，改动各组 `domain[]` **和 `line[]`** 两条的 `targetSize` / `minSize` / `angle`，其余结构与键名保持原样；🔴 **`ids` / 组名 / 键名一律不得改动，`line` 条目不得删除**（但 `line[].targetSize`/`minSize` **必须改**，见下方 🔴 说明） |

> 🔴 **groupProperty 修改纪律**：
> 1. **先存档原始返回值**：`GetAllSpitAssemblyGroupProperty()` 的返回值是唯一真相，先完整保存（原样粘贴到当轮对话/写盘），**后续只在此副本上改**；
> 2. **改 6 个字段**：`domain[]` **和 `line[]`** 每条的 `targetSize` / `minSize` / `angle`（同一组内 `domain` 与 `line` 必须保持一致，同一条数组内多条也保持一致），**`ids` / 组名 / 键名一律不动**；
> 3. **禁止凭记忆重建整个 JSON**（易抄错 ids）——改完直接回传存档副本；
> 4. **残留属性覆盖**：分组属性里已有的 `targetSize`/`minSize`（可能是上次会话残留旧值）**仅作格式参考，一律以公式计算值为准覆盖**，不得沿用、不得追问"旧值怎么来的"。
>
> 🔴 **为什么 `line[]` 必须改，不能留 0（2026-10-19 三次实机卡死复盘）**：
> `GetAllSpitAssemblyGroupProperty` 返回的 `line[].targetSize`/`minSize` **恒为 0**（GUI 新建 EDGE 组的默认值），且 `line[].ids` **非空**（是真实网格线 ID 列表，不是空数组）。
> 若照旧纪律"只改 domain、line 不动"，0 会被后端原样写入网格线尺寸 → 网格生成时**永久卡死**，工具**永不返回**（既不报错也不失败，只能由用户手动中断本轮对话）。
> 实测三次卡死（2026-10-18 ×2、2026-10-19 ×1，**跨不同导弹型号**），唯一共同点即 `line[].targetSize=0 / minSize=0 且 ids 非空`；同一型号、同一分组 id 的两次调用中，**line 填了正确尺寸的那次秒过**。
> 因此：**`line[]` 的 `targetSize`/`minSize` 必须与同组 `domain[]` 填成同一组计算值**（`angle` 同为 10）。这与飞机侧 `cfd-meshing-workflow/references/part-based-surface-mesh.md` §4.2「domain 和 line 均更新」一致。

### `groupProperty` 数据结构

`GetAllSpitAssemblyGroupProperty()` 的 `result` 是一个 **JSON 字符串**，需二次 `json.loads` 后使用（外层 HTTP 响应为 `{"result": "<该字符串>"}`）。解析后结构：

```json
[ {"组名": {"domain": [{"angle":10, "ids":[3,28,29,30], "minSize":3.6, "targetSize":36.0}, ...],
            "line":   [{"angle":10, "ids":[114,115,116], "minSize":3.6, "targetSize":36.0}, ...] }},
  {"另一组名": {...}} ]
```

> ⚠️ 上例数值**仅示意结构，必须按公式现算**，本文件不记具体值；注意 `line` 的尺寸**与同组 `domain` 相同、且不为 0**。
> 🔴 **`line[].ids` 实测为非空**（真实网格线 ID 列表）——本节早前版本写作"`ids:[]`"是早期后端的旧行为，**现已不成立**；照旧写法会使 Agent 误判 `line` 无关而保留 0，直接导致生成卡死（见「groupProperty 修改纪律」🔴）。
> 🔴 **`line[].targetSize`/`minSize` 返回时恒为 0**，**必须**改成与同组 `domain[]` 相同的计算值——**"原始返回值原样回传"仅适用于 `ids` 与结构，不适用于 `line` 的尺寸字段**。

- **取面 ID**：`domain[].ids`。🔴 **但 `line[].ids` 非空且必须保留**——它承载真实网格线（超边）ID，后端按它给各分组边界设尺寸；**不得删除 `line` 条目、不得清空 `line[].ids`、更不得把 `line[].targetSize`/`minSize` 留作 0**。
- **`domain` 数组条目数可能 >1**——取**任一** `domain[0].ids`（或并集）结果相同。
- **按部件设置尺寸**：把要改的那个组的 `domain[]` **和 `line[]`** 中每条 JSON 的 `targetSize` / `minSize` / `angle` 同步改成该部件的计算值（同组内 `domain` 与 `line` 取同一组值，多条需保持一致）。🔴 **只在存档副本上改这几个字段，禁止重建整个 JSON**（见「groupProperty 修改纪律」）。
- 🔴 **弹翼组按序号取该对的 C_root**：`wingLeadingEdgeN` / `wingSideSurfaceN` / `wingTrailingEdgeN` / `wingTipN` 一律用 **`0.02 × wings[N−1].root_chord`**（N 从组名末位数字读出，**不是**统一用第 1 对）。**混用会让某一对偏疏或偏密**。数值一律现算，本文件不记具体值。
- ⚠️ **组名可能乱码**：当前工程可能返回 2 个分组名返回为 `U+FFFD` 乱码（后端编码问题，不可逆）。**遇到非 ASCII 且无法匹配预期组名的键时，不要按名匹配、不要跳过**——按 `domain[].ids` 的面 ID 归属判断，并在 `note` 中记录，提示用户核查后端编码。
- 🔴 **两级结构**：`SpitAssembly` 的分组是两级结构——但🔴 **后端只产出带序号子组名（`wingLeadingEdgeN` / `wingSideSurfaceN` / `wingTrailingEdgeN`），不存在 `wing` 父容器组**（事实中心 M1）。子组自身即持有实际域 ID。构造 groupProperty 时，**删除 `domain[].ids` 为空数组（`[]`）的组**（可能返回的空域占位组），只保留有域的组。`fuselage` / `nose` / `tail` 无子组，直接保留。**传空域组给 `GenerateSurMeshBySpitAssemblyGroupProperty` 会导致工具收到无域组后静默丢弃全部输入，返回 `true` 但实际未生成任何网格。**


## 生成后：不检查，直接进入阶段 7

🔴 **`GenerateSurMeshBySpitAssemblyGroupProperty` 返回成功即为本阶段结束**——**不做生成后验收**（不查网格规模、不估单元数、不做密度量级自检、不预先做逐面封闭性统计）。这些检查在此处**既不产生决策、又纯耗轮次**，已全部删除。生成调用返回后**直接进入阶段 7（各向异性）**。

> 唯一例外：**用户明确要求**统计网格规模时才查（此时用 `GetPointCount` 抽样即可），属用户驱动的临时动作，不是本阶段的必经步骤。

**封闭性不在此处判定**：统一由阶段 8 的「建场试金石」完成——`UGBlockCreate` 返回 `"true"` = 封闭；返回 `"false"` 且报「不封闭的网格线ID如下」= 不封闭，清单以后端报出的为准（见 `references/missile-mesh-sealing-check.md` §6.0）。**届时若不合格**，不要继续各向异性之后的空间网格，转入该排查手册。

> 🔴 **"网格偏密"与"网格不封闭"常常同源**：CAD 面被切得过碎时，碎片边界在网格里既是**开口边界**（→ 不封闭），又会被 `adaptAngle` 判为"曲率超限"而向各组 `minSize` 加密（→ 偏密）。**这种情况靠调 `targetSize` / `minSize` 无解** —— 必须先处理 CAD 碎片（碎面合并 / 回到源头数模），见 `references/missile-mesh-sealing-check.md`「CAD 拓扑体检」。

## 补充加密规则

### 弹体二面角夹角处加密（头部锥体与弹体交接部位）

**触发条件：** 弹体上存在明显的二面角夹角，一般发生在头部锥体与弹体的交接部位。

**加密尺寸：** 0.002 × **D_surface**（表面网格有效弹径；后端 `body_diameter_surface` 字段，失配 >70% 时为弹尾口径——事实中心 §2.2.1）。

### 翼拐角加密

拐角指翼面上曲率突变的结合部位，共 **4 处**：**① 翼前缘 ② 翼梢 ③ 翼后缘 ④ 弹体结合处**。加密尺寸 `0.005 × 当地弦长`，厚度方向网格点数 ≥ 7。

| 翼型（按 M2 探测） | 翼梢（②）的表现形式 |
|------|------|
| 三角翼（无 `wingTipN`） | 翼梢 = 前缘与后缘汇聚的**尖点**（仍算一处拐角） |
| 梯形翼（有 `wingTipN`） | 翼梢 = 真实**梢部端面**（`wingTipN` 面） |

> 🔴 **拐角加密尺寸用「当地弦长」，不是笼统的整对 C_root**：C_root(N) 只在**整块子面无法按展向分段时**才作为"当地弦长"的代用值（见上方「本阶段的"当地弦长"取该对的 C_root」段，那条只约束**表面网格阶段**）。拐角加密属各向异性阶段，能分段就分段：梯形翼按该段弦长（含 C_tip(N)）；三角翼按 `C_local = C_root(N) × (1 − x/(b/2)(N))` 插值，尖点处按事实中心 §2.2.2 钳制、**不取 0**。🔴 **弦长一律取该对**（`wings[N−1]`），不得各对共用。
>
> 拐角加密在**各向异性阶段**实现，不在表面网格阶段单独设参数：**窄面（前缘 ① / 翼梢 ② / 后缘 ③）首层高度＝加密尺寸、增长率 1；侧面与翼身结合处（④）首层高度＝加密尺寸、增长率 1.2**；层数统一 50、中间值＝翼面尺寸。完整参数表见 `references/missile-anisotropic-mesh.md` §「各位置参数」。

**定位方法（找到 nose 与 fuselage 的共享边）：**

```
# 1. 获取 nose 和 fuselage 分组的网格面 ID
#    唯一数据源：GetAllSpitAssemblyGroupProperty() 返回 list[{组名:{line:[...], domain:[{ids:[...]}]}}]
#    按组名取 domain[].ids 即该组网格面 ID。⚠️ 没有 GetGroupDomainIds 这类 helper 工具（历史幻觉工具名）
groups = _parse(GetAllSpitAssemblyGroupProperty())   # 内层 result 为 JSON 字符串，需二次 json.loads
noseDomains = groups["nose"]["domain"][0]["ids"]           # → [1, 2, ...]
fuselageDomains = groups["fuselage"]["domain"][0]["ids"]   # → [3, 4, ...]

# 2. 获取两组的网格线集合（🔴 批量调用，主 Skill 纪律第 3 条：一次传全部 ID，禁止 for 逐个查）
noseConnectors = set()
for d in GetConnectorsByDomains(domain_ids=noseDomains)["domains"]:      # → {"domains":[{"domain_id":id,"connector_ids":[...]}, ...]}
    noseConnectors.update(d["connector_ids"])

fuselageConnectors = set()
for d in GetConnectorsByDomains(domain_ids=fuselageDomains)["domains"]:
    fuselageConnectors.update(d["connector_ids"])

# 3. 取交集 → nose 与 fuselage 的共享边（即头部锥体-弹体交接线）
junctionConnectors = noseConnectors & fuselageConnectors

# 4. 对全部结合边设置分布（不修改两端间距，仅通过增长率控制内部点向 0.002×D 过渡）
# 🔴 真实 Schema 只有 5 个参数：UGReDimensionConfigDistribution(ids, headspace, tailspace, params, mindValue)
#    增长率/层数打包进 params 字符串；不存在 headSpace/headRate/headLayer/tailRate/tailLayer/midValue/disFunc
#    等独立参数——历史文档的 7 参数写法是幻觉调用，照抄会被 Schema 拒绝。
# 🔴 批量：ids 一次传全部结合边（禁止 for 逐条循环，主 Skill 纪律第 3 条）
UGReDimensionConfigDistribution(
    ids=",".join(str(c) for c in junctionConnectors),   # 逗号分隔字符串
    headspace=原值,          # 不修改，先由 GetConnectorsStartAndEndUnitLenth 读原值
    tailspace=原值,          # 不修改
    params="1.2,50,1.2,50",  # 严格格式 "headRate,headLayer,tailRate,tailLayer"（增长率 1.2、层数 50）
    mindValue=0.002 * D_surface   # 🔴 表面网格有效弹径（失配 >70% 时为弹尾口径）
)
```

> **注意：** 此加密在各向异性阶段（`missile-anisotropic-mesh.md`）的翼身结合处处理中执行，不要求在表面网格生成阶段就设好。参见 `references/missile-anisotropic-mesh.md`。（翼身结合处的分步细节在独立 Skill `missile-root-junction`，**仅在各向异性一站式失败回退时加载**，不作常规路由。）

### 翼前缘和梢部

- 🔴 **翼前缘厚度方向点数 ≥ 7**（需求硬指标）——由各向异性阶段的"加密尺寸首层 + 增长率 1 + 层数 50"保证，见 `missile-anisotropic-mesh.md` §「各位置参数」。
- 前缘 / 梢部**弧线**本身的表面网格分辨率同样 ≥ 7 点。

## 工具

| 工具 | 用途 | 类型 |
|------|------|------|
| `UGSur` | 全局表面网格 | [复用] |
| `GetGenerateSurMeshDefaultParam` | 获取默认参数 | [复用] |
| `GenerateSurMeshBySpitAssemblyGroupProperty` | 按部件组生成表面网格 | [复用] |

## 前置条件

1. 水密性处理已完成（自由边检查通过）
2. 导弹 AI 部件分割（`ClassifyMissile`）已完成
3. 碎面合并（**条件触发**：用户提到碎面时执行；未提及时本项不阻塞表面网格生成）
4. 导弹几何尺寸参数已获取（`GetMissileDimensions` 或用户直接提供）
5. 上述任一条件不满足时停止
