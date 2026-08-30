# 按分部件属性生成表面网格

分部件处理完成后（AI 自动部件分割或手动分组），根据各部件组的语义按 MAC 比例生成表面网格。本技能与 `GenerateSurMeshBySpitAssemblyGroupProperty` 工具配合使用，为不同部件组设置差异化的网格尺寸。

## 1. 前置条件

- 部件分割已完成（AI 自动 `SegmentPart` 或手动分组），分组信息已写入分部件组列表。
- MAC（平均气动弦长）已获取。通过 `GetModelParameters` MCP 工具一次性获取（返回 JSON 中的 `mac` 字段），或按 `geometry-parameters.md` 的公式手动计算。
- 如需自定义分组属性，先调用 `GetAllSpitAssemblyGroupProperty` 获取当前分组属性 JSON。

## 2. 分组名称识别规则

自动部件分割产生的分组命名可能混用中文、英文或拼音。按以下规则识别各部件组所属类别：

| 类别 | 组名关键词匹配 | MAC 比例 |
|---|---|---|
| 机头 | 含 `jitou`、`nose`、`机头`、`forebody`、`前机身` | 2% |
| 机尾 | 含 `weibu`、`tail`、`jiwei`、`机尾`、`aft`、`后机身`、`fin`、`rudder`、`尾翼`、`水平尾翼` | 2% |
| 机身 | 含 `jishen`、`fuselage`、`body`、`机身`、`中机身` | 4% |
| 机翼 | 含 `jiyi`、`wing`、`机翼`（含各子组如 `jiyi_wing_upper_surface`、`jiyi_wing_lower_surface`、`jiyi_wing_tip`、`jiyi_trailing_edge`） | 2% |
| 其他 | 以上均不匹配的组 | 4% |

## 3. 参数计算表

每组的目标尺寸和最小尺寸根据 MAC 计算：

```
目标尺寸 = MAC × ratio
最小尺寸 = 目标尺寸 / 10
曲率自适应角度 = 10°
```

| 类别 | MAC 比例 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| 机头 | 2% | `MAC × 0.02` | `MAC × 0.002` | 10° |
| 机尾 | 2% | `MAC × 0.02` | `MAC × 0.002` | 10° |
| 机身 | 4% | `MAC × 0.04` | `MAC × 0.004` | 10° |
| 机翼（含所有子组） | 2% | `MAC × 0.02` | `MAC × 0.002` | 10° |
| 其他 | 4% | `MAC × 0.04` | `MAC × 0.004` | 10° |

## 4. 执行流程

### 4.1 获取分组属性

调用 `GetAllSpitAssemblyGroupProperty` 获取当前所有分部件组的属性 JSON。返回格式示例：

```json
[
  {"jiyi_wing_upper_surface": {
    "line": [{"targetSize":12.34,"minSize":56.78,"angle":56.78,"ids":[1,2,3]}],
    "domain": [{"targetSize":6.7,"minSize":33.44,"angle":56.78,"ids":[1,2,3]}]}
  },
  {"jishen": {
    "line": [{"targetSize":12.34,"minSize":56.78,"angle":56.78,"ids":[1,2,3]}],
    "domain": [{"targetSize":6.7,"minSize":33.44,"angle":56.78,"ids":[1,2,3]}]}
  }
]
```

### 4.2 更新各分组参数

对 JSON 中的每个分组对象，根据其组名（对象的 key）按第 2 节的规则匹配类别，按第 3 节的公式重新计算 `targetSize`（domain 和 line 均更新）和 `minSize`，`angle` 保持 10°。

更新后的 JSON 作为 `groupProperty` 参数传入。

### 4.3 调用生成工具

调用 `GenerateSurMeshBySpitAssemblyGroupProperty` 并按以下规则传参：

| 参数 | 值 |
|---|---|
| `ids` | `"0"`（所有超面）或指定面 ID 字符串 |
| `targetSize` | 全局目标尺寸，取默认值或所有组中最大值 |
| `minSize` | 全局最小尺寸，取默认值或所有组中最小值 |
| `adaptAngle` | `10` |
| `way` | `0`（组合法） |
| `groupProperty` | 更新后的分组属性 JSON 字符串 |

### 4.4 手动模式参数确认

手动模式使用基础 `tool_params` 协议展示主要参数后等待用户确认：

```json
{
  "tool_params": {
    "tool": "GenerateSurMeshBySpitAssemblyGroupProperty",
    "params": [
      {"name": "MAC", "description": "平均气动弦长", "value": "134.75"},
      {"name": "way", "description": "生成方案：0=组合法 1=狭长面 2=四边形占优", "value": "0"},
      {"name": "groups", "description": "各部件组参数", "value": [
        {"name": "jitou", "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "jishen", "targetSize": 5.39, "minSize": 0.54, "angle": 10},
        {"name": "weibu",  "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "jiyi_wing_upper_surface", "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "jiyi_wing_lower_surface", "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "jiyi_wing_tip",          "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "jiyi_trailing_edge",     "targetSize": 2.70, "minSize": 0.27, "angle": 10}
      ]}
    ]
  },
  "options": [
    {"label": "✅ 确认执行", "value": "confirm", "style": "primary"},
    {"label": "取消", "value": "cancel", "style": "danger"}
  ]
}
```

自动模式直接按计算值调用，不展示参数确认。

## 5. 与流程的关系

- 本技能是 `cad-to-mesh.md` 第 3 节"分部件处理路径"中第 5-6 步的具体实现，也是 `part-segmentation.md` 第 5 节"后续操作"中"按分组生成表面网格"的执行入口。
- 前置必须已完成分部件处理（分组已建立），且 MAC 值可用。
- 本技能不涉及体网格和空间网格生成，那些步骤在表面网格生成后由 `cad-to-mesh.md` 第 5 节继续处理。
