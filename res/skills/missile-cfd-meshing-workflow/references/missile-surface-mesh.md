# 导弹表面网格参数

导弹专用表面网格参数体系。对应 需求文件 模块 3。

## 全局设置

- **工具**：`UGSur(type, ids, targetSize, minSize, adaptAngle, way)` `[复用]` 或 `GetGenerateSurMeshDefaultParam()` `[复用]` 获取默认值
- 全局使用默认尺寸，具体以用户输入为准

## 部件网格参数表

Agent 侧根据几何参数计算后传入。

| 部件 | 目标尺寸 | 最小尺寸 | 自适应角 | 公式来源 |
|------|---------|---------|---------|---------|
| 头部（球头） | min(0.01×D, 0.25×R) | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 头部（尖角） | 0.005×D | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 尾部 | 0.05×D | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 弹体 | 0.05×D | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 翼/舵 | ≤0.03×当地弦长，默认 0.02× | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 翼/舵前缘、梢部 | ≤0.008×当地弦长，点数≥7 | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| **舵轴**（`finshaft`） | ≤0.008×当地弦长，**点数≥11** | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 舵根部面 | ≤0.008×当地弦长，点数≥7 | 目标×1/10 | 10° | 需求文件 模块 3.2 |
| 外场 | 按场景计算 | 目标×1/10 | 10° | 需求文件 模块 3.2 |

> **需求文件 3.2 补充说明**：舵根部对应的弹体部位要求与舵根部网格密度保持一致（需求文件 注明"不易实现，建议先不考虑舵"）；弹体二面角加密见下节；翼/舵前缘、梢部、尾缘除前缘加密外，流向全局尺寸按当前翼/舵，后续由各向异性加密。

## 符号说明

| 符号 | 参数名（后端字段） | 来源 |
|------|--------|------|
| D | `body_diameter` | `GetMissileDimensions` 返回 |
| R | `nose_radius` | `GetMissileDimensions` 返回（尖头时为 0） |
| 头部类型 | 由 `nose_radius` 判定 | `> 0` 为球头(ball)，`== 0` 为尖头(sharp) |
| 弹翼当地弦长 | `wing_root_chord`（根部）/ `wing_tip_chord`（梢部） | `GetMissileDimensions` 返回 |
| 舵当地弦长 | `fin_root_chord`（根部）/ `fin_tip_chord`（梢部） | `GetMissileDimensions` 返回 |

> 几何字段口径：`wing_root_chord` / `wing_tip_chord` 供弹翼，`fin_root_chord` / `fin_tip_chord` 供舵，**不可混用**。

> ⚠️ **上表参数只能来自 `GetMissileDimensions` 或用户直接输入**。**禁止由分组属性 `targetSize`/`minSize` 反推 D / R / 弦长**：分组属性只用于生成 `groupProperty` 传参，不作几何量来源。详见 `references/missile-geometry-parameters.md`。
>
> 🔴 **几何量一律以后端 `GetMissileDimensions()` 的返回值为准，Agent 不得自行替换、换算或反推**（含 `body_diameter` / `nose_radius` / `wing_*_chord` / `fin_*_chord` / `fuselage_length`）。
> 若某返回值与模型几何或同批其他字段明显不符（例如 `body_diameter` 与 `body_diameter_tail` 相差数倍）—— **不要自行改用另一字段，也不要按几何反推**。正确做法：**原样使用该返回值**，在阶段 `note` 中记录该不一致，并**向用户 / 后端报告确认**；确认后再按确认值执行。
>
> `GetMissileDimensions` 返回恒定 `"true"`（兜底值）时：**向用户索取 D / R / 弦长**（需求文件 2.1 认可用户输入）；**不得用估算值冒充**，也不得反复重试该接口。本阶段若只缺分部件精度要求，可用 `GetGenerateSurMeshDefaultParam` 的全局默认值（`targetSize`/`minSize`/`adaptAngle`）继续；但外场创建等硬依赖见 `missile-farfield-and-volume.md` §6.2。

## 执行步骤：根据头部类型计算头部网格参数

**必须先调 `GetMissileDimensions` 获取参数，再按 `nose_radius` 判断头部类型并分支计算头部目标尺寸：**

```
params = GetMissileDimensions()
D = params["body_diameter"]
R = params["nose_radius"]

if R > 0:   # 球头(ball)
    noseTargetSize = min(0.01 * D, 0.25 * R)
else:       # 尖头(sharp)，R == 0
    noseTargetSize = 0.005 * D

noseMinSize = noseTargetSize * 0.1
```

**其他部件直接计算（不依赖头部类型）：**

