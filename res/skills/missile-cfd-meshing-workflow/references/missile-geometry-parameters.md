# 导弹几何参数

导弹专用几何参数体系。对应 t.py 模块 2。

## 工具

- **`GetMissileModelParameters()`** — 获取导弹全部关键几何参数
- 需先运行导弹版 AI 分割（`ProcessWithServerForMissile`）

## 返回参数

| 参数 | 符号 | 说明 | 用途 |
|------|------|------|------|
| bodyDiameter | D | 弹径：头部锥体与弹体交接处直径或弹尾直径 | 弹体/尾部/头部网格尺寸基准 |
| finRootChord | C_root | 翼/舵根部弦长（前缘到尾缘距离） | 翼/舵网格尺寸基准 |
| finTipChord | C_tip | 翼/舵梢部弦长 | 翼/舵梢部网格尺寸 |
| ballNoseRadius | R | 球头半径 | 球头网格尺寸 |
| rudderGapHeight | H | 舵底间隙高度（舵底到弹体距离） | 舵面网格参数 |
| characteristicLength | L | 特征长度（软件自行计算） | 空间网格参考长度 |
| bodyLength | — | 弹体长度 | 外场基准（弓形外场 1×弹体长度） |
| noseType | — | 头部类型："ball" 或 "sharp" | 区分球头/尖锐头部处理 |

## 参数依赖关系

```
bodyDiameter (D) ──┬── 头部网格参数（球头 min(0.01×D, 0.25×R)、尖角 0.005×D）
                   ├── 弹体网格参数（0.05×D）
                   ├── 尾部网格参数（0.05×D）
                   └── 弹体二面角加密（0.002×D）

finRootChord (C_root) ──┬── 翼/舵网格参数（0.02×当地弦长）
                       └── 翼/舵前缘/梢部（0.008×当地弦长）

ballNoseRadius (R) ──── 球头网格参数（min(0.01×D, 0.25×R）)

rudderGapHeight (H) ──── 舵底间隙区域处理

bodyLength ──────────── 弓形外场（1×弹体长度）
```

## 参数获取与验证

调用 `GetMissileModelParameters()` 后，检查返回值完整性：

```plaintext
1. 若 bodyDiameter (D) == 0.0 或为 null → 立即停止后续所有步骤，告知用户缺失参数
2. 若 finRootChord (C_root) == 0.0 → 禁止使用弦长相关公式（0.008×C、0.02×C）
3. 若 noseType 为 null 或空字符串 → 禁止分支计算头部网格参数

当任一核心参数（D/C_root/noseType）无效时，Agent 必须：
- 不继续执行依赖该参数的网格生成操作
- 明确告知用户缺少的参数并请求提供
- 严禁用 0.0 代入公式计算
```

## 前置条件

1. 导弹 5 类 AI 分割已完成
2. 上述条件不满足时停止

## 当地弦长使用规则

所有 Skill 和 reference 中出现的"当地弦长"遵循以下规则：

| 出现位置 | 使用 C_root（根部弦长） | 使用 C_tip（梢部弦长） |
|---------|----------------------|----------------------|
| 弦向分布（前缘端 0.008×） | 靠近根部段使用 | 靠近梢部段使用 |
| 弦向分布（后缘端 0.008×） | 靠近根部段使用 | 靠近梢部段使用 |
| 弦向分布（中间值 0.02×） | 根部段用 C_root | 梢部段用 C_tip |
| 展向分布（梢部端 edgeSpacing） | — | 用 C_tip |
| 展向分布（根部端 edgeSpacing） | 用 C_root | — |
| 各向异性 mindValue（0.02×） | 根部段用 C_root | 梢部段用 C_tip |
| 后缘面 bodySpacing（靠近梢部端） | — | 用 C_tip |
| 后缘面 rootSpacing（靠近根部端） | 用 C_root | — |
| 中间区域 | 线性插值：C_local = C_root + (C_tip - C_root) × (x/L_span) |

- C_root = finRootChord，C_tip = finTipChord
- 舵(rudder)的弦长字段名与翼(fin)相同，均取自 `GetMissileModelParameters` 返回值的 `finRootChord` 和 `finTipChord`