# 体创建失败排查手册（封闭性 / CAD 碎片）

> 本文件不是独立的网格阶段，而是**体创建（`UGBlockCreate`）失败时的排查手册**——当 `UGBlockCreate` 返回 `"false"` 且报「不封闭的网格线ID如下」时按此排查；其余失败原因回 `missile-farfield-and-volume.md` §6.3。

### §6.0 体创建失败 / 网格不封闭排查

**创建体网格块 / 外场要求网格封闭**。网格不封闭时后端会**直接拒绝** `UGBlockCreate` 并报错，此时**参数正确也会失败**。

#### 判据

**一条网格线若只被 1 个（或 0 个）网格面引用，即为"不封闭"。**

#### 🔴 检查方法（默认路径：建场即试金石，不做逐面全量统计）

> **为什么要"用建场当试金石"而不是预先统计**（2026-09-29 实机教训）：
> - 预先全量统计（`GetAllObjectByType(6)` + `GetConnectorsByDomains` 全列表 + 引用计数）在网格规模大时**逐面轮询非常慢**（实测 341 线规模就耗 5 分钟以上）；
> - `GetAllObjectByType` 返回的 ID 列表**可能含已删除/未回收的"幽灵 ID"**——查这些 ID 的坐标/下辖线会返回 not found，容易**误判成"不封闭线"**而卡住自己（实测把 8 条幽灵线当成了 8 条不封闭线）；
> - **后端闭包判定才是唯一权威**：`UGBlockCreate` 返回 `"true"` = 网格封闭、可建场；返回 `"false"` 时后端错误信息会**直接列出全部不封闭线 ID**，无需自统计。

**标准流程（2–3 轮调用，替代十几轮统计）**：

1. 直接调用 `UGBlockCreate`（按 `missile-farfield-and-volume.md` §6.2/§6.3 参数）；
2. 返回 `"true"` → **网格封闭**，继续：`GetDomainsByType(3)` 确认外场面已生成 → 进入 §7（质量检查）→ 后续流程；
3. 返回 `"false"` → 看后端错误信息：
   - 含「存在不封闭的网格线ID如下：…」→ 网格**不封闭**，按下方「排查顺序」处理，**不封闭线清单以后端报出的为准**；
   - 不含该字样 → **不是封闭性问题**，回 `missile-farfield-and-volume.md` §6.3「创建失败时的处理」逐项排查。

#### 兜底路径（手动统计，仅在上述路径需要"不封闭线清单"时才用）

> 🔴 **必须过滤幽灵 ID（2026-09-29 实机教训）**：`GetAllObjectByType(6)` 的返回列表里可能有已删除对象的残留 ID——拿它去 `GetConnectorsByDomains` / `GetStartAndEndPointByConnectors` 查询返回 **not found / 空**。**统计引用次数前先剔除这些查不到的 ID**，否则会把幽灵 ID 误判成"0 引用不封闭线"。

```python
from collections import defaultdict
domains = GetAllObjectByType(type=6)["info"]              # 全部网格面 ID（可能含幽灵 ID）
# 🔴 先过滤幽灵 ID：对每个 ID 调 GetConnectorsByDomains，空结果/not found → 剔除
valid = [d for d in domains if GetConnectorsByDomains(domain_ids=[d]).get("domains")]
ref = defaultdict(list)
for d in GetConnectorsByDomains(domain_ids=valid)["domains"]:
    for c in d["connector_ids"]:
        ref[c].append(d["domain_id"])
free_edges = [c for c, ds in ref.items() if len(ds) == 1]   # 不封闭的网格线
```

#### 判定与处理

| 结果 | 处理 |
|---|---|
| `UGBlockCreate` 返回 `"true"`（封闭） | ✅ 继续 §7 质量检查与后续流程 |
| 返回 `"false"` 且报「不封闭的网格线ID如下」 | ❌ **不能建场**，按下述顺序排查；仍无法消除时，**如实报告"外场/体网格/空间网格无法生成"**，并附**后端报出的不封闭线清单**（ID + 端点坐标，坐标用 `GetStartAndEndPointByConnectors` 取） |

**排查顺序**：

1. 🔴 **先做 CAD 拓扑体检**（见下方「CAD 拓扑体检」）—— **这是根因判据**。CAD 面拓扑异常时（**面被切碎**、或**存在大量不属于任何面的游离线**），"不封闭"与"网格偏密"是**同一个根因的两个表现**，靠调 `targetSize` / `minSize` **永远解决不了**。
2. **碎面合并是否已完成**（§3.1）—— 碎面未合并时，碎面边界处的网格线可能不连续 → 先完成碎面合并，再重新生成一遍表面网格
3. **网格是否一次干净生成** —— 若"不被任何面引用"的孤立线占网格线总数的比例明显偏高，说明网格可能是多次生成叠加的残留（数据不干净） → 清空网格后**重新生成一次**
4. 以上处理后仍不封闭 → 报告后端（表面网格生成的边界共享问题）

