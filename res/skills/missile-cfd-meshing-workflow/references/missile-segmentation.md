# 导弹 AI 部件分割

导弹专用 AI 分割流程。对应 需求文件 模块 1.3。

## 分割类别

| ID | 英文 | 中文 | 特征 |
|----|------|------|------|
| 0 | nose | 弹头 | 球头：半径较小球面；尖锐头部：无明显边界 |
| 1 | fuselage | 弹体 | 主体圆柱/锥段 |
| 2 | wing | 弹翼 | 三角翼/梯形翼，与弹体无缝连接；**无主组**，拆为子面 |
| 3 | tail | 尾部 | 尾部收段，一般为平面 |

> 🔴 **型号事实（引用事实中心 §1）**：2026-10-08 确认**无舵、无舵轴**（无 `fin`/`finshaft`）；2026-10-09 确认**当前为三角翼**——无 `wingTip` 子面属**正常缺失**（梯形翼才有），梢部端/后缘梢侧一律按 `wingSideSurface` 自由梢端处置，**不得因 `wingTip` 缺失而误判"分组不全"**。

## 工具

- **`ClassifyMissile(serverHost, serverPort, outputDir)`** — 导弹 AI 部件分割（后端接口名 `ClassifyMissile`）
- **`GetMissilePartGroups()`** — 分割后查询各部件分组及超面 ID（无参数）

> `ClassifyMissile` **会同时输出弹翼的子面分组**（`wingLeadingEdge` / `wingTrailingEdge` / `wingSideSurface`（+`wingTip`）），**无需额外调用拆分接口**。分割完成后用 `GetMissilePartGroups` 或 `GetAllSpitAssemblyGroupProperty` 查分组。
>
> ⚠️ **兜底返回值陷阱**：`ClassifyMissile` 与 `GetMissilePartGroups` 可能返回恒定 `"true"`（无数据）——**这是接口兜底值，绝不能当作"分割成功"**，据此判定会形成**误报**。
>
> **判定"分组就绪"的唯一依据是分组查询**：`GetAllSpitAssemblyGroupProperty` 能读到 `nose` / `fuselage` / `wing*` / `tail` 等分组即视为分组就绪。由此得到两条实际处理路径：
> - 🔴 **分组已存在 → 跳过 `ClassifyMissile`**，直接进入后续阶段。无论分组来自本次自动分割、历史分割还是**用户手工划分**，只要工程里已有可用分组，就**不需要**重新跑分割。
> - **分组不存在** → 需先分割；此时若 `ClassifyMissile` 返回兜底值 / `"false"`，**先核对三个连接参数**（见本节「参数收集与确认」；端口串用是最常见原因），核对后仍失败才**停止并告知用户"分割接口不可用，无法自动分部件"**（需求文件 1.3 未满足），不得假装跑过、也不得卡在本阶段反复重试。
>
> ⚠️ **组名乱码容错**：分组名可能返回为 `U+FFFD` 乱码（后端编码问题）。**遇到无法匹配预期名（`nose`/`fuselage`/`wing*`/`tail`）的键时，不要按名匹配、不要判为"分组缺失"而跳过**——按该组 `domain[].ids` 的面 ID 归属识别，并在 `note` 中记录、提示用户核查后端输出编码。

## 参数收集与确认

1. **调用时必须传齐 `serverHost` / `serverPort` / `outputDir` 三个参数，严禁空参调用**（MCP Schema 中三者均为必填，缺参会直接校验失败）。
2. 询问远程推理服务器的 IP、端口和点云输出目录；用户未提供时使用默认值：
   - 服务器 IP：`"7.31.130.92"`
   - 服务器端口：`9009`（整数）
   - 输出目录：`"D:/A_GridStarCode/output"`
   🔴 这三个值是**本推理服务的固定配置**，直接取本节所列值。**不得从历史记忆、其他模型的配置或过往会话里取值** —— 端口尤其容易被串用，取错会连到别的服务上，表现为调用返回 `"false"` 或超时。用户明确给出具体值时以用户值为准（用户明确值 > 本节默认值 > 任何记忆值）。
3. manual 模式使用基础 `tool_params` 协议展示参数并等待用户确认；auto 模式按默认值直接执行。
4. 调用失败或超时时停止，不继续后续步骤。

## 分组查询（GetMissilePartGroups）

分割完成后，用 `GetMissilePartGroups()` 查询**当前模型已有**的导弹部件分组，返回数组：

```json
[
  {"group_name": "nose",  "face_count": 3, "face_ids": [0, 5, 12]},
  {"group_name": "wing",  "face_count": 6, "face_ids": [...]}
]
```

- `group_name`：组名（后端 4 类：`nose` / `fuselage` / `tail` / `wing`；🔴 当前型号无舵/舵轴，不再返回 `fin` / `finshaft`）
- `face_count`：该组超面数量
- `face_ids`：该组超面 ID 列表

