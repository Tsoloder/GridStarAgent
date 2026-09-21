# 导弹表面网格参数

导弹专用表面网格参数体系。对应 t.py 模块 3。

## 全局设置

- **工具**：`UGSur(type, ids, targetSize, minSize, adaptAngle, way)` `[复用]` 或 `GetGenerateSurMeshDefaultParam()` `[复用]` 获取默认值
- 全局使用默认尺寸，具体以用户输入为准

## 部件网格参数表

Agent 侧根据几何参数计算后传入。

| 部件 | 目标尺寸 | 最小尺寸 | 自适应角 | 公式来源 |
|------|---------|---------|---------|---------|
| 头部（球头） | min(0.01×D, 0.25×R) | 目标×1/10 | 10° | t.py 模块 3.2 |
| 头部（尖角） | 0.005×D | 目标×1/10 | 10° | t.py 模块 3.2 |
| 尾部 | 0.05×D | 目标×1/10 | 10° | t.py 模块 3.2 |
| 弹体 | 0.05×D | 目标×1/10 | 10° | t.py 模块 3.2 |
| 翼/舵 | ≤0.03×当地弦长，默认 0.02× | 目标×1/10 | 10° | t.py 模块 3.2 |
| 翼/舵前缘、梢部 | ≤0.008×当地弦长，点数≥7 | 目标×1/10 | 10° | t.py 模块 3.2 |
| 舵根部面 | ≤0.008×当地弦长，点数≥7 | 目标×1/10 | 10° | t.py 模块 3.2 |
| 外场 | 按场景计算 | 目标×1/10 | 10° | t.py 模块 3.2 |

## 符号说明

| 符号 | 参数名 | 来源 |
|------|--------|------|
| D | bodyDiameter | `GetMissileModelParameters` 返回 |
| R | ballNoseRadius | `GetMissileModelParameters` 返回 |
| noseType | noseType | `GetMissileModelParameters` 返回，值为 `"ball"` 或 `"sharp"` |
| 当地弦长 | finRootChord（根部）/ finTipChord（梢部） | `GetMissileModelParameters` 返回 |

## 执行步骤：根据 noseType 计算头部网格参数

**必须先调 `GetMissileModelParameters` 获取参数，然后根据 `noseType` 分支计算头部目标尺寸：**

```
params = GetMissileModelParameters()
D = params["bodyDiameter"]
noseType = params["noseType"]

if noseType == "ball":
    R = params["ballNoseRadius"]
    noseTargetSize = min(0.01 * D, 0.25 * R)
else:  # noseType == "sharp"
    noseTargetSize = 0.005 * D

noseMinSize = noseTargetSize * 0.1
```

**其他部件直接计算（不依赖 noseType）：**

| 部件 | 目标尺寸 | 最小尺寸 |
|------|---------|---------|
| 尾部 | 0.05 × D | 目标 × 0.1 |
| 弹体 | 0.05 × D | 目标 × 0.1 |
| 翼/舵 | 0.02 × 当地弦长（默认） | 目标 × 0.1 |
| 翼/舵前缘、梢部 | 0.008 × 当地弦长 | 目标 × 0.1 |
| 舵根部面 | 0.008 × 当地弦长 | 目标 × 0.1 |

**全局参数：** 自适应角统一为 10°。

**传入工具：** 各部件参数拼装后调用 `GenerateSurMeshBySpitAssemblyGroupProperty`：

| 参数 | 值 |
|------|-----|
| `ids` | `"0"`（所有超面）或指定面 ID 字符串 |
| `targetSize` | 全局目标尺寸，取各组中最大值 |
| `minSize` | 全局最小尺寸，取各组中最小值 |
| `adaptAngle` | `10` |
| `way` | `0`（组合法），可选：`0`=组合法 `1`=狭长面 `2`=四边形占优 |
| `groupProperty` | 更新后的分组属性 JSON 字符串 |

## 补充加密规则

### 弹体二面角夹角处加密（头部锥体与弹体交接部位）

**触发条件：** 弹体上存在明显的二面角夹角，一般发生在头部锥体与弹体的交接部位。

**加密尺寸：** 0.002×D。

**定位方法（找到 nose 与 body 的共享边）：**

```
# 1. 获取 nose 和 body 分组的网格面 ID
noseDomains = GetSpliteAssemlyDomainsBatch(["nose"])   # → {"groups":[{"groupName":"nose","domain_ids":[1, 2, ...]}]}
bodyDomains = GetSpliteAssemlyDomainsBatch(["body"])   # → {"groups":[{"groupName":"body","domain_ids":[3, 4, ...]}]}

# 2. 获取两组的网格线集合
noseConnectors = set()
for id in noseDomains["groups"][0]["domain_ids"]:
    result = GetConnectorsByDomain(id)           # → {"ids": [101, 102, ...]}
    noseConnectors.update(result["ids"])

bodyConnectors = set()
for id in bodyDomains["groups"][0]["domain_ids"]:
    result = GetConnectorsByDomain(id)
    bodyConnectors.update(result["ids"])

# 3. 取交集 → nose 与 body 的共享边（即头部锥体-弹体交接线）
junctionConnectors = noseConnectors & bodyConnectors

# 4. 对每条共享边设置分布（不修改两端间距，仅通过增长率控制内部点向 0.002×D 过渡）
# 各子 Skill 共用参数：增长率 1.2、层数最大 50、分布类型 0（双曲正切）
for cid in junctionConnectors:
    UGReDimensionConfigDistribution(
        id=cid,
        headSpace=原有值,    # 不修改
        tailSpace=原有值,    # 不修改
        headRate=1.2,
        headLayer=50,
        tailRate=1.2,
        tailLayer=50,
        midValue=0.002 * D,
        disFunc=0
    )
```

> **注意：** 此加密在各向异性阶段（`missile-anisotropic-mesh.md`）的翼身结合处处理中执行，不要求在表面网格生成阶段就设好。参见 `references/missile-anisotropic-mesh.md` 和独立 Skill `fin-body-junction`。

### 翼/舵前缘和梢部

- 翼/舵前缘和梢部：点数≥7（前缘/梢部弧线需足够分辨率）

## 工具

| 工具 | 用途 | 类型 |
|------|------|------|
| `UGSur` | 全局表面网格 | [复用] |
| `GetGenerateSurMeshDefaultParam` | 获取默认参数 | [复用] |
| `GenerateSurMeshBySpitAssemblyGroupProperty` | 按部件组生成表面网格 | [复用] |

## 前置条件

1. 水密性处理已完成（自由边检查通过）
2. 导弹 5 类 AI 分割已完成
3. 碎面合并已完成
4. 导弹几何参数已获取（`GetMissileModelParameters` 或用户直接提供）
5. 上述任一条件不满足时停止
