# 关键几何参数计算

## 1. 参数获取方式与优先级

**自动分割提取 > 用户输入 > AI 测量 > 文档默认值**

- 运行「部件分割」流程后，四个尺寸值自动写入 `Data` 对象，可通过 Getter 直接读取。
- 用户未运行部件分割时，优先询问用户是否已知翼根弦长 C_root、翼尖弦长 C_tip、机翼展长 b（或半展长 b/2）等参数。
- 用户未提供时，尝试通过工具测量。
- 工具不可用时，使用文档默认值并向用户说明。
- **`GetModelParameters` 返回 0.0 时，表示对应参数未自动计算，应回退至文档默认值。**

## 1.1 自动分割提取（软件原生参数）

运行 `SegmentPart`（部件分割）MCP 工具后，`CalculateDimensions` 从点云和分割结果计算出四个尺寸值，并通过 `Data` 类的公开接口持久化存储。

| 尺寸名称 | Data Getter | 说明 |
|---|---|---|
| 机身长度 | `data->GetFuselageLength()` | 机头到机尾在主方向上的跨度 |
| 机翼半展长 | `data->GetWingHalfSpan()` | 翼梢到翼根的距离（全模型取半、半模型取全） |
| 翼根弦长 | `data->GetWingRootChord()` | 翼根位置前后缘在主方向上的距离 |
| 翼尖弦长 | `data->GetWingTipChord()` | 翼尖组 (`jiyi_wing_tip`) 在主方向上的跨度 |

### 访问方式

在 C++ 代码中：
```cpp
Data* data = App::GetInstance()->GetBaseData();
double L    = data->GetFuselageLength();
double b_2  = data->GetWingHalfSpan();
double c_r  = data->GetWingRootChord();
double c_t  = data->GetWingTipChord();
```

在 Python/Script 脚本中通过调用 `workflows/run` 或后续 MCP 工具（如 `GetPartDimensions`，预留）读取。

> **前提条件**：必须先运行 `SegmentPart`（部件分割）流程，否则四个值均为 0.0。
>
> **回退策略**：若 `GetModelParameters` 返回全 0 或部分为 0，使用 §1.2 文档默认值替代。

## 1.2 文档默认参数值

当 `GetModelParameters` 返回 0（未自动计算）且用户未提供、工具不可用时，使用以下默认值作为估算依据。这些默认值来源于某典型飞行器模型，仅作参考，实际值与具体模型相关。

| 尺寸名称 | 符号 | 默认值 | 单位 |
|---|---|---:|---|
| 机身长度 | L | 1191.98 | mm |
| 翼根弦长 | C_root | 203.19 | mm |
| 翼尖弦长 | C_tip | 60.58 | mm |
| 机翼半展长 | b/2 | 586.10 | mm |
| 平均气动弦长 | MAC | 144.74 | mm |
| 特征长度 | - | 1680.28 | mm |

> **使用规则**：`GetModelParameters` 返回的 JSON 中，某个参数值为 0.0 时，用上表对应默认值替代。MAC 的默认值可直接使用，也可用 C_root、C_tip 默认值按公式重新计算。

### 默认值验证

- MAC 默认值 144.74 可由 C_root=203.19, C_tip=60.58 计算验证：
  - λ = 60.58 / 203.19 ≈ 0.2981
  - MAC = (2/3) × 203.19 × [(1 + 0.2981 + 0.2981²) / (1 + 0.2981)] ≈ 144.74

## 2. MAC（平均气动弦长）计算

### 2.1 计算公式

1. 计算尖根比：λ = C_tip / C_root
2. 计算 MAC：MAC = (2/3) × C_root × [(1 + λ + λ²) / (1 + λ)]

### 2.2 计算示例

已知 C_root = 195, C_tip = 41：

1. λ = 41 / 195 ≈ 0.210256
2. (1 + λ + λ²) = 1 + 0.210256 + 0.044208 ≈ 1.254464
3. (1 + λ) = 1.210256
4. [(1 + λ + λ²) / (1 + λ)] ≈ 1.254464 / 1.210256 ≈ 1.036536
5. (2/3) × C_root ≈ 130
6. MAC ≈ 130 × 1.036536 ≈ 134.75

## 3. 当地弦长

当地弦长在展向各截面不同：
- 翼根处 = C_root
- 翼尖处 = C_tip
- 沿展向线性变化

若无更精确数据，可近似取 MAC 作为机翼代表性弦长。

## 4. 关键尺寸测量清单

> **自动工具**：`GetModelParameters` — 获取所有已自动计算的几何参数。需先运行部件分割（`SegmentPart`）方可获取翼根弦长、翼尖弦长、机翼半展长、机身长度；特征长度（包围盒对角线）和 MAC 在任何状态下均可计算。
>
> **预留工具**：`MeasureDistance`（两点距离测量）和 `GetPointOnSurface`（获取数模点坐标）— 当前 MCP 工具列表中尚不存在。在工具就绪前，优先使用 `GetModelParameters` 或向用户询问参数。

| 尺寸名称 | 测量方法 | 用途 |
|---|---|---|
| 机身长度 L | `GetModelParameters` 自动获取 | 参考 |
| 翼根弦长 C_root | `GetModelParameters` 自动获取 | MAC 计算 |
| 翼尖弦长 C_tip | `GetModelParameters` 自动获取 | MAC 计算 |
| 机翼半展长 b/2 | `GetModelParameters` 自动获取 | MAC 计算、各向异性尺寸 |
| 特征长度 | `GetModelParameters` 自动获取（包围盒对角线） | 外场大小、附面层计算 |
| MAC | `GetModelParameters` 自动计算 | 网格参数推导 |

所有参数均可通过 `GetModelParameters` MCP 工具一站式获取。


## 5. 部件网格参数表

部件网格参数表（含目标尺寸、最小尺寸、曲率自适应角度）参见 SKILL.md "知识库参数体系" 章节。使用 `GenerateSurMeshBySpitAssemblyGroupProperty` 时，各分部件组的目标尺寸和最小尺寸应参照该表。

## 6. 工具调用顺序

### 路径 A：已运行部件分割（推荐）

1. 确认用户已运行「部件分割」（`SegmentPart`）。
2. 调用 `GetModelParameters` 工具，一次性获取 C_root、C_tip、b/2、L、MAC 和特征长度。
3. 检查返回值中是否有 0.0 字段，若有则用 §1.2 文档默认值替代。
4. 用户可确认数值是否有误。
5. 根据 MAC 推导部件网格参数表。
6. 将参数传入 `GenerateSurMeshBySpitAssemblyGroupProperty` 或 `UGSur`。

### 路径 B：未运行部件分割

1. 调用 `GetModelParameters` 获取特征长度（如为 0.0 则用默认值 1680.28 mm）。
2. 询问用户是否已知 C_root、C_tip、b 等参数。
3. 用户未提供时，尝试使用 [`MeasureDistance`] + [`GetPointOnSurface`] 测量。
4. 工具不可用时，使用 §1.2 文档默认值并向用户说明。
5. 根据 MAC 或已知参数推导部件网格参数表。
6. 将参数传入 `GenerateSurMeshBySpitAssemblyGroupProperty` 或 `UGSur`。

方括号 `[]` 表示预留工具，当前需用户手动替代或通过询问获取。
