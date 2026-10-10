# 类型一：翼梢相邻后缘面处理

类型一后缘面与翼梢面相邻，由 4 条网格线（2 条长边 L1/L2、2 条短边 S1/S2）组成。

> ⚠️ **注意事项**
>
> 本步骤为机翼一体化处理（`WingAnisoProcessWingAnisotropy`）内部的失败回退流程。
> 正常情况下不需要执行本步骤，一体化入口已自动处理后缘面。
> 以下步骤仅在 `WingAnisoProcessWingAnisotropy` 返回的 `steps` 中 `step8b_te_delete_assemble` 失败时，
> 或后缘面处理条目 `status=failed` 时使用。

**间距参数**（动态计算，调用 `GetModelParameters` 获取 `wing_half_span` 和 `mac`）：
- `bodySpacing` = 0.1% × 半展长 = 0.001 × wing_half_span（靠近翼梢端）
- `rootSpacing` = 2% × 当地弦长 = 0.02 × mac（靠近翼根端）
- `params` = `"1.2,10,1.2,10"`

---

## 批量处理入口

```python
# 先通过 GetModelParameters 获取 wing_half_span 和 mac（当地弦长）
# bodySpacing = 0.001 × wing_half_span
# rootSpacing = 0.02 × mac（或 localChord）

# 类型判定已得到 domainId 和 wingTipId
ProcessTrailingEdgeType1(
    domainId=te_domain_id,          # 来自 ClassifyTrailingEdgeDomains 的 domain_id
    wingTipId=wing_tip_id,          # 来自 ClassifyTrailingEdgeDomains 的 wing_tip_id
    halfSpan=wing_half_span,        # 通过 GetModelParameters 获取
    localChord=mac,                 # 通过 GetModelParameters 获取
    bodySpacing=0.001 * wing_half_span,
    rootSpacing=0.02 * mac,
    params="1.2,10,1.2,10"          # 分布参数（可调整）
)
```

返回 `{"status":"success","message":"type1 done"}` 表示成功；
返回 `{"status":"failed","message":"..."}` 时回退到下方分步流程。

---

## 分步流程

> 仅在批量处理入口失败时执行以下步骤。

### 步骤 1：合并边

调用 `MergeEdgesByDomain`（当前后缘面 ID），得到 `longids`（L1、L2）和 `shortids`（S1、S2）。
- 若返回为空或缺少 `longids`/`shortids` → 失败停止。

### 步骤 2：验证交线位置

后缘面与翼梢面的公共线（交线）必须在短边中。
- 调用 `GetConnectorsByDomain`（`id` = 后缘面ID）和 `GetConnectorsByDomain`（`id` = 翼梢面ID），取两个返回的 `ids` 数组的交集。
- 交线必须等于 S1 或 S2，否则 → 失败停止。

### 步骤 3：短边设点数 + 平均分布

S1、S2 各一遍，点数固定为 5：
- 先调用 `GetPointCount`（当前短边 ID）获取当前点数。
- 若点数 != 5：
  - `SetConnectorPointCount`（当前短边 ID, 5）
  - `SetConnectorAverageDistribution`（当前短边 ID）
- 若点数 == 5：跳过操作。
- **S2 同理**。

### 步骤 4：判定 L1、L2 的方向

调用 `DetermineDirectionForType1`（后缘面 ID、翼梢面 ID、L1 ID、L2 ID），得到 `l1_tip_end` 和 `l2_tip_end`。
- `l1_tip_end` = `"start"` → L1 的 start 端靠近翼梢（headspace=`bodySpacing`, tailspace=`rootSpacing`）
- `l1_tip_end` = `"end"` → L1 的 end 端靠近翼梢（headspace=`rootSpacing`, tailspace=`bodySpacing`）
- L2 同理。

### 步骤 5：长边 L1 平滑分布

`SetConnectorSmoothDistribution`（L1, headspace, tailspace, params, rootSpacing）

### 步骤 6：长边 L2 设点数 + 平滑分布

- `CopyConnectorPointCount`（L1, L2）
- `SetConnectorSmoothDistribution`（L2, headspace, tailspace, params, rootSpacing）

### 步骤 7：删除原面

`DeleteDomain`（当前后缘面 ID, isDeleteConnector=0）

### 步骤 8：装配

`AssembleConnectorsToDomain`（L1, L2, S1, S2，逗号分隔，使用原始 ID，因操作后 ID 不变）
