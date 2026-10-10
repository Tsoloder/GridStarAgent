# 按分部件属性生成表面网格

分部件处理完成后（AI 自动部件分割或手动分组），根据各部件组的语义按 MAC 比例生成表面网格。本技能与 `GenerateSurMeshBySpitAssemblyGroupProperty` 工具配合使用，为不同部件组设置差异化的网格尺寸。

## 1. 前置条件

- 部件分割已完成（AI 自动 `ProcessWithServer` 或手动分组），分组信息已写入分部件组列表。
- MAC（平均气动弦长）已获取。通过 `GetModelParameters` MCP 工具一次性获取（返回 JSON 中的 `mac` 字段），或按 `geometry-parameters.md` 的公式手动计算。
- 如需自定义分组属性，先调用 `GetAllSpitAssemblyGroupProperty` 获取当前分组属性 JSON。

## 2. 部件分组参数表

自动部件分割产生的分组名称与参数如下表。每个分组的目标尺寸和最小尺寸强制根据 MAC 计算，不能修改：

```
目标尺寸 = MAC × ratio
最小尺寸 = 目标尺寸 / 10
曲率自适应角度 = 10°
```

### 2.1 机头 / 机身 / 机尾

| 分组名称 | 中文名称 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| `nose` | 机头 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `fuselage` | 机身 | `MAC × 0.04` | `MAC × 0.004` | 10° |
| `tail` | 机尾 | `MAC × 0.02` | `MAC × 0.002` | 10° |

### 2.2 机翼

| 分组名称 | 中文名称 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| `wingUpper` | 机翼上表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `wingLower` | 机翼下表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `wingTip` | 翼梢 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `wingTrailingEdge` | 机翼后缘 | `MAC × 0.02` | `MAC × 0.002` | 10° |

### 2.3 吊舱

| 分组名称 | 中文名称 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| `enginePylonInner` | 吊舱支架内表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `enginePylonOuter` | 吊舱支架外表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `enginePylonTrailingEdge` | 吊舱支架后缘 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `enginePylonDefault` | 吊舱支架斜平面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `engineInner` | 吊舱内表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `engineOuter` | 吊舱外表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `engineTrailingEdge` | 吊舱后缘 | `MAC × 0.02` | `MAC × 0.002` | 10° |

### 2.4 立尾

| 分组名称 | 中文名称 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| `verticalTailRight` | 立尾右表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `verticalTailLeft` | 立尾左表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `verticalTailTip` | 立尾尾梢 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `verticalTailTrailingEdge` | 立尾后缘 | `MAC × 0.02` | `MAC × 0.002` | 10° |

### 2.5 平尾

| 分组名称 | 中文名称 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| `horizontalTailUpper` | 平尾上表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `horizontalTailLower` | 平尾下表面 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `horizontalTailTip` | 平尾尾梢 | `MAC × 0.02` | `MAC × 0.002` | 10° |
| `horizontalTailTrailingEdge` | 平尾后缘 | `MAC × 0.02` | `MAC × 0.002` | 10° |

### 2.6 其他

| 分组名称 | 中文名称 | 目标尺寸 | 最小尺寸 | 曲率自适应角度 |
|---|---|---|---|---|
| 其他（以上均不匹配的组） | 其他 | `MAC × 0.04` | `MAC × 0.004` | 10° |

## 3. 分组名称匹配规则

自动部件分割产生的分组命名可能混用中文、英文或拼音。按以下规则将实际组名匹配到第 2 节的分组：

