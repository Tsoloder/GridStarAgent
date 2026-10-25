# 导弹 CAD → 网格全流程

导弹专用 CAD 导入到网格导出全流程参数。

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

1. 取公差：**用户明确指定公差时，直接以其为准**（不再查询默认值）；未指定时调用 `GetDealWatertightTolenrance()` 获取默认公差值。参数优先级为 **用户明确值 → 默认查询值**（与 §5、§4 的同类写法保持一致）。
2. 用获取的公差调用 `DealWatertight`
3. 检查返回值中的 `has_free_edges` 字段判断是否还有自由边
4. 若存在自由边，公差 ×5 重试
5. 最多递增 2 次（共 3 次调用），若仍未处理任何自由边则停止
6. 判断：
   - 无自由边 → 成功 → 进入下一流程
   - 自由边全在半模线 → 成功
   - 其他位置有自由边 → 提示模型可能有缝隙/穿插/孔洞
7. **用户修复后继续**：出现"其他位置有自由边"时停止本阶段，请用户手动修复模型；**用户处理完毕后，确认"继续执行"再进入下一流程**（manual 模式用 `options` 给出"继续 / 终止"选项；auto 模式向用户说明后等待用户确认）。

## §3 分部件与碎面合并

### §3.1 碎面合并（**条件触发**）

🔴 **触发条件（与飞机流程一致）**：用户提到**碎面、合并面、合并碎面**时，在分部件之后、表面网格之前执行本阶段。**用户未提及时本阶段不作为必经步骤**，标记 `skipped` + `note` 即可。

> ⚠️ **例外（回头补做）**：若进入表面网格后出现**不封闭**（`UGBlockCreate` 返回 `"false"` 并报不封闭线）或**网格异常偏密**，须回到本阶段补做碎面合并 —— 这两者常与 CAD 碎片同源，见 `missile-mesh-sealing-check.md`「CAD 拓扑体检」。

- **工具**：`UGSurfaceProcessing(edgeIDs, type, tolerance, minLenth)` `[复用]` — 作用于**超边**。参数语义（工具固有，以工具 docstring 为准）：`edgeIDs` = 超边 ID 串（如 `"5,6,7"`）；`type` = **-1 合并 / 2 打散 / 3 删除**；`tolerance` = **面积比**（⚠️ **不是几何容差/公差**，勿按"公差"口径理解取值）；`minLenth` = **最小边长**。这两个取值 → 按当前模型尺度**现场确定**（`minLenth` 与网格最小尺寸同量级），**不得沿用记忆或历史运行中的数值**；输入不当返回 `"false"`。

> 🔴 **唯一决策流（消除歧义，禁止无限尝试）**：
> `UGSurfaceProcessing`（换 `minLenth` 参数 **1 次**）→ 失败 → `UGDelRedundantDom`（1 次）→ 失败 → `UGDamageRepari`（1 次）→ 失败 → **跳过 + `note`**。
> 全程最多 **3 次工具调用**，**每个工具最多 1 次换参**；已知返回 `false` 时换参命中的概率极低，反复试只耗轮次、不会成功——**严禁多次换参反复重试同一工具**，各工具失败后按上表顺序走下一个，不得回跳。
> 成功后按下方"修复工具清单"的其余工具（AutoExtractConnector / UGRepairRedundantDom / CreateCoons）视情况补充，**不再穷举**。

**修复工具清单（按唯一决策流顺序；均为通用工具，导弹可用）**：