> ⚠️ **返回数组会「重复 + 补零」，解析必须做聚合**：同一个 `group_name` 可能出现**多次** —— 其中若干条是 `face_count = 0` / `face_ids = []` 的**占位条目**，且**整段列表可能整体重复若干遍**。
> **正确做法：按 `group_name` 聚合，只取 `face_count > 0` 的那条（或对各条 `face_ids` 取并集）。**
> 🔴 **直接取第一条（如 `d[0]`）会拿到 `face_count = 0`，从而误判成"该部件没有面"** —— 这是本接口最容易踩的坑。

> **命名对应**（后端 ↔ 本项目文档）：`fuselage` = 弹体；`wing` = 弹翼（`wing*` 子面）。以实际返回为准。
>
> **与 `GetAllSpitAssemblyGroupProperty` 的分工**：`GetMissilePartGroups` 只返回"组名 + 超面 ID"；若需要每个分组的 `targetSize`/`minSize`/`angle`（表面网格 `groupProperty` 必填），仍用 `GetAllSpitAssemblyGroupProperty`。

## 翼面子面定义（分割产物，对应需求文件 1.4）

**翼不设独立主组，直接拆为以下子面**（3 个基本 + 1 个条件性）：

| 子面 | 英文 | 说明 |
|------|------|------|
| 前缘面 | wingLeadingEdge | 翼前缘弧形面（可能平面/弧面） |
| 尾缘面 | wingTrailingEdge | 翼尾缘狭长面（后缘处理使用） |
| 翼侧面 | wingSideSurface | 翼侧面（保证为平面，实际可能根部加厚） |
| 翼梢面 | wingTip | **仅梯形翼存在**（三角翼没有，🔴 当前为三角翼 → 正常缺失） |

> 若不存在明显二面角，各部件可以合并为一个面（需求文件 1.4 用词"尽量"，可降级但须留痕，见 `missile-cad-to-mesh.md` §3.1）。

## 分组归并（子组 → 主组）—— 🔴 必做

### 为什么必须做
后端 `GetMissilePartGroups` / `GetMissileDimensions` 按 **4 类主组名精确匹配**（`nose` / `fuselage` / `tail` / `wing`）。
而 AI 分割 / 手动分组产生的是**子面级组名**（`wingLeadingEdge` …）→ **匹配不上**，后果是 **`GetMissileDimensions` 的弹翼 2 项（`wing_root_chord` / `wing_tip_chord`）与半翼展全为 `0`**。

**必须把子组归并到主组**，后端才算得出弹翼尺寸。

### 归并映射
| 现有组（前缀匹配） | 归入主组 | 说明 |
|---|---|---|
| `nose` | `nose` | 已一致，跳过 |
| `tail` | `tail` | 已一致，跳过 |
| `fuselage` | `fuselage` | 弹体 |
| `wing*`（`wingLeadingEdge` / `wingSideSurface` / `wingTrailingEdge` / `wingTip`） | **`wing`** | **弹翼** |

⚠️ 前缀语义：`wing*` = 弹翼。🔴 当前型号无舵、无舵轴，不存在 `fin*` / `finshaft` 分组。

### 前置检查：分组是否已就绪（已就绪则**跳过**本阶段）

先调 `GetAllSpitAssemblyGroupProperty` 判断。**同时满足以下全部条件即可直接跳过归并**：

1. 分组名**恰为 4 个主组**（`nose` / `fuselage` / `tail` / `wing`）；
2. **不存在子面级组名**（`wing*`），也**不存在乱码组**；
3. 各组面 ID 合集**覆盖全部数模面**（与 `GetAllObjectByType(2)` 的数量核对）；
4. 抽查验证：`GetMissileDimensions()` **7 项全部有值、无 `0`**。

> ✅ **用户手动分组同样有效** —— 判定"分组已就绪"的依据是**分组查询结果**（主组齐备、面 ID 全覆盖），与分组来自自动分割还是用户手工操作无关；手动分组后同样可由 `GetMissileDimensions` 取到全部尺寸。
> 若选择手动分组，**务必在分组前记录子面 ID 台账**（各向异性 / 后缘处理据此按面 ID 定位）。

### 执行步骤
1. **读分组 + 记台账**：`GetAllSpitAssemblyGroupProperty()` 读全部分组；**同时记录各子组的面 ID**（如 `wingLeadingEdge=[6,20]`、`wingTrailingEdge=[40,48,69,82]`），供后续按 ID 定位。
2. **归类汇总**：按上表把每组映射到目标主组，汇总主组应含的面 ID，并**剔除 `0`**（`wingTrailingEdge=[0,18]` 的 `0` 是占位，见主 SKILL「ID 0 过滤规则」）。
3. **建组 + 搬迁**（每个主组一遍）：
   - `UGSpitAssemblyCreateNewGroup(groupName=<主组名>, strDiagon, strDiagonMin, strAngle)`（主组已存在时调用无害）
   - `UGSpitAssemblyMoveNodesToNewGroup(selectType=1, groupName=<主组名>, ids="面ID,逗号分隔")`
   - 🔴 **`selectType` 必须传整数 `1`**（超面）——**传字符串 `"1"` 会被当成超边**，面会被错误地放进 `line` 而非 `domain`，**归并不生效**。超边同理用整数 `0`。
