"""导弹弹翼各向异性网格处理工具。

对生成好的导弹表面非结构网格的弹翼部分做各向异性处理，
在 SuperFacePointCloudProcessorMissile 完成分部件处理之后调用。

导弹弹翼分组命名规则（数字后缀表示第几对弹翼）：
  wingLeadingEdge1, wingTrailingEdge1, wingSideSurface1, wingTip1
  wingLeadingEdge2, wingTrailingEdge2, wingSideSurface2, wingTip2
  wingLeadingEdge3, wingTrailingEdge3, wingSideSurface3, wingTip3

处理流程（每对弹翼独立处理）：
  1. 获取弹体尺寸（弹径、弹长）
  2. 自动计算该对弹翼的弦长/展长/MAC
  3. 分类网格线（前缘、后缘长/短、翼身交线、翼梢交线）
  4. 分布设置
  5. 创建各向异性线组
  6. 应用各向异性域
  7. 狭长面处理（后缘面）
"""

from client import send_post_request


def MissileAnisoProcessMissileAnisotropy():
    """导弹弹翼各向异性处理主入口 — 依次执行全部子步骤并汇总结果。

    包含：尺寸数据获取 → 弹翼线分类 → 分布设置 →
          各向异性线组创建 → 各向异性域生成 → 狭长面处理。
    前置条件：
      - 导弹表面非结构网格已生成
      - 导弹子部件分割已完成
      （fuselage, nose, tail, wingLeadingEdgeN, wingTrailingEdgeN,
       wingSideSurfaceN, wingTipN 等分组已存在）

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","steps":{...}}
    """
    return send_post_request("MissileAnisoProcessMissileAnisotropy", {})
