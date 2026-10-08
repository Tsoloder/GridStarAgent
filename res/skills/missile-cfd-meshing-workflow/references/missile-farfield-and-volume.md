# 空间网格与外场创建

> 原属  §6.1–§6.4 + §7，独立为参考文件以便按需读取。

### §6.1 场景分类（4 种）

**第一步：先判定，再选场景（本步不可省略）。**

1. **外场是否存在** —— 调用通用只读查询 `GetDomainsByType(3)`（`3` = **外场面**；属 `query` 组，默认启用）：
   - 返回**非空**（如 `{"info":[12,13]}`）→ **外场存在** → 场景一 / 二
   - 返回 `{"info":[]}` → **无外场** → 场景三 / 四（须先体创建生成外场，见 §6.2–§6.3）
2. **全模 / 半模** —— 依据：① 工程是否含**对称面 / 半模边界线**；② 水密阶段自由边是否**全部位于半模线**（见 §2）；③ 用户说明。
3. **任一依据不足时向用户确认，不得猜测**（auto 模式下属"缺少无法推导的必填信息"，可用简短问题询问，见第 2.1 节第 5 条）。

**第二步：按下表选择场景。**

| 场景 | 条件 | 处理 |
|------|------|------|
| 一 | 外场存在 + 全模 | 外场面→外场属性，物面→物面属性，法向量向外 |
| 二 | 外场存在 + 半模 | 外场→外场属性，物面→物面属性，生维面→对称属性，法向量向外 |
| 三 | 无外场 + 全模 | 先通过体创建生成外场 |
| 四 | 无外场 + 半模 | 先设半模边界 → 体创建外场 → 生成空间网格 |

### §6.2 外场生成规则

- **弓形外场**：按 **1 倍弹体长度**设置，头部距离按 **1.5 倍球头半径**（需求文档 6.2）。🔴 **实测注记（2026-10-05）**：实机正确外场使用 `GetCreateBlockDefaultParam()` 的**默认 `geoParam`**（`"20320.4,20320.4,20320.4,20320.4"` ≈ **4 × 全弹长**），与需求文档"1 倍弹长"不符——**以默认值为准，不要按弹长重算**（见 §6.3）。
- 非结构网格下，外场是体创建的产物（`UGBlockCreate` 的 `chooseParam` 第 3 位 = `3` 为弓形），不存在独立的外场创建步骤。
- `fuselage_length` 与 `nose_radius` 来自 `GetMissileDimensions` 或用户输入。

### §6.3 体创建

1. 调用 `GetCreateBlockDefaultParam()`，取默认 `geoParam`（4 个浮点，长度/半径/短轴/长轴）、`meshSize`、`centerCoor`（注意：`chooseParam` 默认值为 `"0,0,0"`，本项目按下方第 2 步覆盖为 `"-1,0,3"`）。
2. 按 §6.2 外场规则确定实际参数：
   - `geoParam` = **默认值原样使用**（第 1 位 = 4 × 全弹长 ≈ `20320.4`；🔴 实测正确外场即默认值，**不要**按"1 倍弹长"重算第 1 位）。
   - `chooseParam` = `"-1,0,3"`（第 1 位=头部方向、第 2 位=体类型 `0`=外场、第 3 位=外场形状 `3`=弓形）。🔴 **方向编码以实机实测为准（2026-10-05）**：`-1` = 弓形头部朝 **-X（弹头方向，正确，用户目视确认）**；`0` 与 `1` 实测都反到 **+X** 且歪（工具 Schema 注释"0=+X、1=-X"与实机不符）。**本项目唯一合法值 = `"-1,0,3"`，严禁传 `"0,0,3"` / `"1,0,3"`**。
   - `centerCoor` = 弹体包围盒中心 + **弓形固有偏移补偿**。🔴 实测（2026-10-05）：传 centerCoor=(2461.67,0,-0.996)（弹体中心）时，外场实际中心落在 (-925.1, +6773.5)，**弓形放置自带固定偏移 (Δx=-3386.77, Δy=+6773.5, Δz≈0)**。因此必须 **反向补偿**：`centerCoor = 弹体包围盒中心 + (3386.77, -6773.5, 0)`（本导弹实测 = `"5848.44,-6773.5,-0.996"`），补偿后才能与手动正确版一致（锥顶朝 -X、Y 对称 ±20320.4）。
     - 弹体包围盒中心取法：`GetAllObjectByType(5)` 取网格线 → `GetStartAndEndPointByConnectors` → 各轴 min/max 取中值；循环绕一圈微调补偿值使锥顶 Y≈0、X 中心=弹体中心。
     - ⚠️ 补偿值 (3386.77, -6773.5) 可能随尺寸/方向变化，**建后必须校验**：读外场几何确认 Y 对称、X 中心对中，不对则迭代微调 `centerCoor` 再建（见第 4 步）。
   - `meshSizeOrDimension` = 默认 `meshSize` 转 float（字段名改名映射；`meshType` 转 int，`0`=给定尺寸）。
