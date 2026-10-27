# 空间网格与外场创建

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

- **弓形外场**：`geoParam` = **外场长度 `1.5 × 弹体长度`，另外 3 位 `1.2 × 弹体长度`**（2026-10-23 用户裁定）；头部距离 = **0.2 × 弹径 D**。🔴 **不以 `GetCreateBlockDefaultParam()` 默认 `geoParam`（≈4×全弹长）为准**，也不做任何按型号的调整——**固定套用这两个倍数**。
- 非结构网格下，外场是体创建的产物（`UGBlockCreate` 的 `chooseParam` 第 3 位 = `3` 为弓形）。
- `fuselage_length` 与 `body_diameter`（弹径 D）来自 `GetMissileDimensions` 或用户输入；🔴 **一律取接口值**（**单位已是 mm，禁止 ×1000**）——旧"固定值覆盖接口"的做法**已作废**（事实中心 §2，2026-10-14 用户裁定）。

### §6.3 体创建（一键：`CreateMissileFarFieldBlock`）

🔴 **唯一入口是导弹工具 `CreateMissileFarFieldBlock()`（无入参）**。上面 §6.2 的倍数规则、头部方向推导、`centerCoor` 排除远场面、`meshSize` 等比缩放，以及最终那次 `UGBlockCreate` 调用，**全部在后端一次完成**——Agent 不需要、也**不应该**再去调 `GetCreateBlockDefaultParam` / `GetAllSpitAssemblyGroupProperty` / `GetStartAndEndPointByConnectors` / `UGBlockCreate` 来手工拼这套参数。

```
CreateMissileFarFieldBlock()   # 无参。返回 status="success" 即外场已建
```

**前置条件（顺序不可颠倒）**：数模已导入 → 表面网格已生成（`MissileSurfMeshGenerate`）→ `ClassifyMissile` 已完成（后端靠 `nose` / `fuselage` 分组定弹头方向）→ `GetMissileDimensions` 已调用（全弹长写回 `SPIDER::Data`）。

**后端做什么**（了解即可，**不要据此复算**）：

| 参数 | 来源 |
|---|---|
| `chooseParam` 第 1 位（头部方向） | `nose` 与 `fuselage` 分组**网格面**包围盒中心沿弹体最长轴的先后 → **`0=+X` `1=−X` `2=+Y` `3=−Y` `4=+Z` `5=−Z`**（= 2×轴号 + 正向 0 / 负向 1；X轴=0、Y轴=1、Z轴=2），与后端 `CreatEllipse()` 映射一致。弓形锥顶落在 `centerCoor + 头部方向 × 外场长度/2` |
| `chooseParam` 第 2/3 位 | 恒 `0`（外场）、恒 `3`（弓形） |
| `geoParam` | 外场长度 `1.5 × 全弹长`，另外 3 位 `1.2 × 全弹长`（§6.2，单位 mm） |
| `centerCoor` | 模型包围盒中心，**已排除外场面 `OUTFILED` 与加密源面 `SOURCE`**。弓形严格以它居中（X 跨 `centerCoor ± S/2`，Y/Z 关于 `centerCoor` 对称），无需补偿 |
| `meshSizeOrDimension` | 默认网格尺寸 × (外场长度 / 默认 geoParam)，等比缩放；`meshType` = `0`（给定尺寸） |

> ⚠️ 后端取分组几何用的是**网格面**包围盒，不是分割返回的**超面 ID**。历史上直接用超面 ID 查端点得到过不像弹头的几何（2026-10-23 实测）——这条坑现已由后端消化，Agent 侧不必再关心。

**返回与判据**：

- 🔴 **只看 `status`**：`"success"` 即完成本阶段，**直接进入 §6.4 空间网格**——**不做建后校验**（2026-10-23 用户裁定：为省时省略）。不查外场面数量、不核对包围盒。
- 返回值（`geoParam` / `chooseParam` / `centerCoor` / `meshSizeOrDimension` / `headDirection`）是后端**实际使用**的参数，仅供排查回显，**不要求 Agent 据此复算**。
- `"failed"` 时按 `message` 排查：
  - `fuselage_length is 0: run ClassifyMissile + GetMissileDimensions first` → 前置步骤漏了
  - `empty model bounding box` → 无物面网格，检查表面网格是否已生成
  - `UGBlockCreate returned false (see sealing check)` → 转 `missile-mesh-sealing-check.md` 排查不封闭线
  - 返回里带 `warning`（`nose`/`fuselage` 分组缺失）→ 头部方向已回退 `+X`（`headDirectionSource` = `fallback+X`），**必须向用户确认真实弹头指向**，别默认 +X 就是对的

