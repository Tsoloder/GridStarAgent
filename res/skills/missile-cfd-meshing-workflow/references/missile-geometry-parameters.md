# 导弹几何尺寸参数

导弹专用几何参数体系。

## 工具

- 🔴 **MCP 工具（首选来源）**：`GetMissileDimensions()` — 无参数，注册在 `missile` 工具分组内。
  调用前需先 `enable_tool_group("missile")`（`query` / `project` 为默认已启用，`missile` 不是）。
- **内部接口（非 MCP，C++ 层）**：`SPIDER::Data::GetFuselageLength()` — 取全弹长；`GetWingRootChords()` / `GetWingThicknesses()` — 各向异性处理器按对读的尺寸数组（由 `GetMissileDimensions` 写回）

> ⚠️ `GetMissileDimensions` **是** MCP 工具。历史文档曾写作"不是 MCP 工具、不可调用"，该说法**已作废**——照此执行会放弃唯一的尺寸来源。取尺寸时**先调它**，失败或返回兜底值才回退用户输入。

## 尺寸来源

> 🔴 **2026-10-17 用户裁定：尺寸恢复为「几何实算」，来源是分割流水线**。
>    接口 `GetMissileDimensions()` 返回值仍**是唯一权威来源**，但**不再写死**——
>    它读的是 `ClassifyMissile` 第 10 步 `CalculateDimensions()` 实算并缓存的同一份表。
>    **未跑分割 → 缓存为空 → 接口返回 `{}`**（不再是固定数值）。
>
> 🔴 **调用顺序被约束**：必须先 `ClassifyMissile`（产出 `wing*N` 分组 + 实算尺寸），
>    再 `GetMissileDimensions()`（读缓存）。顺序颠倒会拿到 `{}`。
>
> 🔴 **2026-10-16 起弹翼按对**：弹翼尺寸是**数组** `wings[]`（下标 = 对数−1，每项 `root_chord` / `tip_chord` / `thickness` / `half_span`），第 N 对翼取 `wings[N−1]`；标量键只有 `wing_root_chord` / **`wing_thickness`**（= `wings[0]`，单对翼便捷入口）；🔴 旧标量键 `wing_tip_chord` / `wing_half_span` **已不再产出**。
> 🔴 **接口返值单位就是毫米（mm）**，**直接传参，禁止再 ×1000**。
> 🔴 **弹翼尺寸按对取**（`wings` 数组，2026-10-16 新增）。

| 尺寸 | 来源 | 说明 |
|------|------|------|
| 全弹长 fuselage_length | `GetMissileDimensions()` / 用户输入 | 🔴 接口返 **mm**，直接用 |
| 弹径 D (body_diameter) | `GetMissileDimensions()` | 头锥/弹体交接处口径；球头公式与各向异性用它 |
| **表面网格有效弹径** body_diameter_surface | `GetMissileDimensions()` 返回的同名字段 | 🔴 **表面网格非球头部分用此值**；失配 >70% 时为弹尾口径（见下） |
| 弹尾口径 body_diameter_tail | `GetMissileDimensions()` | 失配判据来源；弹尾尺寸 |
| 翼根弦长 C_root(N) | `GetMissileDimensions()` → `wings[N−1].root_chord` | 第 N 对弹翼表面网格尺寸（0.02×）/ 各向异性**中间值**（🔴 **按对取**，不得各对共用） |
| 🔴 **翼厚度 T(N)** | `GetMissileDimensions()` → `wings[N−1].thickness` | **各向异性首层高度/端间距基准**（`0.1×`，2026-10-22 起）；= 该对翼根部尾缘带沿厚度方向的跨度 |
| 翼梢弦长 C_tip(N) | `GetMissileDimensions()` → `wings[N−1].tip_chord` | 🔴 **按翼型分叉**（事实中心 M2）：三角翼（无 `wingTipN`）≈0，禁止 0 代入、梢部端中间值钳制到该对 C_root(N) 计算值；梯形翼（有 `wingTipN`）正常使用 |
| 半翼展 b/2(N) | `GetMissileDimensions()` → `wings[N−1].half_span` | 第 N 对展向分布跨度（🔴 仅 Agent 侧报告；后端不参与尺寸计算） |
| 球头半径 R (nose_radius) | `GetMissileDimensions()` / 用户输入 | 🔴 接口返 **mm**，直接用；`>0` 球头 / `==0` 尖头（M3） |

