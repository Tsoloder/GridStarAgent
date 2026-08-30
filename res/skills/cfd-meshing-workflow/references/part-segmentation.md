# 部件分割（AI 自动识别 + 几何法机翼小部件）

用户提及"部件分割"、"分割部件"、"自动分割"、"识别部件"时进入此流程。该流程通过 AI 自动识别 + 几何法将数模面按部件语义分组，并对机翼类部件进一步拆分为子组，为后续按分组生成表面网格或导出分割结果提供基础。

部件分割是独立于手动分部件处理路径（见 `cad-to-mesh.md` 第 3 节）的自动化方案：手动路径由用户逐个创建和分配分组，本流程由 AI 一次性完成全部分割。

## 1. 前置检查

1. 确认数模已导入。如未导入，先引导用户完成 CAD 导入（见 `cad-to-mesh.md` 第 1 节）。
2. 数模导入是部件分割的硬性前置条件；未导入时不执行分割。

## 2. 参数收集与确认

1. 询问点云输出目录（`outputDir`）。若用户不提供，使用默认临时目录。
2. 手动模式使用基础 `tool_params` 协议展示参数并等待用户确认（以下示例仅适用于 manual 模式）：

```json
{
  "tool_params": {
    "tool": "SegmentPart",
    "params": [
      {"name": "outputDir", "description": "点云输出目录", "value": ""}
    ]
  },
  "options": [
    {"label": "✅ 确认执行", "value": "confirm", "style": "primary"},
    {"label": "取消", "value": "cancel", "style": "danger"}
  ]
}
```

3. 自动模式使用已知值或默认临时目录直接执行。
4. 用户确认后才调用 `SegmentPart` 工具；未确认时不执行。

## 3. 执行分割

1. 调用 `SegmentPart` 工具，工具名称和参数以实时 MCP Schema 为准。
2. 等待工具返回；只有明确成功后才进入结果展示阶段。
3. 分割失败或超时时，按基础协议错误处理规则处理，不继续后续步骤。

## 4. 结果展示

1. 工具返回分割结果 JSON，展示给用户。
2. 结果格式：

```
[{"group_name": "wing", "faces": [4, 31, 55]}, ...]
```

3. 每个分组包含组名和所包含的面 ID 列表。
4. 其中机翼组（`wing` / `机翼`）会被进一步拆分为 4 个子组：
   - 机翼_上表面
   - 机翼_下表面
   - 翼稍
   - 后缘

5. 分割完成后各部件自动染色（不同颜色区分），无需额外调用染色工具。

## 5. 后续操作

分割完成后，manual 模式使用 `options` 询问用户是否需要基于分割结果做进一步操作；auto 模式根据用户原始目标自动判断下一步：

```json
{
  "options": [
    {"label": "1️⃣ 按分组生成表面网格", "value": "mesh_by_group", "style": "primary"},
    {"label": "2️⃣ 导出分割结果", "value": "export_result", "style": "default"},
    {"label": "3️⃣ 查看分组详情", "value": "view_details", "style": "default"}
  ]
}
```

### 按分组生成表面网格

用户选择此选项后，按 `references/part-based-surface-mesh.md` 的流程执行：

1. 调用 `GetModelParameters` 获取 MAC。
2. 调用 `GetAllSpitAssemblyGroupProperty` 获取当前分组属性 JSON。
3. 根据各部件组的语义（机头/机身/机尾/机翼/其他）按 MAC 比例更新各组的目标尺寸和最小尺寸。
4. 调用 `GenerateSurMeshBySpitAssemblyGroupProperty` 生成表面网格。

详细参数计算规则和调用流程见 `references/part-based-surface-mesh.md`。

### 导出分割结果

用户选择此选项后，按基础协议的导出流程执行，导出格式和参数以实时导出工具 Schema 为准。

导出前可调用 `GetModelParameters` 获取自动计算的几何尺寸（机身长度、翼根弦长、翼尖弦长、机翼半展长、特征长度、MAC）一并输出或展示。

### 查看分组详情

用户选择此选项后，列出各分组的组名、面数量和面 ID 范围，不调用有副作用的工具。

## 与其他流程的关系

- **CAD 导入后**：在 `cad-to-mesh.md` 流程A第 4 步中，选项"自动部件分割"即指向本流程。用户从 CAD 导入路径进入部件分割时，直接从第 1 节前置检查开始。
- **手动分部件处理**：本流程的分割结果可作为手动分部件处理的输入，跳过手动创建分组步骤。
- **表面网格生成**：分割完成后选择"按分组生成表面网格"，复用 `cad-to-mesh.md` 的分部件网格生成路径。