> 需要删掉重建时：`DeleteBlock(blockIDs, isDeleteDomain=0, isDeleteConnector=0)` **只删体块**，外场面用 `DeleteDomain(domainIDs, isDeleteConnector=1)` 单独删——🔴 **严禁 `DeleteBlock` 传 `isDeleteDomain=1`**（会级联误删表面网格）。


### §6.4 空间网格生成

1. 确定附面层参数（默认值）：y+=1、Re=1.5×10⁷、参考长度取 MAC、增长率 1.15、层数 40、扩散因子 0.5、单元类型四面体。

**`caliperFirst`（第一层厚度）直接套公式，不现场推导**：

```plaintext
Re_x = ρ·U∞·L/μ ;  Cf = 0.0576/Re_x^0.2 ;  caliperFirst_mm = y+·μ/(ρ·U∞·√(Cf/2)) × 1000
```

- 全 SI 代入（`L` 用**米**）：`μ`≈1.789×10⁻⁵、`ρ`≈1.225、`U∞` 由马赫数换算、`L`=MAC（缺失用全弹长近似）。
- ⚠️ **单位陷阱**：`L` 误按 mm 代入 → Re 差 1000 倍 → caliperFirst 差 1000 倍。**算完 ×1000 转 mm 再传参**，note 记录 Re/L/马赫数。
- 量级参考：**0.001 ~ 0.01 mm**（超声速更低）；**> 0.1 mm 必错**。

2. 调用 `UGUGSp(generateWay, ids, layer=40, growRate=1.15, caliperFirst=计算值, diffusionFactor=0.5)` 生成空间网格。
   - 🔴 `ids` 先剔幽灵块——**只取存活台账中的块**（事实中心 §4），否则 `UGUGSp` 连续返回 `false`。
   - 更换 ids 后若仍 `false` → 转 §6.3 失败排查（可能是敞口拒绝体填充），**不得连续换参重试**。

## §7 质量检查与输出

### §7.1 网格质量

> 🔴 **本流程只查面网格，不查体网格**（2026-10-15 用户裁定）。理由见 §7.1.2。

#### 7.1.1 面网格质量检查（本流程唯一的质量检查）

| 指标 | 工具参数 | 合格标准 | 处理规则 |
|---|---|---|---|
| 最小角 | `ExamineDomain(examType="MinmumAngle")` | > 10° | 若小于 10°，需查看最小角出现位置（给出数值 + 网格面 ID）。模型特征导致的最小角可不处理；各向异性单元处的最小角不纳入判断。 |

- `ids` **必须传实际网格面 ID**（`GetAllObjectByType(6)` 的 `info` 拼接，如 `"1,2,3,...,91"`）；传空串返回 `"false"`。
- 🔴 **不要先试 `CheckMissileMeshQuality`**：该工具后端返回恒定 `"true"`（兜底值，无数据），**属于主 Skill「工具可用性判定」表的"返回恒定值 → 不可用"类**，直接跳过、不调用。质量检查走上面这行的 `ExamineDomain` 单一路径。

#### 7.1.2 🔴 体网格质量检查 —— 本流程不执行

| 指标 | 工具参数 | 说明 |
|---|---|---|
| ~~最大角~~ | ~~`ExamineBlock(examType="ExamineMaximumIncludeAngle")`~~ | 🔴 **不再调用**。指标（≤178°、禁止 179.9°）本流程不检查 |

