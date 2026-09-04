# 从 CAD 生成 CFD 网格

phase_plan 通用格式与输出规则参见系统提示词第 5.1 节。本流程的默认阶段计划如下：

- `id`: `cad-mesh-main`
- 默认阶段：CAD 导入、水密性处理、分部件处理、表面网格生成、体网格块创建、空间网格生成
- 可选阶段未采用时标记为 `skipped`


水密性与分部件既可单独选择，也可串联执行。用户要求完整处理或同时提到二者时，采用"水密性处理 → 分部件处理 → 表面网格"的组合路径。

## 1. 导入 CAD

1. 获取 CAD 文件路径。
2. 从实时工具列表定位 CAD 导入工具，通常为 `ImportCADFile`。
3. 根据 Schema 获取角度、目标尺寸、最小尺寸、单位和追加方式等参数。
4. 手动模式使用基础 `tool_params` 协议确认参数；自动模式使用已知值或默认值直接执行。
5. 等待工具返回；只有明确成功后才进入下一阶段。
6. 导入成功后，标记 `import` 阶段为 `completed`、下一阶段为 `active`，并提供当前可用路径：水密性处理、分部件处理（手动分组或 AI 自动部件分割）、水密性后分部件处理、直接提取边界线。阶段计划的输出方式遵循 cfd_workflow.md 第 5.1 节的模式规则。用户选择 AI 自动部件分割时，读取 `references/part-segmentation.md`。
7. 手动模式使用 `options` 等待用户选择路径；用户尚未选择时，不得直接调用 `UGSur` 或其他表面网格生成工具。
8. 自动模式根据用户原始目标选择最完整的匹配路径；目标不明确时仍使用 `options` 询问，不得擅自扩大处理范围。
9. 只有路径已经明确且完成相应前置处理后，才能进入表面网格生成阶段。

## 2. 水密性处理路径

**详细的水密性处理与自由边检查迭代逻辑，参见 `references/watertight-processing.md`。**

核心流程：
1. 从 MCP 工具中识别公差查询和水密性处理工具。当前接口通常为 `GetDealWatertightTolenrance` 与 `DealWatertight`，仍应以实时工具列表为准。
2. 调用公差查询工具并等待结果。
3. 从查询结果提取公差；手动模式使用 `tool_params` 显示实际值并确认，自动模式直接采用查询值。不得提前猜测或硬编码公差。
4. 调用水密性处理工具，并等待明确的成功结果。
5. **自由边检查**：处理后检查是否还有自由边。存在自由边则公差加大 5 倍重试，最多重试 2 次。根据自由边位置判断是否处理成功（半模线位置可接受）。
   - **预留工具**：`CheckFreeEdges`（自由边检查）— 当前 MCP 工具列表中尚不存在。在工具就绪前，需提示用户在软件界面中目视检查自由边（蓝色显示）。
6. 水密性处理失败（自由边在非半模线位置）时停止，提示用户模型可能存在缝隙、穿插、孔洞等错误，不得继续查询网格默认参数或调用 `UGSur`。
7. 水密性处理成功后，调用 `GetGenerateSurMeshDefaultParam` 或实时 MCP 中对应的默认参数工具。
8. 从返回值提取 `targetSize`、`minSize`、`adaptAngle`，结合对象范围和生成方案形成 `UGSur` 参数表。
9. 如已完成几何参数计算（MAC），应根据部件网格参数表调整各部件的目标尺寸和最小尺寸。参见 `references/geometry-parameters.md`。
10. 如果已通过 AI 自动部件分割完成了分部件处理，按分组生成表面网格可转而使用 `references/part-based-surface-mesh.md` 的流程，利用 `GenerateSurMeshBySpitAssemblyGroupProperty` 为各部件组分别设置基于 MAC 的网格参数。
11. 手动模式由用户确认，自动模式采用查询结果直接调用 `UGSur`；等待成功结果后才报告表面网格完成。
12. 这条路径的强制顺序为：`ImportCADFile` → 公差查询 → 水密性处理 → 自由边检查 → 表面网格默认参数查询 → `UGSur`（或 `GenerateSurMeshBySpitAssemblyGroupProperty` 分部件优化路径）。

## 3. 分部件处理路径

本节描述手动分部件处理流程。如需 AI 自动识别部件并分组，转至 `references/part-segmentation.md`。

1. 获取现有分组或分部件信息。
2. 根据用户目标创建、重命名或调整分组。
3. 需要指定对象时，声明目标类型，并遵守基础对象范围选择规则。
4. 将对象分配到相应分组。
5. 获取各分组的表面网格参数。
   - 如果 MAC 值已知（`GetModelParameters` 获取或按公式计算），建议按 `references/part-based-surface-mesh.md` 的规则为各部件组设置基于 MAC 比例的目标尺寸和最小尺寸。
   - 如果 MAC 值不可用，通过 `GetGenerateSurMeshDefaultParam` 获取全局默认参数作为各组的统一值。