| 目标分组 | 组名关键词匹配 |
|---|---|
| `nose` | 含 `nose`、`jitou`、`机头`、`forebody`、`前机身` |
| `fuselage` | 含 `fuselage`、`jishen`、`body`、`机身`、`中机身` |
| `tail` | 含 `tail`、`weibu`、`jiwei`、`机尾`、`aft`、`后机身` |
| `wingUpper` | 含 `wingUpper`、`upper`、`上翼面`、`机翼上表面` |
| `wingLower` | 含 `wingLower`、`lower`、`下翼面`、`机翼下表面` |
| `wingTip` | 含 `wingTip`、`tip`、`翼梢`、`翼尖` |
| `wingTrailingEdge` | 含 `wingTrailingEdge`、`机翼后缘`、`机翼尾缘` |
| `enginePylonInner` | 含 `enginePylonInner`、`pylonInner`、`支架内` |
| `enginePylonOuter` | 含 `enginePylonOuter`、`pylonOuter`、`支架外` |
| `enginePylonTrailingEdge` | 含 `enginePylonTrailingEdge`、`支架后缘`、`支架尾缘` |
| `enginePylonDefault` | 含 `enginePylonDefault`、`pylon`（不含 inner/outer/TE 关键词时）、`支架斜平`、`pylon斜` |
| `engineInner` | 含 `engineInner`、`吊舱内`、`短舱内` |
| `engineOuter` | 含 `engineOuter`、`吊舱外`、`短舱外` |
| `engineTrailingEdge` | 含 `engineTrailingEdge`、`吊舱后缘`、`短舱尾缘` |
| `verticalTailRight` | 含 `verticalTailRight`、`立尾右`、`垂尾右` |
| `verticalTailLeft` | 含 `verticalTailLeft`、`立尾左`、`垂尾左` |
| `verticalTailTip` | 含 `verticalTailTip`、`立尾梢`、`垂尾梢`、`立尾尖` |
| `verticalTailTrailingEdge` | 含 `verticalTailTrailingEdge`、`立尾后缘`、`垂尾后缘`、`垂尾尾缘` |
| `horizontalTailUpper` | 含 `horizontalTailUpper`、`平尾上` |
| `horizontalTailLower` | 含 `horizontalTailLower`、`平尾下` |
| `horizontalTailTip` | 含 `horizontalTailTip`、`平尾梢`、`平尾尖` |
| `horizontalTailTrailingEdge` | 含 `horizontalTailTrailingEdge`、`平尾后缘`、`平尾尾缘` |

**匹配优先级**：先匹配精确分组名（如 `enginePylonInner`），再匹配关键词。含 `pylon` 但不含 `inner`/`outer`/`trailing` 的归入 `enginePylonDefault`。以上均不匹配的组归入"其他"。

## 4. 执行流程

### 4.1 获取分组属性

调用 `GetAllSpitAssemblyGroupProperty` 获取当前所有分部件组的属性 JSON。返回格式示例：

```json
[
  {"wingUpper": {
    "line": [{"targetSize":12.34,"minSize":56.78,"angle":56.78,"ids":[1,2,3]}],
    "domain": [{"targetSize":6.7,"minSize":33.44,"angle":56.78,"ids":[1,2,3]}]}}
  },
  {"fuselage": {
    "line": [{"targetSize":12.34,"minSize":56.78,"angle":56.78,"ids":[1,2,3]}],
    "domain": [{"targetSize":6.7,"minSize":33.44,"angle":56.78,"ids":[1,2,3]}]}}
  }
]
```

### 4.2 更新各分组参数

对 JSON 中的每个分组对象，根据其组名（对象的 key）按第 3 节的规则匹配到目标分组，再按第 2 节的参数表重新计算 `targetSize`（domain 和 line 均更新）和 `minSize`，`angle` 保持 10°。

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
        {"name": "nose",                     "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "fuselage",                 "targetSize": 5.39, "minSize": 0.54, "angle": 10},
        {"name": "tail",                     "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "wingUpper",         "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "wingLower",         "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "wingTip",                  "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "wingTrailingEdge",         "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "enginePylonInner",         "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "enginePylonOuter",         "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "enginePylonTrailingEdge",  "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "enginePylonDefault",       "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "engineInner",              "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "engineOuter",              "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "engineTrailingEdge",       "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "verticalTailRight",        "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "verticalTailLeft",         "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "verticalTailTip",          "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "verticalTailTrailingEdge", "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "horizontalTailUpper",      "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "horizontalTailLower",      "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "horizontalTailTip",        "targetSize": 2.70, "minSize": 0.27, "angle": 10},
        {"name": "horizontalTailTrailingEdge","targetSize": 2.70, "minSize": 0.27, "angle": 10}
      ]}
    ]
  },
  "options": [
    {"label": "✅ 确认执行", "value": "confirm", "style": "primary"},
    {"label": "取消", "value": "cancel", "style": "danger"}
  ]
}
```

> 以上数值以 MAC=134.75 为例。机身和"其他"组的目标尺寸为 `MAC × 0.04`，其余组为 `MAC × 0.02`。

自动模式直接按计算值调用，不展示参数确认。

## 5. 与流程的关系

- 本技能是 `cad-to-mesh.md` 第 3 节"分部件处理路径"中第 5-6 步的具体实现，也是 `part-segmentation.md` 第 5 节"后续操作"中"按分组生成表面网格"的执行入口。
- 前置必须已完成分部件处理（分组已建立），且 MAC 值可用。
- 本技能不涉及体网格和空间网格生成，那些步骤在表面网格生成后由 `cad-to-mesh.md` 第 7 节继续处理。