| 顺序 | 工具 | 用途 | 关键参数 |
|---|---|---|---|
| 1 | `UGSurfaceProcessing(edgeIDs, type, tolerance, minLenth)` | **碎面合并（首选主入口）** | `type` = **-1 合并 / 2 打散 / 3 删除**；`tolerance` = 面积比；`minLenth` = 最小边长；`edgeIDs` = **超边 ID** |
| 2 | `UGDelRedundantDom(selectedID, domType, precisio, overlapRatio)` | **删重面** / topo 错误面 / 内部面 | `domType` = 1 重面 / 2 topo 错误面 / 3 内部面；`selectedID` = **超面 ID** |
| 3 | `UGDamageRepari(edgeIDs, repairPattern, fillStyle)` | **碎面修复**（工具 docstring 原文即"碎面修复"） | `repairPattern` = **-1 合并 / 2 打散 / 3 删除**；`fillStyle` = 填充方式；`edgeIDs` = **超边 ID** |
| 4 | `AutoExtractConnector(ids)` / `ManualExtractConnector(ids, precision)` | **提取边界线** —— 把相邻面共用的边界重建为**一条共享线**，直接治"面之间不共享边"造成的**不封闭** | `ids` = **超边 ID**；`ManualExtractConnector` 另有 `precision` = 合并精度 |
| 5 | `UGRepairRedundantDom(selectedID, domType, precisio, overlapRatio)` | **修复**重面 / topo 错误面 / 内部面（`UGDelRedundantDom` 是**删除**版） | `domType` = 1 重面 / 2 topo 错误面 / 3 内部面；`selectedID` = **超面 ID** |
| 6 | `CreateCoons(ids)` | 用 **4 条边界重建一张曲面** —— 把碎曲面换成单面 | `ids` = **超边 ID** |
| 7 | `DeleteFC(ids, flag)` / `DeleteNbsFace(ids, flag)` | 删除数模线 / 数模面；`flag=1` 连带删除关联对象 | ⚠️ **破坏性，必须先确认工程已备份** |

> 🔴 **顺序原则**：先"**合并 / 修复**"（1–3），再考虑"**删除**"（5）；删除类操作前**必须确认已备份**。全部无效时兜底 = **回到源头 CAD 把面缝合成实体后重新导出**（见 `missile-mesh-sealing-check.md`「CAD 拓扑体检」处置）。

> ⚠️ `UGSurfaceProcessing` 作用于**超边 ID**（`type`=-1 合并 / 2 打散 / 3 删除），需有效超边集合；调用失败时本阶段跳过。**跳过时必须在 `note` 中显式记录"当前模型未合并"**，明确列出：① 头部/尾部未合并为单面；② 翼「无明显二面角时各部件合并为一个面」未执行。对应要求用词为"**尽量**"，属可接受降级，但**必须留痕**，不得含糊成"按规则跳过"——日后核对需求时才看得出缺口在哪。

> 🔴 **碎面合并会改动超面集合（合并/删除面，ID 与数量都可能变化）**：定位一律走 `GetAllSpitAssemblyGroupProperty` 按**带序号子组名**取 `domain[].ids`（事实中心 G2）。因面 ID 可能变化，**每次进入各向异性/后缘阶段前重查一次分组**，不沿用任何早前查到的面 ID。

按部件规则：

| 部件 | 合并规则 |
|------|---------|
| 头部/尾部 | 尽量合并为一个面 |
| 翼 | 前缘面、翼梢面（仅梯形翼有，**按 M2 探测 `wingTipN` 是否存在**）、尾缘面、翼侧面；无明显二面角时可合并 |
| 其他 | 保证平整，仅保留关键特征 |

辅助工具：
- `UGDelRedundantDom(selectedID, domType, precisio, overlapRatio)` `[复用]` — 删重复面
- `UGSpitAssemblyCreateNewGroup` `[复用]` — 创建部件组
- `UGSpitAssemblyMoveNodesToNewGroup` `[复用]` — 移动元素到组

详见 `references/missile-segmentation.md` 获取部件分割详细规则。

## §4 表面网格生成

详见 `references/missile-surface-mesh.md` 获取部件网格参数公式表。

- 全局：`UGSur(type, ids, targetSize, minSize, adaptAngle, way)` `[复用]`
- 按部件组：`GenerateSurMeshBySpitAssemblyGroupProperty(ids, targetSize, minSize, adaptAngle, way, groupProperty)` `[复用]`
- 补充：弹体二面角夹角处加密 0.002×D


> 网格封闭性检查与修复工具清单 → `references/missile-mesh-sealing-check.md`
> 空间网格与外场创建 → `references/missile-farfield-and-volume.md` 

# END