6. 按分组生成表面网格。
   - 使用 `GenerateSurMeshBySpitAssemblyGroupProperty` 工具，传入更新后的分组属性 JSON。
   - 如未自定义分组属性，使用 `GetAllSpitAssemblyGroupProperty` 获取的默认值直接传入。
   - 详细参数计算和调用流程见 `references/part-based-surface-mesh.md`。

## 4. 默认参数生成表面网格

1. 通过工具`GetGenerateSurMeshDefaultParam`获取表面网格生成默认参数。
2. 用户没有要求则默认生成所有超面（使用工具`GetAllObjectByType`获取所有超面的ID）的表面网格。
3. 如果需要已有的网格面加密或者稀疏，则再次调用工具`UGSur`生成表面网格即可。
4. 如果已通过 AI 自动部件分割（`SegmentPart`）或手动分组完成了分部件处理，且 MAC 值已知，建议改用 `references/part-based-surface-mesh.md` 的流程。`GenerateSurMeshBySpitAssemblyGroupProperty` 能为不同部件组设置差异化的网格尺寸参数，比统一默认参数更合理。

## 5. 体网格与空间网格

**阶段闸门（强制）**：表面网格生成成功后，必须完成体网格块创建（`UGBlockCreate`），然后才能进入空间网格生成（`UGUGSp`）。任何时候都不得在表面网格生成后直接调用 `UGUGSp`。

此步骤为强制，不可跳过。manual 模式使用 `tool_params` 确认体创建参数；auto 模式查询默认参数后直接执行 `UGBlockCreate`。体网格块成功后，manual 模式使用 `options` 询问是否继续生成空间网格；auto 模式根据用户原始目标判断。然后按以下步骤执行。

### 5.1 场景判断

根据用户模型情况判断属于哪种场景：

| 场景 | 外场面 | 模型类型 | 前置操作 |
|---|---|---|---|
| 场景一 | 存在 | 全模 | 无 |
| 场景二 | 存在 | 半模 | 无（外场属性 + 物面属性 + 对称属性） |
| 场景三 | 不存在 | 全模 | 先通过体创建生成外场 |
| 场景四 | 不存在 | 半模 | 先设置半模边界线 → 体创建生成外场 |

半模场景需先调用 `UGHalfModelLine(cnIDs, symmetry)` 设置半模边界线。

### 5.2 外场生成规则

- 亚音速（0-1 马赫）：模型特征长度的 20 倍，外场形状一般给球形。
- 超音速（>1 马赫）：大于 1.5 倍特征长度，外场形状一般给弓形。
- 特征长度通过 `GetModelParameters` MCP 工具获取（返回 JSON 中的 `characteristic_length` 字段）。

### 5.3 体创建

1. 查询体创建所需参数：调用 `GetCreateBlockDefaultParam()`。
2. 确认参数并调用 `UGBlockCreate(geoParam, chooseParam, centerCoor, meshType, meshSizeOrDimension)` 创建体网格块。
   - `chooseParam` 的外场形状值：0=球形，1=立方体，2=圆柱，3=弓形。
3. 体网格块成功后，manual 模式使用 `options` 询问是否生成空间网格；auto 模式根据用户原始目标判断。

### 5.4 空间网格生成

如果需要空间网格：

1. 确定附面层参数：
   - 首层高度：根据 y+、雷诺数、参考长度计算。y+ 默认 1，雷诺数默认 1.5×10⁷，参考长度取 MAC。
   - 增长率：1.2（默认）。
   - 层数：40（默认）。
   - 扩散因子：粗网格 0.5 / 中等 0.8 / 细网格 0.98。
   - 单元类型：四面体（默认）。
   - **预留工具**：`CalculateFirstLayerHeight`（附面层首层高度计算）— 当前不存在，AI 按公式计算或向用户询问。
2. 确定需要反向法向量的网格面 ID（`revetId`）。
3. 调用 `UGUGSp(layer, growRate, caliperFirst, diffusionFactor, diffusionDensity, generateWay, revetId)` 生成空间网格。
4. 手动模式使用 `tool_params` 确认，自动模式使用查询或 Schema 默认参数直接执行。

## 6. 导出

1. 询问导出格式和目标文件。
2. 根据实时导出工具 Schema 获取对象类型、对象 ID、数据格式、精度和单位等参数。
3. 文件覆盖或批量导出属于有副作用操作，必须确认目标路径和影响范围。
4. 执行导出并等待返回。
5. 只根据实际返回报告导出是否成功。