### 🔴 表面网格弹径取值 `body_diameter_surface`（2026-10-14 用户裁定）

> 原文："表面网格生成中，导弹的几何参数弹径和弹尾口径差距大的时候，表面网格生成**除了球头公式**，弹径 D 就用弹尾口径替代来生成更合理。**球头的网格生成还是用原本的弹径和球头半径参数公式计算**。"

| 情形 | 表面网格（非球头部分） | 球头公式 |
|------|------------------|---------|
| `\|D − D_tail\| > 0.7 × D_tail`（失配 >70%，2026-10-16 由 20% 改定） | 🔴 **弹尾口径** | 🔴 **仍用原始 D + R**，不受影响 |
| 差异 ≤70% / 尾口径非法 | 弹径 D | D + R |

- 接口**已直接给出** `body_diameter_surface`，**取字段即可，不必自判**；字段缺失/为 0 时按上表自判。
- 球头、弹体二面角加密等**是否也用尾口径**：🔴 用户仅指定"表面网格生成（球头公式除外）"，**球头公式明确例外**；其余表面网格尺寸按上表用 `body_diameter_surface`。

## 使用原则

1. 🔴 **接口返回值唯一权威**（事实中心 §2）——D / `wings[N−1].*` / R / 全弹长**一律直接用接口值**，**无任何固定值例外**
2. **`GetMissileDimensions` 是 MCP 工具** — 需先 `enable_tool_group("missile")`。🔴 **返回 `{}` 有两种含义**：
   - **未跑过 `ClassifyMissile`**（尺寸缓存为空，2026-10-17 起）→ **先跑分割**再取尺寸
   - 分组缺失（分割跑过但没产出可用分组）→ 排查分割
   返回恒定 `"true"` 同样为兜底值，不得当作成功
3. 🔴 **单位**：接口返回**毫米（mm）**——**直接传参，禁止 ×1000**
4. **各向异性处理器内部自动计算** MAC — 调用 `MissileWingAnisoProcessWingAnisotropy()` 后，返回结果中包含步骤 2 的每对弹翼 MAC 值
5. 🔴 **各向异性处理器依赖接口**：一站式工具后端按对读 `SPIDER::Data` 的 `m_wingRootChords` / `m_wingThicknesses`（由 `GetMissileDimensions` 写回），**未调接口就调它会 `failed`** → 各向异性前必须先取尺寸
6. 🔴 **顺序约束（2026-10-17 起，2026-10-22 沿用）**：`ClassifyMissile` → `GetMissileDimensions` → `MissileSurfMeshGenerate` → `MissileWingAnisoProcessWingAnisotropy`。尺寸由分割流水线实算并缓存，跳过分割会拿到空表

### 接口未覆盖的量

| 量 | 来源 | 当前状态 |
|---|------|---------|
| **平均气动弦长 MAC** | **各向异性处理器自动计算** | `MissileWingAnisoProcessWingAnisotropy()` 返回结果中步骤 2 每对弹翼包含 MAC 值 |
| 🔴 **翼厚度 T(N)** | `GetMissileDimensions()` → `wings[N−1].thickness` | 2026-10-22 新增；各向异性首层/端间距基准（`0.1×`） |
| 特征长度 | 需求文件 2.1："特征长度：软件自行计算" | 软件内部计算，**agent 不传入** |

## 参数依赖关系

```
body_diameter (D) ──┬── 头部网格参数（球头 min(0.01×D, 0.25×R)）
                    ├── 弹体网格参数（0.05×D）
                    ├── 尾部网格参数（0.05×D）
                    └── 弹体二面角加密（0.002×D）

body_diameter_surface ──── 表面网格（非球头部分）用的口径
                           （|D − D_tail| > 0.7×D_tail 时 = 弹尾口径，否则 = D）
                           🔴 球头公式不用它，仍用原始 body_diameter + nose_radius

nose_radius (R) ───────── 球头网格参数（min(0.01×D, 0.25×R)）

wings[N−1].root_chord ──────── 弹翼表面网格参数（0.02×该对根弦长）
wings[N−1].thickness  ──────── 🔴 弹翼各向异性首层高度/端间距（0.1×该对厚度，2026-10-22 起）
wings[N−1].root_chord ──────── 弹翼各向异性中间值（0.02×该对根弦长）
                              （🔴 **按对取**：`wingSideSurface2` → `wings[1]`，不得各对共用）
                              （🔴 C_tip **按翼型分叉**（M2）：三角翼≈0 → 梢部端中间值钳制到该对 C_root 计算值、不代入 0；梯形翼正常使用）

wings[N−1].half_span (b/2) ── 弹翼展向分布（线性插值 C_local 的跨度，按对取）

fuselage_length ─────────────── 弓形外场（外场长度 = 1.5×弹体长度，另外 3 位 = 1.2×弹体长度）

body_diameter (D) ────── 弓形外场头部距离（0.2×D）
```