> 🔴 **执行时机**：体创建（`UGBlockCreate`）是封闭性判定环节——成功=封闭通过，失败且报不封闭线=进入本排查手册。不要求在表面网格生成后单独跑一遍全量统计。

#### CAD 拓扑体检（碎片化判据）

网格封闭性受 CAD 面拓扑支配：**面与面之间若不共享边，接缝在网格里就必然是开口边界。**

用只读查询做一次体检（全部为**通用**工具，导弹可用）：

```python
lines = len(GetAllObjectByType(1)["info"])   # 1 = 数模线（CAD 边）
faces = len(GetAllObjectByType(2)["info"])   # 2 = 数模面
ratio = lines / max(faces, 1)                # 边/面比
```

| 指标 | 取法（只读） | 正常 | 异常 |
|---|---|---|---|
| **边/面比** | `GetAllObjectByType(1)` 数模线 ÷ `GetAllObjectByType(2)` 数模面 | **≤3**（闭合面 E ≈ 1.5–2 F） | **>5** |
| **面数 vs 部件数** | `GetAllObjectByType(2)` ÷ 实际部件数 | 每部件 1–5 面 | 单组远超（如某子部件却分出 76 个面） |
| **分组面数分布** | `GetAllSpitAssemblyGroupProperty()` | 各组面数 ≈ 该部件实际片数 | 某组面数异常多 |

> ⚠️ **边/面比偏高有两种完全不同的成因，处置也不同 —— 必须区分，否则会修错地方**：
>
> | 成因 | 特征 | 处置 |
> |---|---|---|
> | **A. 面被切碎** | `GetAllObjectByType(2)` 本身就大（某子部件却 76 个面） | §3.1 的 `UGDamageRepari`（碎面修复）合并，必要时 `CreateCoons` 重建单面 |
> | **B. 游离线多** | 面数正常，但大量曲线**不属于任何面的边界** | `AutoExtractConnector` / `ManualExtractConnector` 重建共享边界；残留线本身是垃圾，可考虑 `DeleteFC` 清理（⚠️ 破坏性，先备份） |

> 💡 `.spd` 是 **OpenCASCADE BREP 文本格式**，可直接读出 `Surfaces` / `Curves` / `TShapes` 计数乃至逐条边的端点坐标 —— 定位"碎在哪"时优先读它，比反复重生成网格快得多。

**处置**：面碎片化的出路在 **CAD 层，不在网格参数层**：

1. 让 §3.1 的碎面合并真正成功（先核对 `tolerance` = **面积比**、`minLenth` = **最小边长**，按模型尺度取值）；
2. 回到源头数模重新导出 / 让建模方缝合后再导入；
3. 都做不到时 —— **如实报告**"表面网格不封闭、外场/体网格/空间网格无法生成"，附**不封闭线清单 + 拓扑体检数字**（面数 / 线数 / 边面比），**不得靠反复调网格尺寸硬试**。

> ⚠️ **ID 换算**：`GetConnectorsByDomains` 返回的网格线 ID 与 GridStar 界面显示的 ID **相差 `1`**（界面 ID = 本接口 ID + 1）。对照界面报错清单时需换算。
>
> ⚠️ **两个接口的返回格式容易踩**：
> - `GetStartAndEndPointByConnectors` 的 `coordinates` 是**字符串**（如 `"[4108,-15,536.076]"`），需自行切分再转 float，**不是数组**；
> - `GetConnectorsStartAndEndUnitLenth` 返回的 `start` / `end` 是**参数值、不是长度** —— **不要拿它算网格线长度**（会得到 0~1 量级的无意义数）；要长度就用 `GetStartAndEndPointByConnectors` 取两端点坐标求距离。

#### 布点间距现场测量（判断"分组尺寸是否真的生效"）

把 **线长**（`GetStartAndEndPointByConnectors`）与 **点数**（`GetPointCount`）合起来，可得**每条网格线的实际平均布点间距 = 线长 ÷ (点数 − 1)**，再按分部件分组汇总：

- **各组间距互不相同、且与各自目标尺寸同量级** → `groupProperty` 里的**分组尺寸确实生效**；
- **各组间距都趋同** → 分组尺寸没生效（回落某个全局值）→ **此时应查接口 / 传参，而不是继续调数值**。
> 注意：本法只能看到**网格线（面边界）上的间距**，看不到面内部的单元尺寸；对细长面（如翼面）**不能据此判定内部过密**。