| 部件 | 目标尺寸 | 最小尺寸 |
|------|---------|---------|
| 尾部 | 0.05 × D | 目标 × 0.1 |
| 弹体 | 0.05 × D | 目标 × 0.1 |
| 弹翼 | 0.02 × 弹翼**根部**弦长（`wing_root_chord`） | 目标 × 0.1 |
| 舵 | 0.02 × 舵**根部**弦长（`fin_root_chord`） | 目标 × 0.1 |
| 弹翼/舵前缘、梢部 | 0.008 × 各自当地弦长 | 目标 × 0.1 |
| 舵根部面 | 0.008 × 舵当地弦长 | 目标 × 0.1 |

> 🔴 **本阶段的"当地弦长"取 C_root（根部弦长）** —— 需求文件 2.1 对翼/舵弦长 C 的定义即"**根部**前缘到尾缘的距离"。此阶段分组粒度是**整组**（`wing` / `fin`），无法按展向分段设不同尺寸；若误取 C_tip，整个翼面会按最小弦长加密（弹翼 C_root/C_tip 可差 5 倍以上），网格显著偏密。

**全局参数：** 自适应角统一为 10°。

**传入工具：** 各部件参数拼装后调用 `GenerateSurMeshBySpitAssemblyGroupProperty`：

| 参数 | 值 |
|------|-----|
| `ids` | `"0"`（所有超面）或指定面 ID 字符串 |
| `targetSize` / `minSize` | 全局（兜底）尺寸 —— **取各组计算值中的极值**：`targetSize` = max(各组 targetSize)，`minSize` = min(各组 minSize)。若各组尺寸尚未计算（`GetMissileDimensions` 返回兜底值时），再降级到 `GetGenerateSurMeshDefaultParam()` 的返回值。按部件区分的尺寸一律通过 `groupProperty` 里各组的 `domain[].targetSize` / `minSize` 设置 |
| `adaptAngle` | `10` |
| `way` | `0`（组合法），可选：`0`=组合法 `1`=狭长面 `2`=四边形占优 |
| `groupProperty` | **`GetAllSpitAssemblyGroupProperty()` 返回的 JSON 字符串原样回传**（工具 docstring 明示）。如需按部件区分尺寸，只改动各组 `domain[].targetSize` / `minSize` / `angle`，其余结构与键名保持原样；`ids` / `line` 等字段不得改动或删除 |

### `groupProperty` 数据结构

`GetAllSpitAssemblyGroupProperty()` 的 `result` 是一个 **JSON 字符串**，需二次 `json.loads` 后使用（外层 HTTP 响应为 `{"result": "<该字符串>"}`）。解析后结构：

```json
[ {"组名": {"domain": [{"angle":10, "ids":[3,28,29,30], "minSize":3.60832, "targetSize":72.16634}, ...],
            "line":   [{"angle":10, "ids":[],         "minSize":3.60832, "targetSize":72.16634}, ...] }},
  {"另一组名": {...}} ]
```

- **取面 ID**：`domain[].ids`（`line[].ids` 为空，是网格线分组，本阶段不用）。
- **`domain` 数组条目数可能 >1**——取**任一** `domain[0].ids`（或并集）结果相同。
- **按部件设置尺寸**：把要改的那个组的 `domain[]` 中每条 JSON 的 `targetSize` / `minSize` / `angle` 同步改成该部件的计算值（同一组内多条需保持一致）。
- ⚠️ **组名可能乱码**：当前工程可能返回 2 个分组名返回为 `U+FFFD` 乱码（后端编码问题，不可逆）。**遇到非 ASCII 且无法匹配预期组名的键时，不要按名匹配、不要跳过**——按 `domain[].ids` 的面 ID 归属判断，并在 `note` 中记录，提示用户核查后端编码。


## 生成后验收（🔴 必做，不可省略）

生成接口返回成功 **不等于** 网格可用。必须读取实际统计量、**把数字报出来**（不能只说"已生成"）：

| 核对项 | 判据 |
|---|---|
| **网格规模** | 报出**顶点数 / 单元数**。当前工具层**没有**直接返回这两个量的查询接口 → 用 **Σ `GetPointCount`(网格线 ID)** 作**代理量**（量级正确即可），并说明是代理量。🔴 **代理量只需抽样，不必全量**：取全部网格线的一小部分（如按 ID 均匀抽 10–20 条）求 Σ 点数，再按比例外推总量即可；一次性全量逐条查（动辄上百条、需分批）对验收结论无增益，纯耗轮次。🔴 **只报"网格线 N 条 / 网格面 M 个"是拓扑计数，不能当网格规模** |
| **密度量级自检** | 估算期望单元数 ≈ Σ(各部件表面积 ÷ 该部件目标尺寸²)，与实际值对照；**实际值超过估算 3 倍以上即视为异常**，先查原因再继续 |
| **网格封闭性** | 🔴 **不预先做逐面全量统计**（慢：341 线规模实测 5 分钟+；且 `GetAllObjectByType` 可能返回**幽灵 ID**，查不到坐标/下辖线时会被**误判成"不封闭线"**而卡住自己，2026-09-29 实机教训）。封闭性的**最终判定统一由"建场试金石"完成**（进入阶段 10 时 `UGBlockCreate` 返回 `"true"` = 封闭；返回 `"false"` 且报「不封闭的网格线ID如下」= 不封闭，清单以后端报出的为准），见 `references/missile-mesh-sealing-check.md` §6.0 |