## 参数获取与验证

尺寸获取路径（按优先级）：

1. **`GetMissileDimensions()`（MCP，🔴 唯一权威来源）** — 需先 `enable_tool_group("missile")`，且**必须先跑过 `ClassifyMissile`**（尺寸由分割流水线实算缓存）。一次拿全 `fuselage_length` / `nose_radius` / `body_diameter` / `body_diameter_tail` / `body_diameter_surface` / 🔴 **弹翼 `wings` 数组**（每项 `root_chord` / `tip_chord` / **`thickness`** / `half_span`，下标 = 对数−1），单位 mm
2. **各向异性处理器** — `MissileWingAnisoProcessWingAnisotropy()` 无参数，内部**自行**取尺寸：全部从 `SPIDER::Data` 读（由第 1 步写回），🔴 **弹翼按对读 `m_wingRootChords[N−1]` / `m_wingThicknesses[N−1]`**（旧文所述"直读尺寸缓存 `wings`、不经 `SPIDER::Data` 中转"**已作废**）；算完 MAC 返回 JSON 中 `pairs[].mac` 等字段。🔴 **未调接口 → 按对数组为空 → 直接 `failed`**
3. **内部接口（非 MCP）** — `SPIDER::Data::GetFuselageLength()` / `GetWingRootChords()` / `GetWingThicknesses()`（C++ 层）
4. ~~历史标定值~~ — 🔴 **已作废**（2026-10-14），禁止引用
5. **用户输入** — 球头半径 R、其他接口缺失尺寸

### 关键约束

- D（弹径）：取接口值，**禁止由分组属性 `targetSize` 反推**
- C_root(N)（第 N 对翼根弦长）：🔴 **按对取接口值** `wings[N−1].root_chord`（N 从组名末位数字读出）——不得用某一对的 C_root 代跑其他对
- 🔴 **T(N)（第 N 对翼厚度）**：**按对取接口值** `wings[N−1].thickness`——**各向异性首层/端间距的唯一基准**（`0.1×`），2026-10-22 起取代"当地弦长加密尺寸"
- C_tip(N)（第 N 对翼梢弦长）：🔴 **按对取** `wings[N−1].tip_chord`，并**按翼型分叉**（事实中心 M2）——三角翼≈0，**禁止 0 代入**，梢部端中间值钳制到**该对** C_root 计算值；梯形翼按接口值正常使用
- 半翼展 b/2(N)：🔴 **按对取接口值** `wings[N−1].half_span`（仅报告字段，后端不参与尺寸计算）
- 🔴 **单位**：`GetMissileDimensions()` 返回**毫米（mm）**（几何坐标系本就是 mm，后端不做换算）；**直接传参，禁止 ×1000**

## 几何量的唯一来源（反推禁令）

| 数量 | 来源 | ❌ 禁止来源 |
|---|---|---|
| D（弹径） | `GetMissileDimensions()` | 由分组属性 `targetSize` 反推 |
| D_tail / 表面网格有效弹径 | `GetMissileDimensions()` | 反推 |
| R（球头半径） | `GetMissileDimensions()` / 用户输入 | 反推 |
| 全弹长 | `GetMissileDimensions()` / 内部接口 / 用户输入 | 任何非导弹几何查询工具 |
| 各对当地弦长 C_root(N) / C_tip(N) | `GetMissileDimensions()` 的 **`wings[N−1]`**（按对） | 由分组属性 `targetSize`×10 或 `0.02×弹径` 反推 |
| 🔴 **各对翼厚度 T(N)** | `GetMissileDimensions()` 的 **`wings[N−1].thickness`**（按对） | 反推 / 用弦长比例代替 |
| 各对半翼展 b/2(N) | `GetMissileDimensions()` 的 **`wings[N−1].half_span`**（按对） | 由分组属性反推 |
| MAC | 各向异性处理器自动计算（`MissileWingAnisoProcessWingAnisotropy` 返回） | 用全弹长/弦长充当真值 |