4. **验证**：再调 `GetMissileDimensions()`，弹翼 2 项 + 半翼展**不再为 `0`** 即成功。

### 组名乱码时的处理
若组名出现不可逆乱码→ **无法按名归并**：
- 可按 **ID 分布**判断归属（与已识别组**互补的范围**是有力线索，如 `31–90` 补集即另一子部件的其余面）；
- 但**不得武断**：记入 `note` 并**请用户在界面确认这两组的实际归属**后再归并。

## 归并的代价与台账（🔴 各向异性 / 后缘 按 ID 驱动）

### 代价
**GridStar 中一个超面只能属于一个分组** —— 归并是"**搬迁**"，**会失去原子面归属（子面区分丢失）** → **各向异性处理与后缘处理不能再按组名定位**。应对：归并前记录**子面 ID 台账**，这些阶段按 **面 ID** 驱动（相关工具本就接受面 ID，组名只是"取 ID 的键"）。需求文件 4.1 允许**不做各向异性**，则无影响即可跳过。

### 台账方案（优先路径）
**归并后各向异性与后缘处理的定位源 = 台账或子面分组，二者满足其一即可**（并非要求子面分组存在——归并后它必然不存在）：

1. **台账（有归并前记录 / 用户手动分组时同步记录）**：按台账面 ID 驱动各位置处理（前缘 / 侧面 / 后缘线的归属判断仍按 `missile-anisotropic-mesh.md` 及各子 Skill 的判据，只是**取面 ID 的键 = 台账而非子面分组名**）。
2. **台账缺失 → 子面分组缺失**：按「不强行推理纪律」（事实中心 G1）`skipped` + `note`，**不得几何反推、不得试探性调用**。

> 各各向异性/后缘子 Skill 的前置条件已按"台账 or 子面分组 二择一"同步更新。

### 台账的更新纪律
**碎面合并（阶段 4）会改动超面集合（ID 与数量都可能变化）**——若归并前已记台账，合并完成后**必须重查 `GetAllSpitAssemblyGroupProperty` 并更新台账**，否则后续按旧 ID 定位会落空。

### 无台账时的处置（非默认路径，仅限强线索）
几何反推仅在有台账线索、能一两次查准的轻量场景作辅助；**不要变成长时间探索**：
- 对 `wing` 组每个网格面，调 `GetConnectorsByDomains` 取其网格线 → `GetStartAndEndPointByConnectors` 取端点坐标 → 按坐标特征分类：

| 子面 | 判据（本模型：弹轴 = X，翼厚度 = Y） | 对照 |
|---|---|---|
| 后缘面 `wingTrailingEdge` | 垂直于弹轴 → **X 跨度为 0** | 面法向平行弹轴 |
| 前缘面 `wingLeadingEdge` | 横跨厚度方向 → **Y 范围跨 0** | 面横跨厚度中面 |
| 侧面 `wingSideSurface` | 位于厚度极值平面 → **Y 为单值 (±t)** | 面平行于厚度极值面 |

> 判据通用形式：垂直于弹轴的面 = 后缘；其余两类中 **横跨厚度者 = 前缘**、**位于厚度极值平面者 = 侧面**。
> 弹轴方向 = 模型包围盒的**最长跨度方向**；厚度方向由 `wing` 组各面的坐标分布确定。

## 测量方法（只读，可复用）
```python
# 取某组各面的坐标范围：面 → 网格线 → 端点
conns = GetConnectorsByDomains(domain_ids=[网格面ID])   # → {"domains":[{"domain_id":N,"connector_ids":[...]}]}
pts   = GetStartAndEndPointByConnectors(connector_ids=conns)  # → {"connectors":[{"id","start_point","end_point"}]}
# 每个 point 的 coordinates 形如 "[4108,-15,536.076]"，统计 min/max 即得面范围
```

## 头部区分处理

### 球头（ball nose）
- 特征：前端有半径较小的球面，与锥体相切
- 参数：球头半径 R，用于表面网格参数计算
- 网格参数：目标尺寸 min(0.01×D, 0.25×R)，最小尺寸 目标×1/10，自适应角 10°

### 尖锐头部（sharp nose）
- 特征：前端尖锐，无明显球面边界
- 参数：无需球头半径
- 网格参数：目标尺寸 0.005×D，最小尺寸 目标×1/10，自适应角 10°

## 外场处理

若模型含外场，单独设为部件，不归入以上部件分类。

## 前置条件

1. CAD 已导入并设置单位（unit=1 表示 mm）
2. 水密性处理已完成（自由边检查通过）
3. 上述任一条件不满足时停止

## 辅助工具

| 工具 | 用途 | 类型 |
|------|------|------|
| `UGSpitAssemblyCreateNewGroup` | 创建部件组 | [复用] |
| `UGSpitAssemblyMoveNodesToNewGroup` | 移动元素到组 | [复用] |
| `UGDelRedundantDom` | 删重复面 | [复用] |