3. 调用 `UGBlockCreate(geoParam, chooseParam, centerCoor, meshType, meshSizeOrDimension)`。
   - 🔴 返回 `"true"` = 网格封闭，外场面已生成；返回 `"false"` 且报不封闭线 = 转 `missile-mesh-sealing-check.md` 排查。
4. **建后立即校验（不省略）**：
   - `GetDomainsByType(3)` 确认外场面已生成（弓形通常 5 个面）。
   - 用 `GetConnectorsByDomains` + `GetStartAndEndPointByConnectors` 读外场几何，核对：
     - **弓形头部（被最多线共享的点）是否朝 -X**（锥顶 x ≈ 弹体中心 x − 半长，如 [-7700, -7698]）；
     - **Y 是否对称 ±半长**（如 ±20320.4，中心≈0）；X 中心是否≈弹体中心 x。
   - 三项全过 → 请用户界面目视复核方向；**中心/对称性不对 → 按偏差微调 centerCoor 反向补偿后重删重建**（最多 2–3 轮，不穷举）。
   - 清理注意：重删时 `DeleteBlock(blockIDs, isDeleteDomain=0, isDeleteConnector=0)` **只删体块**，外场面用 `DeleteDomain(domainIDs, isDeleteConnector=1)` 单独删——⚠️ 严禁 `DeleteBlock` 传 `isDeleteDomain=1`（会级联误删表面网格，2026-10-05 教训）。

> 失败处理（返回 `"false"`，按序排查，不穷举参数组合）：
> 1. 错误信息含「不封闭的网格线ID如下」→ 转排查（清单以后端报出为准）。
> 2. 核对格式：`chooseParam` 为 3 整数字符串、`geoParam` 为 4 浮点、`meshSizeOrDimension` 为数值。
> 3. 仍失败 → 记录实际参数与返回值，报告用户/后端，停止重试。

### §6.4 空间网格生成

1. 确定附面层参数：

| 参数 | 默认值 |
|------|--------|
| y+ | 1 |
| 雷诺数 Re | 1.5×10⁷（范围 1×10⁷~3×10⁷） |
| 参考长度 | 平均气动弦长（MAC） |
| 增长率 | 1.15 |
| 层数 | 40 |
| 扩散因子 | 粗网格 0.5 / 中等 0.8 / 细网格 0.98 |
| 单元类型 | 四面体 |

> MAC 由后端接口返回；当前 `GetMissileDimensions` 未含 MAC，缺失时用全弹长或当地弦长近似，并在 `note` 记录。

2. 调用 `UGUGSp(generateWay, ids, layer=40, growRate=1.15, caliperFirst=计算值, diffusionFactor=0.5/0.8/0.98)` 生成空间网格。

## §7 质量检查与输出

### §7.1 面网格质量

