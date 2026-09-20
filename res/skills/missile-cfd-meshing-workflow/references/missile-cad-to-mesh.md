# 导弹 CAD → 网格全流程

导弹专用 CAD 导入到网格导出全流程参数。对应 t.py 模块 1/3/6/7。

## §1 CAD 导入与单位设置

### 1.1 导入

- **工具**：`ImportCADFile(filename, angle, targetSize, minSize, unit=1, append=0)` `[复用]`
- **单位**：unit=1 表示 mm；其他保持默认

### 1.2 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| filename | 用户指定 | 导弹 CAD 文件路径 |
| angle | 0 | 旋转角度 |
| targetSize | 0 | 目标尺寸（0=自动） |
| minSize | 0 | 最小尺寸（0=自动） |
| unit | 1 | 单位（mm） |
| append | 0 | 追加模式（0=新建） |

## §2 水密性处理

### 2.1 工具

- **`GetDealWatertightTolenrance()`** `[复用]` — 获取当前模型的默认水密性公差
- **`DealWatertight(tolenrance)`** `[复用]` — 执行水密性处理

### 2.2 Agent 侧逻辑（公差查询 + 5 倍递增）

1. 先调用 `GetDealWatertightTolenrance()` 获取默认公差值
2. 用获取的公差调用 `DealWatertight`
3. 检查返回值中的 `has_free_edges` 字段判断是否还有自由边
4. 若存在自由边，公差 ×5 重试
5. 最多递增 2 次（共 3 次调用），若仍未处理任何自由边则停止
6. 判断：
   - 无自由边 → 成功 → 进入下一流程
   - 自由边全在半模线 → 成功
   - 其他位置有自由边 → 提示模型可能有缝隙/穿插/孔洞

## §3 分部件与碎面合并

### §3.1 碎面合并

- **工具**：`UGSurfaceProcessing(edgeIDs, type, tolerance, minLenth)` `[复用]`

按部件规则：

| 部件 | 合并规则 |
|------|---------|
| 头部/尾部 | 尽量合并为一个面 |
| 翼 | 前缘面、翼梢面（梯形翼才有）、尾缘面、翼侧面；无明显二面角时可合并 |
| 舵 | 前缘、舵顶、根部、尾缘、侧面 |
| 其他 | 保证平整，仅保留关键特征 |

辅助工具：
- `UGDelRedundantDom(selectedID, domType, precisio, overlapRatio)` `[复用]` — 删重复面
- `UGSpitAssemblyCreateNewGroup` `[复用]` — 创建部件组
- `UGSpitAssemblyMoveNodesToNewGroup` `[复用]` — 移动元素到组

详见 `references/missile-segmentation.md` 获取 5 类分割详细规则。

## §4 表面网格生成

详见 `references/missile-surface-mesh.md` 获取部件网格参数公式表。

- 全局：`UGSur(type, ids, targetSize, minSize, adaptAngle, way)` `[复用]`
- 按部件组：`GenerateSurMeshBySpitAssemblyGroupProperty(ids, targetSize, minSize, adaptAngle, way, groupProperty)` `[复用]`
- 补充：弹体二面角夹角处加密 0.002×D

## §6 空间网格生成

### §6.1 场景分类（4 种）

| 场景 | 条件 | 处理 |
|------|------|------|
| 一 | 外场存在 + 全模 | 外场面→外场属性，物面→物面属性，法向量向外 |
| 二 | 外场存在 + 半模 | 外场→外场属性，物面→物面属性，生维面→对称属性，法向量向外 |
| 三 | 无外场 + 全模 | 先通过体创建生成外场 |
| 四 | 无外场 + 半模 | 先设半模边界 → 体创建外场 → 生成空间网格 |

### §6.2 外场规则

- **弓形外场**，按 1 倍弹体长度设置
- 头部距离按 1.5 倍头部半径
- **工具**：`CreateMissileFarField(bodyLength, headRadius)` — 场景三/四（无外场）时调用
- 半模边界：`UGHalfModelLine(cnIDs, symmetry)` `[复用]`

### §6.3 体创建

- **工具**：`UGBlockCreate(geoParam, chooseParam, centerCoor, meshType, meshSizeOrDimension)` `[复用]`
- 参数说明：