> ⚠️ **为什么禁止反推**：分组属性 `targetSize` / `minSize` 的比值并不一致；按 `0.05×D` 或 `0.1×D` 反推会得到**相差 2 倍**的 D，直接带偏 `bodySpacing`/`rootSpacing`/各向异性尺寸。**分组属性只用于表面网格的 `groupProperty` 传参。**

## 前置条件

1. 部件分组已存在（用 `GetAllSpitAssemblyGroupProperty` 验证）。
2. 上述条件不满足时停止

## 当地弦长使用规则

所有 Skill 和 reference 中出现的"当地弦长"遵循以下规则：

🔴 **一切弦长都按对取**：第 N 对翼的弦长下标一律为 `wings[N−1]`（`C_root(N) = wings[N−1].root_chord`、`C_tip(N) = wings[N−1].tip_chord`）。N 由**翼性子面组名末位数字**读出（`wingSideSurface2` → 第 2 对）。**不得用某一对的弦长代跑其他对**——各对弦长可差 5 倍以上。

| 出现位置 | 使用 C_root(N)（根部弦长） | 使用 C_tip(N)（梢部弦长） | 🔴 三角翼（无 `wingTipN`，C_tip≈0） |
|---------|----------------------|----------------------|----------------------|
| **表面网格（模块 3.2）弹翼 0.02×** | **该对整组取 C_root(N)** | 不适用（分组为整组，无法按展向分段） | 该对整组取 C_root(N) |
| 🔴 **各向异性首层高度 / 端间距（0.1×）** | **不适用——一律用 `wings[N−1].thickness`（该对翼厚度）** | 同左（**厚度基准无翼型分支**） | 同左 |
| 🔴 **各向异性 mindValue（0.02×）** | 根部段用 C_root(N) | 梢部段用 C_tip(N) | 一律 C_root(N)（0.02×C_root(N)） |
| 弦向分布中间值（0.02×） | 根部段用 C_root(N) | 梢部段用 C_tip(N) | 一律 C_root(N)（0.02×C_root(N)） |
| 后缘面 rootSpacing（0.02×，靠近根部端） | 用 C_root(N) | — | 用 C_root(N)（0.02×C_root(N)） |
| 中间区域 | 线性插值：C_local = C_root(N) + (C_tip(N) − C_root(N)) × (x/L_span) | | 🔴 此处 L_span = **该对**半翼展 `wings[N−1].half_span`（接口值）；三角翼 C_local 退化：C_local = C_root(N) × (1 − x/(b/2)(N))（尖点处→0，中间值钳制见上） |

> 🔴 **2026-10-22 口径变更**：旧表中所有 `0.005×当地弦长` 的**首层/端间距**行（弦向前缘端/后缘端、展向梢部端/根部端 edgeSpacing、后缘面 bodySpacing）**已全部作废**——各向异性首层与端间距一律 = **`0.1 × wings[N−1].thickness`**，**与弦长及翼型均无关**。仅"中间值 `0.02×弦长`"类仍按本表取弦长。
>
> **最后一列只在三角翼生效**。梯形翼（有 `wingTipN`）一律按前两列的常规规则计算——C_tip(N) 取接口值，"梢部端"就是真实梢部弦长，**不做钳制**。翼型判定见事实中心 M2。
>
> 🔴 **弹尾加密不在本流程**（2026-10-22）：旧一站式的 step6 弹尾加密已随 `MissileAnisoProcessMissileAnisotropy` 弃用，**本流程不处理弹尾各向异性**，弹尾由后续独立工具负责。
>
> 三角翼的"当地弦长"定义：从根部 C_root(N) **线性减小至翼尖尖点处为 0**。公式可算但**尖点附近中间值不得取 0**——所有「`0.02×当地弦长`」的梢部端一律钳制到**该对** C_root(N) 的比例值（`0.02×C_root(N)`，**按接口现算**）。`x/(b/2)(N)` 跨度用**该对**接口半翼展值。

- **弹翼（`wing*` 分组）**：🔴 **C_root(N) / T(N) 按对取接口值**（`wings[N−1].root_chord` / `wings[N−1].thickness`，事实中心 §2）；🔴 **C_tip(N) 按对取并按翼型分叉**（M2 / §2.2.2）——三角翼（无 `wingTipN`）≈0 正常，禁止 0 代入、梢部端中间值钳制到**该对** C_root(N) 计算值；梯形翼（有 `wingTipN`）按接口值正常使用