- **工具**：`ExamineDomain(examType, ids)` `[复用]`
- **examType**：`"MinmumAngle"`（最小角检查）
- **`ids`**：⚠️ **必须传实际网格面 ID**（由 `GetAllObjectByType(6)` 取到的 `info` 列表拼成，如 `"1,2,3,...,91"`）；**传空串会返回 `"false"`**。
- **标准**：除去各向异性单元，最小角 > 10°
- **若 < 10°（需求文件 7.1 强制）**：**必须给出该处的最小角数值与出现位置（网格面 ID）**，才能判定"模型特征导致、可不处理"。仅写"模型特征处可接受"**不成立** —— 未给出位置与数值的判定属**不合格报告**。

### §7.2 体网格质量

- **工具**：`ExamineBlock(examType, ids)` `[复用]` — `ids` 同样必须传实际**网格块** ID（`GetAllObjectByType(7)`）；无体网格块时本步无法执行，标记 `blocked` + note（不是"跳过"）。
- **examType**：`"ExamineMaximumIncludeAngle"`（最大角检查）
- **标准**：最大角 ≤ 178°；> 178° 需输出数量；**禁止 179.9° 单元**

### §7.3 综合质量检查（可选）

- **工具**：`CheckMissileMeshQuality(domainIds, blockIds)` — ⚠️ **返回兜底值**（恒定 `"true"`）时视为不可用
- 计划返回：`{"surfaceMinAngle": float, "volumeMaxAngle": float, "badCellCount": int, "passed": bool}`
- **质量检查一律以通用工具 `ExamineDomain` + `ExamineBlock` 为准**（§7.1/§7.2），这是唯一真实依据。

### §7.4 边界条件设置

- **工具**：`BorderConditionAddGroup(name, groupID, colorNumber)` `[复用]`
- **工具**：`BorderConditionSaveDataToDomain(domainIDs, name, groupID, property)` `[复用]`
- 参数说明：

| 参数 | 说明 |
|------|------|
| `name` | 边界条件组名，使用**实际存在的分组名**（如 `"nose"`/`"fuselage"`/`"tail"`，或弹翼子面名 `wingLeadingEdge`…）。⚠️ 归并后弹翼主组为 `"wing"`；🔴 当前型号无舵，无 `"fin"` 组 |
| `groupID` | 从 100 开始递增，每个分组 +1（100, 101, 102, ...） |
| `colorNumber` | 按分组顺序循环取 0–5 |
| `domainIDs` | 逗号分隔的网格面 ID 字符串，如 `"4,31,55"`。通过 `GetAllSpitAssemblyGroupProperty()` 按组名取 `domain[].ids` 后拼接（`GetSpliteAssemlyDomainsBatch` 返回兜底值时改用本方式） |
| `property` | 首次铺底**必须为 `-10`**（无边界条件）；实际属性（物面→粘性固壁、外场→远场、对称→对称）在铺底完成后逐组设置 |

- 分类：外场、物面、对称
- 物面可按分部件细分：弹头、弹体、弹翼、尾部（🔴 当前型号无舵）
- **首次铺底 property 必须为 -10（无边界条件）**

### §7.5 输出

- **保存工程**：`SaveSpdFile(filename)` `[复用]`，名字默认为模型名，路径默认为模型路径
- **网格导出**：`ExportGrid(outType, objType, name, outIDs, dataType, precision, unit)` `[复用]`
  - ⚠️ **`outType` 合法值只有** `"CGNS_3.21"` / `"CGNS_2.54"` / `"Gridgen"` / `"Plot3D"`——**CGNS 导出必须传 `"CGNS_3.21"`（或 `"CGNS_2.54"`），严禁传 `"cgns"`**（传 `"cgns"` 会被拒绝）。
  - `objType`：`"Connector"` 网格线 / `"Domain"` 网格面 / `"Block"` 网格块。
  - `dataType` 0=二进制 1=十进制 2=无格式；`precision` 0=单精度 1=双精度；`unit` 0=不转换 1=米 2=毫米 3=英寸。CGNS 格式下后三者取默认值。
  - **无体网格块（外场创建被阻塞）时不要尝试导出 `"Block"`**，应如实报告"无体网格可导出"。
