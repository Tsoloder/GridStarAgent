# 导弹 AI 部件分割

导弹专用 AI 分割流程。

## 分割类别

| ID | 英文 | 中文 | 特征 |
|----|------|------|------|
| 0 | nose | 弹头 | 球头：半径较小球面；尖锐头部：无明显边界 |
| 1 | fuselage | 弹体 | 主体圆柱/锥段 |
| 2 | wing | 弹翼 | 三角翼/梯形翼，与弹体无缝连接；**无主组**，拆为子面 |
| 3 | tail | 尾部 | 尾部收段，一般为平面 |

> 🔴 **型号事实（引用事实中心 §1）**：翼型**运行时探测**——**`wingTipN` 存在 = 梯形翼，不存在 = 三角翼**（M2）。三角翼下无 `wingTipN` 属**正常缺失**，梢部端/后缘梢侧一律按 `wingSideSurfaceN` 自由梢端处置，**不得因 `wingTipN` 缺失而误判"分组不全"**。

## 工具

- **`ClassifyMissile(serverHost, serverPort, outputDir)`** — 导弹 AI 部件分割（后端接口名 `ClassifyMissile`）
- **`GetMissilePartGroups()`** — 分割后查询各部件分组及超面 ID（无参数）

> `ClassifyMissile` **会同时输出弹翼的带序号子面分组**（`wingLeadingEdgeN` / `wingTrailingEdgeN` / `wingSideSurfaceN`（+`wingTipN`），N = 第几对弹翼），**无需额外调用拆分接口**。分割完成后用 `GetMissilePartGroups` 或 `GetAllSpitAssemblyGroupProperty` 查分组。
>
> 🔴 **本工具同时产出几何尺寸（2026-10-17）**：流水线第 10 步 `CalculateDimensions()` 会从点云与分割结果**实算**全弹长 / 弹径 / 弹尾口径 / 球头半径 / 各对翼 `wings[]`（弦长、**厚度**与半翼展），并缓存供 `GetMissileDimensions()` 读取。因此**必须先跑本工具，`GetMissileDimensions()` 才有值**（未跑 → 返回 `{}`）。分组已存在而跳过 `ClassifyMissile` 时，若尺寸缓存同样为空，则需补跑一次分割才能取到尺寸。
>
> ⚠️ **兜底返回值陷阱**：`ClassifyMissile` 与 `GetMissilePartGroups` 可能返回恒定 `"true"`（无数据）——**这是接口兜底值，绝不能当作"分割成功"**，据此判定会形成**误报**。
>
> **判定"分组就绪"的唯一依据是分组查询**：`GetAllSpitAssemblyGroupProperty` 能读到 `nose` / `fuselage` / `wing*` / `tail` 等分组即视为分组就绪。由此得到两条实际处理路径：
> - 🔴 **分组已存在 → 跳过 `ClassifyMissile`**，直接进入后续阶段。无论分组来自本次自动分割、历史分割还是**用户手工划分**，只要工程里已有可用分组，就**不需要**重新跑分割。
> - **分组不存在** → 需先分割；此时若 `ClassifyMissile` 返回兜底值 / `"false"`，**先核对三个连接参数**（见本节「参数收集与确认」；端口串用是最常见原因），核对后仍失败才**停止并告知用户"分割接口不可用，无法自动分部件"**，不得假装跑过、也不得卡在本阶段反复重试。
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
  {"group_name": "wingSideSurface1",  "face_count": 2, "face_ids": [...]}
]
```

- `group_name`：组名（后端：`nose` / `fuselage` / `tail` + 弹翼**带序号子组**；🔴 **不返回 `wing` 主组**）
- `face_count`：该组超面数量
- `face_ids`：该组超面 ID 列表

> ⚠️ **返回数组会「重复 + 补零」，解析必须做聚合**：同一个 `group_name` 可能出现**多次** —— 其中若干条是 `face_count = 0` / `face_ids = []` 的**占位条目**，且**整段列表可能整体重复若干遍**。
> **正确做法：按 `group_name` 聚合，只取 `face_count > 0` 的那条（或对各条 `face_ids` 取并集）。**
> 🔴 **直接取第一条（如 `d[0]`）会拿到 `face_count = 0`，从而误判成"该部件没有面"** —— 这是本接口最容易踩的坑。

> **命名对应**（后端 ↔ 本项目文档）：`fuselage` = 弹体；`wing*N` = 弹翼带序号子面。以实际返回为准。
>
> **与 `GetAllSpitAssemblyGroupProperty` 的分工**：`GetMissilePartGroups` 只返回"组名 + 超面 ID"；若需要每个分组的 `targetSize`/`minSize`/`angle`（表面网格 `groupProperty` 必填），仍用 `GetAllSpitAssemblyGroupProperty`。

## 翼面子面定义（分割产物）

**翼不设独立主组，直接拆为以下子面**（3 个基本 + 1 个条件性）：

| 子面 | 英文 | 说明 |
|------|------|------|
| 前缘面 | wingLeadingEdgeN | 翼前缘弧形面（可能平面/弧面）；N = 第几对弹翼 |
| 尾缘面 | wingTrailingEdgeN | 翼后缘面（各向异性阶段一并加密） |
| 翼侧面 | wingSideSurfaceN | 翼侧面（保证为平面，实际可能根部加厚） |
| 翼梢面 | wingTipN | **仅梯形翼存在**（🔴 **按 M2 探测**：无此分组 = 三角翼，属正常缺失） |

> 🔴 **带序号命名**：N = 第几对弹翼（1 / 2 / 3），如 `wingSideSurface1`、`wingLeadingEdge2`。弹翼对数由现有分组自动检测（检测 `wingSideSurface1` ~ `wingSideSurface3`），最多支持 3 对。
> 🔴 **这个 N 同时是尺寸下标**：第 N 对翼的一切弦长/展长取 `wings[N−1]`（事实中心 §2.1）——`wingSideSurface2` → `wings[1]`，**不得各对共用第 1 对的弦长**。
> **后端只产出这套带序号的子组名，不存在 `wing` 主组** —— 子组名即为定位键，各向异性/后缘阶段直接按组名取面 ID（见事实中心 G2）。
> 若不存在明显二面角，各部件可以合并为一个面（用词"尽量"，可降级但须留痕，见 `missile-cad-to-mesh.md` §3.1）。

## 子面分组即定位键（🔴 无需归并）

**后端分组体系 = `nose` / `fuselage` / `tail` + 弹翼带序号子组**。弹翼**不设 `wing` 主组**，`wingLeadingEdgeN` / `wingSideSurfaceN` / `wingTrailingEdgeN` / `wingTipN` 这些子组本身持有面 ID，直接就是定位键。

因此**不需要任何"子组 → 主组"归并操作**，也不存在"归并后子面组名被吞"的问题。各向异性阶段（阶段 7，含弹翼后缘面）一律：

```
groupId = GetAllSpitAssemblyGroupProperty() 中 ["wingSideSurfaceN"].domain[].ids
```

> ⚠️ **碎面合并（阶段 4）会改动超面集合**（ID 与数量都可能变化）——所以**每次进入各向异性阶段前重查一次分组**，取当前有效的面 ID，不沿用早前查到的值（事实中心 G2）。

## 测量方法（只读，可复用）
```python
# 取某组各面的坐标范围：面 → 网格线 → 端点（🔴 批量一次拿全，禁止逐面循环）
conns = GetConnectorsByDomains(domain_ids=<该组全部面ID>)   # → {"domains":[{"domain_id":N,"connector_ids":[...]}, ...]}
pts   = GetStartAndEndPointByConnectors(connector_ids=[c for d in conns["domains"] for c in d["connector_ids"]])  # → {"connectors":[{"id","start_point","end_point"}]}
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