**封闭性不合格时**（`UGBlockCreate` 返回 `"false"` 且报「不封闭的网格线ID如下」，或已确认存在未处理的不封闭线）——不要继续"各向异性 / 后缘面处理"（这两阶段很贵，且不封闭的网格最终必被外场/体网格拒绝），转入 `references/missile-mesh-sealing-check.md` 排查。

> 🔴 **"网格偏密"与"网格不封闭"常常同源**：CAD 面被切得过碎时，碎片边界在网格里既是**开口边界**（→ 不封闭），又会被 `adaptAngle` 判为"曲率超限"而向各组 `minSize` 加密（→ 偏密）。**这种情况靠调 `targetSize` / `minSize` 无解** —— 必须先处理 CAD 碎片（碎面合并 / 回到源头数模），见 `references/missile-mesh-sealing-check.md`「CAD 拓扑体检」。

## 补充加密规则

### 弹体二面角夹角处加密（头部锥体与弹体交接部位）

**触发条件：** 弹体上存在明显的二面角夹角，一般发生在头部锥体与弹体的交接部位。

**加密尺寸：** 0.002×D。

**定位方法（找到 nose 与 fuselage 的共享边）：**

```
# 1. 获取 nose 和 fuselage 分组的网格面 ID
#    真实数据源：GetAllSpitAssemblyGroupProperty() 返回 list[{组名:{line:[...], domain:[{ids:[...]}]}}]
#    取对应组的 domain[].ids 即该组网格面 ID
noseDomains = GetGroupDomainIds("nose")   # 解析 helper → [1, 2, ...]
fuselageDomains = GetGroupDomainIds("fuselage")   # → [3, 4, ...]

# 2. 获取两组的网格线集合
noseConnectors = set()
for id in noseDomains:
    result = GetConnectorsByDomains([id])        # → {"domains":[{"domain_id":id,"connector_ids":[...]}]}
    noseConnectors.update(result["domains"][0]["connector_ids"])

fuselageConnectors = set()
for id in fuselageDomains:
    result = GetConnectorsByDomains([id])
    fuselageConnectors.update(result["domains"][0]["connector_ids"])

# 3. 取交集 → nose 与 fuselage 的共享边（即头部锥体-弹体交接线）
junctionConnectors = noseConnectors & fuselageConnectors

# 4. 对每条共享边设置分布（不修改两端间距，仅通过增长率控制内部点向 0.002×D 过渡）
# 各子 Skill 共用参数：增长率 1.2、层数最大 50、分布类型 0（双曲正切）
for cid in junctionConnectors:
    UGReDimensionConfigDistribution(
        id=cid,
        headSpace=原有值,    # 不修改
        tailSpace=原有值,    # 不修改
        headRate=1.2,
        headLayer=50,
        tailRate=1.2,
        tailLayer=50,
        midValue=0.002 * D,
        disFunc=0
    )
```

> **注意：** 此加密在各向异性阶段（`missile-anisotropic-mesh.md`）的翼身结合处处理中执行，不要求在表面网格生成阶段就设好。参见 `references/missile-anisotropic-mesh.md` 和独立 Skill `missile-root-junction`。

### 翼/舵前缘和梢部

- 翼/舵前缘和梢部：点数≥7（前缘/梢部弧线需足够分辨率）

## 工具

| 工具 | 用途 | 类型 |
|------|------|------|
| `UGSur` | 全局表面网格 | [复用] |
| `GetGenerateSurMeshDefaultParam` | 获取默认参数 | [复用] |
| `GenerateSurMeshBySpitAssemblyGroupProperty` | 按部件组生成表面网格 | [复用] |

## 前置条件

1. 水密性处理已完成（自由边检查通过）
2. 导弹 AI 部件分割（`ClassifyMissile`）已完成
3. 碎面合并已完成
4. 导弹几何尺寸参数已获取（`GetMissileDimensions` 或用户直接提供）
5. 上述任一条件不满足时停止