| 参数 | 说明 |
|------|------|
| `geoParam` | 4 个浮点数（长度/半径/短轴/长轴），默认值通过 `GetCreateBlockDefaultParam()` 获取。导弹弓形外场按 1×弹体长度调整 |
| `chooseParam` | 外场形状：`0`=球形 `1`=立方体 `2`=圆柱 `3`=弓形 |
| `centerCoor` | 体网格块中心坐标，默认值通过 `GetCreateBlockDefaultParam()` 获取 |
| `meshType` | `0`=给定尺寸，`1`=期望点数（默认 41） |
| `meshSizeOrDimension` | 网格尺寸（float），从 `GetCreateBlockDefaultParam` 返回的 `meshSize` 字段改名传入 |

- 调用约定（强制）：
  1. 先调用 `GetCreateBlockDefaultParam()` 获取默认 `geoParam`/`chooseParam`/`centerCoor`/`meshType`/`meshSize`。
  2. **字段名映射**：返回值中的 `meshSize` 传入 `UGBlockCreate` 时必须改名为 `meshSizeOrDimension`，并转为数值（float）；严禁传字符串、严禁使用 `meshSize` 作为参数名（MCP Schema 无此参数，会直接校验失败）。
  3. 若按 §6.2 外场规则调整了 `geoParam`，`meshSizeOrDimension` 必须按同一缩放系数同步调整，保持外场网格分辨率一致。

### §6.4 空间网格参数

| 参数 | 默认值 |
|------|--------|
| y+ | 1 |
| 雷诺数 Re | 1.5×10⁷（范围 1×10⁷~3×10⁷） |
| 参考长度 | 平均气动弦长（MAC） |
| 增长率 | 1.15 |
| 层数 | 40 |
| 扩散因子 | 粗网格 0.5 / 中等 0.8 / 细网格 0.98 |
| 单元类型 | 四面体 |

- **体网格生成**：`UGUGSp(generateWay, ids, layer=40, growRate=1.15, caliperFirst=计算值, diffusionFactor=0.5/0.8/0.98)` `[复用]`

## §7 质量检查与输出

### §7.1 面网格质量

- **工具**：`ExamineDomain(examType, ids)` `[复用]`
- **examType**：`"MinmumAngle"`（最小角检查）
- **标准**：除去各向异性单元，最小角 > 10°
- 若 < 10°：查看位置，模型特征导致的可不处理

### §7.2 体网格质量

- **工具**：`ExamineBlock(examType, ids)` `[复用]`
- **examType**：`"ExamineMaximumIncludeAngle"`（最大角检查）
- **标准**：最大角 ≤ 178°；> 178° 需输出数量；**禁止 179.9° 单元**

### §7.3 综合质量检查（可选）

- **工具**：`CheckMissileMeshQuality(domainIds, blockIds)`
- 返回：`{"surfaceMinAngle": float, "volumeMaxAngle": float, "badCellCount": int, "passed": bool}`
- **注意**：该工具为辅助参考，Agent 必须首先使用通用工具 `ExamineDomain` 和 `ExamineBlock` 做真实质量检查，不能仅依赖 `CheckMissileMeshQuality` 的 `passed` 字段作为质量合格的唯一依据。

### §7.4 边界条件设置

- **工具**：`BorderConditionAddGroup(name, groupID, colorNumber)` `[复用]`
- **工具**：`BorderConditionSaveDataToDomain(domainIDs, name, groupID, property)` `[复用]`
- 参数说明：

| 参数 | 说明 |
|------|------|
| `name` | 边界条件组名，直接使用导弹分部件分组名称（如 "nose"/"body"/"fin"/"rudder"/"tail"） |
| `groupID` | 从 100 开始递增，每个分组 +1（100, 101, 102, ...） |
| `colorNumber` | 按分组顺序循环取 0–5 |
| `domainIDs` | 逗号分隔的网格面 ID 字符串，如 `"4,31,55"`。通过 `GetSpliteAssemlyDomains` 获取各分组 domain IDs 后拼接 |
| `property` | 首次铺底**必须为 `-10`**（无边界条件）；实际属性（物面→粘性固壁、外场→远场、对称→对称）在铺底完成后逐组设置 |

- 分类：外场、物面、对称
- 物面可按分部件细分：弹头、弹体、弹翼、舵、尾部
- **首次铺底 property 必须为 -10（无边界条件）**

### §7.5 输出

- **保存工程**：`SaveSpdFile(filename)` `[复用]`，名字默认为模型名，路径默认为模型路径
- **网格导出**：`ExportGrid(outType="cgns", objType, name, outIDs, dataType, precision, unit)` `[复用]`，CGNS 格式
