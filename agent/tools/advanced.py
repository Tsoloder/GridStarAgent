"""GridStar advanced 业务域工具。"""

from client import send_post_request


def SegmentPart(outputDir: str):
    """自动部件分割：对当前已导入的数模执行水密性处理 → 表面网格生成 → 点云导出 → 远程 AI 大部件分割 → 几何法机翼小部件分割 → 各部件自动染色 → 清除网格。

    使用场景：数模文件已通过 ImportCADFile 导入到 GridStar 中。
    执行后返回 JSON 格式的部件分割结果（含机翼子部件）。

    Args:
        outputDir: 点云文件和分割结果 JSON 的输出目录。

    Returns:
        返回 JSON 格式的部件分组结果,例如：
        [{"group_name":"wing","faces":[4,31,55]}, ...]
        其中 wing 组会被进一步拆分为：
            jiyi_wing_upper_surface / jiyi_wing_lower_surface / jiyi_wing_tip / jiyi_trailing_edge
        失败时返回 "false"。
    """
    return send_post_request("SegmentPart", {"outputDir":outputDir}, timeout=360)


def SegmentPartDirect(pointCloudPath: str):
    """直接远程部件分割：传入已有的点云文件路径,直接执行远程 AI 部件分割后清除网格。

    使用场景：已经有点云文件（f6.txt 格式）,不需要重新生成。
    执行后返回 JSON 格式的部件分割结果。

    Args:
        pointCloudPath: 点云文件完整路径（f6.txt 格式）。

    Returns:
        返回 JSON 格式的部件分组结果,例如：
        [{"group_name":"wing","faces":[4,31,55]}, {"group_name":"tail","faces":[0,1,3,5]}]
        失败时返回 "false"。
    """
    return send_post_request("SegmentPartDirect", {"pointCloudPath":pointCloudPath})


def ClassifyTrailingEdgeDomains():
    """判定所有后缘面的类型。

    一次性获取 jiyi_trailing_edge / jiyi_wing_tip / engine / fuselage 分组,
    通过网格线交集自动判断每个后缘面的类型（类型一:翼梢相邻, 类型二:吊舱-机身相邻）。

    使用场景：后缘面处理的第一步,获取每个后缘面的类型标签和关联的分组信息。

    Returns:
        JSON 字符串,格式:
        {
          "classifications": [
            {"domain_id":12, "type":1, "wing_tip_id":34},
            {"domain_id":14, "type":2}
          ],
          "te_domains":[12,13,14,15],
          "wing_tip_domains":[34,35],
          "engine_domains":[50,51],
          "fuselage_domains":[60,61]
        }
    """
    return send_post_request("ClassifyTrailingEdgeDomains", {})


def DetermineDirectionForType1(teDomainId: int, wtDomainId: int, l1Id: int, l2Id: int):
    """类型一方向判定：判断两条长边哪端靠近翼梢。

    此工具封装了后缘面与翼梢面的网格线拓扑匹配逻辑,
    调用者不需要关心网格线间的几何关系细节。

    Args:
        teDomainId: 后缘网格面 ID.
        wtDomainId: 翼梢网格面 ID.
        l1Id: 长边 1 的网格线 ID.
        l2Id: 长边 2 的网格线 ID.

    Returns:
        JSON 字符串,格式:
        {
          "intersection_connector_id": N,
          "l1_tip_end": "start" or "end",
          "l2_tip_end": "start" or "end"
        }
    """
    return send_post_request("DetermineDirectionForType1", {
        "teDomainId": teDomainId,
        "wtDomainId": wtDomainId,
        "l1Id": l1Id,
        "l2Id": l2Id
    })


def IdentifyType2Roles(teDomainId: int, engineDomainIds: str, fuselageDomainIds: str):
    """类型二角色识别：识别后缘面的 6 条网格线角色（A/B/C/D/短边/E）。

    通过后缘面与 engine/fuselage 分组的网格线交集确定角色 A（engine 公共线）
    和角色 B（fuselage 公共线）,再通过端点匹配确定 C/D/E/短边。
    调用者不需要关心网格线间的几何关系细节。

    Args:
        teDomainId: 后缘网格面 ID.
        engineDomainIds: engine 分组的网格面 ID 列表,逗号分隔,如 "50,51".
        fuselageDomainIds: fuselage 分组的网格面 ID 列表,逗号分隔,如 "60,61".

    Returns:
        JSON 字符串,格式:
        {
          "A": N, "B": N, "C": N, "D": N,
          "shortEdge": N, "E": N,
          "a_start_id": N, "a_end_id": N,
          "b_start_id": N, "b_end_id": N,
          "c_near_a_end": "start|end",
          "d_near_a_end": "start|end",
          "e_near_a_end": "start|end",
          "d_a_side": "start|end",
          "e_a_side": "start|end"
        }
        其中 c_near_a_end / d_near_a_end / e_near_a_end 分别表示角色 C/D/E 的哪一端靠近角色 A,
        d_a_side / e_a_side 表示 D/E 连接的是 A 的 start 端还是 end 端,
        调用方可直接使用此值来设置分布方向,无需自行比对端点。
    """
    return send_post_request("IdentifyType2Roles", {
        "teDomainId": teDomainId,
        "engineDomainIds": engineDomainIds,
        "fuselageDomainIds": fuselageDomainIds
    })