> 🔴 **为什么删除（2026-10-15 用户裁定）**：`ExamineBlock(ExamineMaximumIncludeAngle)` 对整套体网格（几十万四面体）执行时**后端无响应、调用永久挂死**——既不会返回 `"false"`，也不会报错，只能由用户手动停止本轮对话。
> 实跑证据：该工具共被调用 4 次（2026-10-10 / 10-12 / 10-13 ×2），**4 次全部卡住**，其中 3 次为用户手动停止、1 次为后端断连（`RemoteDisconnected`）。**没有一次拿到结果。**
> 原先本文写的"后端崩溃 / `ConnectionResetError` / 重试同错"防御**与实机表现不符**——实际是不报错地挂死，故该防御从未被触发。工具层 `send_post_request` 默认不限时（见 `agent/client.py`），后端算不出来就无限等待。
> 🔴 **纪律**：**不得调用该工具**（包括"试一次看看"）。若用户明确要求体网格质量，先说明该工具会挂死后端、当前无法测得，由用户决定是否另想办法；**不得静默重试、不得换参重试**。

### §7.2 边界条件设置

- **工具**：`BorderConditionAddGroup(name, groupID, colorNumber)` `[复用]`
- **工具**：`BorderConditionSaveDataToDomain(domainIDs, name, groupID, property)` `[复用]`
- 参数说明：

| 参数 | 说明 |
|------|------|
| `name` | 边界条件组名，使用**实际存在的分组名**（如 `"nose"`/`"fuselage"`/`"tail"`，或弹翼带序号子面名 `wingLeadingEdgeN`…）。🔴 **后端不产出 `wing` 主组**，弹翼直接用子组名 |
| `groupID` | 从 100 开始递增，每个分组 +1（100, 101, 102, ...） |
| `colorNumber` | 按分组顺序循环取 0–5 |
| `domainIDs` | 逗号分隔的网格面 ID 字符串，如 `"4,31,55"`。通过 `GetAllSpitAssemblyGroupProperty()` 按组名取 `domain[].ids` 后拼接（`GetSpliteAssemlyDomainsBatch` 返回兜底值时改用本方式）。🔴 **一次查询拿全全部分组 ID 并缓存到事实卡**（主 Skill 纪律第 3 条）：后续逐组设置 BC 时**直接取缓存**，严禁对每个 BC 组重复调 `GetAllSpitAssemblyGroupProperty`（10 个 BC 组 = 10 次冗余查询） |
| `property` | 首次铺底**必须为 `-10`**（无边界条件），**严禁在铺底阶段写入 4（粘性固壁）/7（远场）/8（对称）等实际属性值**；实际属性只能在铺底完成后、**按用户明确要求**逐组设置（见下方「属性设置闸门」） |

- 属性分类（**仅作映射参考，不是"照此执行"指令**）：外场→远场(7)、物面→粘性固壁(4)、对称面→对称(8)
- 物面可按分部件细分：弹头、弹体、弹翼、尾部
- **首次铺底 property 必须为 -10（无边界条件）**

> 🔴 **属性设置闸门**：铺底（全部组写入 `-10`）完成后**即停**。实际属性设置走 `BorderConditioConfigProperty(number=2)` **逐组编辑**，且**必须先获得用户明确要求**（如"把外场设为远场""物面设粘性固壁"）才可写入 4/7/8；**用户未明确要求前，不得自行写入任何实际属性值**。本步骤到此为止，下阶段为质量检查（§7.1）。

### §7.3 输出

- **保存工程**：`SaveSpdFile(filename)` `[复用]`，名字默认为模型名，路径默认为模型路径
- **网格导出**：`ExportGrid(outType, objType, name, outIDs, dataType, precision, unit)` `[复用]`
  - ⚠️ **`outType` 合法值只有** `"CGNS_3.21"` / `"CGNS_2.54"` / `"Gridgen"` / `"Plot3D"`——**CGNS 导出必须传 `"CGNS_3.21"`（或 `"CGNS_2.54"`），严禁传 `"cgns"`**（传 `"cgns"` 会被拒绝）。
  - `objType`：`"Connector"` 网格线 / `"Domain"` 网格面 / `"Block"` 网格块。
  - `dataType` 0=二进制 1=十进制 2=无格式；`precision` 0=单精度 1=双精度；`unit` 0=不转换 1=米 2=毫米 3=英寸。CGNS 格式下后三者取默认值。
  - **无体网格块（外场创建被阻塞）时不要尝试导出 `"Block"`**，应如实报告"无体网格可导出"。
