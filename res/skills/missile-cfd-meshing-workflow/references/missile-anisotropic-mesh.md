# 导弹各向异性网格参考

导弹专用各向异性网格处理。对应 需求文件 模块 4。

## 适用位置

| 位置 | 说明 |
|------|------|
| 翼/舵前缘 | 前缘弧形面加密 |
| 翼/舵后缘 | 后缘狭长面加密 |
| 翼/舵梢部 | 梢部端面加密 |
| 翼/舵侧面 | 上下表面加密 |
| 翼/舵结合处 | 翼/舵与弹体连接处加密 |

## 工具

各向异性**不使用一站式工具**，按下表 4 个子 Skill **手动分步执行**（定位网格线 → 读原分布 → 设间距/分布）。

- 分布设置工具：`UGReDimensionConfigDistribution` / `UGReDimensionSmoothDistribution` / `SetConnectorPointCount` / `SetConnectorSmoothDistribution` / `SetConnectorAverageDistribution` / `CopyConnectorPointCount`（均 `[复用]`）
- ⚠️ `GenerateANisoDomainGrid` 在当前工具层**不存在**（历史规格遗留），**勿调用**。

## 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 首层高度 | 根据各位置表面网格尺寸确定 | 翼前缘=0.008×当地弦长 |
| 增长率 | 1.2 | 各子 Skill 共用 |
| 层数 | 按增长率增长到全局尺寸停止，最大不超过 50 层 | — |
| 分布类型 | 0（双曲正切） | 各子 Skill 共用 |

## 核心原则

- 各向异性只控制内部点分布，不更改边界分布
- 翼梢处网格线首层与前缘一致

## 位置判断与路由

根据用户输入或模型特征，判断需要各向异性处理的位置列表。每个位置对应一个独立子 Skill。

| 位置 | 子 Skill 名称 | 触发关键词 |
|------|--------------|-----------|
| 翼/舵弦向（前缘↔后缘） | `missile-chordwise-direction` | 弦向、弦长方向、前缘后缘、chordwise |
| 翼/舵展向（根部↔梢部） | `missile-spanwise-direction` | 展向、根部、梢部、半展长、spanwise |
| 翼/舵结合处 | `missile-root-junction` | 翼身结合、舵体结合、junction |
| 翼/舵后缘（狭长面） | `missile-trailing-edge-processing` | 后缘面、trailing edge（已有独立 Skill） |

> 4 个子 Skill 同时覆盖弹翼(wing)和舵(fin)，通过分组名前缀 `wing` / `fin` 区分。执行时翼先、舵后。

## 执行流程

### 步骤一：读取公共参数

```plaintext
各子 Skill 共用的参数默认值：
- 增长率（headRate / tailRate）：1.2
- 层数（headLayer / tailLayer）：最大 50 层
- 分布类型（disFunc）：0（双曲正切）
- 核心原则：各向异性只控制内部点分布，不更改边界分布
```

### 步骤二：按位置逐项处理

对判定需要处理的每个位置：

1. 用 `read_skill_resource("对应子 Skill 名", "SKILL.md")` 读取完整流程。
2. 按子 Skill 中的步骤执行工具调用。
3. 记录每个位置的处理结果（成功/失败）。

> **优先级规则**：先处理**弦向**（前缘后缘加密），再处理**展向/结合处**（翼根翼梢加密），最后处理**后缘**（狭长面处理）。

### 步骤三：全局一致性检查

所有位置处理完成后，检查以下内容：

1. **边界一致性**：各子 Skill 修改的内部点分布不能影响边界分布。
2. **相邻区域过渡**：相邻位置的网格线分布是否平滑过渡。若不满足，使用 `UGReDimensionSmoothDistribution` 做平滑处理。
3. **对边匹配**：涉及狭长面的区域，需保证对边的点数及分布相同。

## 前置条件

1. 表面网格已生成
2. 导弹几何尺寸已获取（`GetMissileDimensions`：当地弦长、弹径等）
3. 若用户对网格数量没有要求，可不做各向异性处理
4. **链路接口可用**：4 个子流程（弦向/展向/结合处/后缘）都以 `GetConnectorsByDomains`（取"面下辖网格线"）来定位目标线。该接口**若返回兜底值**（恒定 `"true"`/空，说明当前工程无网格或接口不可用），则**整个各向异性阶段 `skipped`**（按主 Skill「工具可用性判定」通用规则处理，**勿重试、勿换参**），在 `note` 记录后直接进入后缘面处理。
5. 🔴 **子面分组闸门（先查询、再决定做不做，禁止强行推理）**：调 `GetAllSpitAssemblyGroupProperty` 查是否存在各位置所需的**子面分组**（`wingLeadingEdge` / `wingTrailingEdge` / `wingSideSurface` / `finLeadingEdge` / `finTrailingEdge` / `finTop` / `finSideSurface` / `finRoot`）：
   - **子面分组存在** → 按本文件流程正常执行（取线用该分组 ID）；
   - **只有主组（`wing` / `fin`）而无子面分组**（用户手动分组 / 归并后无台账）→ **本阶段直接 `skipped` + `note`**，进入后缘面处理；🔴 **禁止**：为定位子面做几何反推（拉大量线的端点坐标逐面判断）、禁止用试探性调用（对候选面逐个调工具看返回）猜子面、禁止在 `note` 之外额外消耗轮次。几何反推仅在有台账线索、能一次查准时作辅助，不是默认路径。
