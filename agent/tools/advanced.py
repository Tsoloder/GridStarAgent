"""GridStar advanced 业务域工具。"""

from client import send_post_request


def ProcessWithServer(serverHost: str, serverPort: int, outputDir: str):
    """自动部件分割：对当前已导入的数模执行水密性处理 → 表面网格生成 → 点云导出 → 远程 AI 大部件分割(7类) → 几何法机翼小部件分割 → 各部件自动染色 → 清除网格。

    使用场景：数模文件已通过 ImportCADFile 导入到 GridStar 中。
    执行后返回 JSON 格式的部件分割结果（含机翼子部件）。

    Args:
        serverHost: 远程推理服务器 IP 地址。
        serverPort: 远程推理服务器端口号。
        outputDir: 点云文件和分割结果 JSON 的输出目录。

    Returns:
        返回 JSON 格式的7类部件分组结果,例如：
        [{"group_name":"nose","faces":[...]}, {"group_name":"fuselage","faces":[...]}, ...]
        其中 main_wing 组会被进一步拆分为：
            wingUpperSurface / wingLowerSurface / wingTip / wingTrailingEdge
        失败时返回 "false"。
    """
    return send_post_request("ProcessWithServer", {"serverHost":serverHost, "serverPort":serverPort, "outputDir":outputDir}, timeout=360)


def ClassifyTrailingEdgeDomains():
    """判定所有后缘面的类型。

    一次性获取 wingTrailingEdge / wingTip / engine / fuselage 分组,
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


def IdentifyType2Roles(teDomainId: int, engineDomainIds: str, fuselageDomainIds: str,
                       upperSurfaceDomainIds: str, lowerSurfaceDomainIds: str):
    """类型二角色识别：识别后缘面的 6 条网格线角色。

    通过后缘面与 engine/fuselage/上表面/下表面分组的网格线交集确定角色：
    - A（短边）：与 engine 公共
    - B（短边）：与 fuselage 公共
    - F（长边）：与上表面公共
    - D、E（长边）：与下表面公共
    - C（短边）：连接 E 和 F 的剩余线
    调用者不需要关心网格线间的几何关系细节。

    Args:
        teDomainId: 后缘网格面 ID.
        engineDomainIds: engine 分组的网格面 ID 列表,逗号分隔,如 "50,51".
        fuselageDomainIds: fuselage 分组的网格面 ID 列表,逗号分隔,如 "60,61".
        upperSurfaceDomainIds: 机翼上表面分组的网格面 ID 列表,逗号分隔.
        lowerSurfaceDomainIds: 机翼下表面分组的网格面 ID 列表,逗号分隔.

    Returns:
        JSON 字符串,格式:
        {
          "A": N, "B": N, "C": N, "D": N, "E": N, "F": N,   // 各角色线 ID
          "A_start": N, "A_end": N,   // A 的首尾点
          "B_start": N, "B_end": N,   // B 的首尾点
          "C_start": N, "C_end": N,   // C 的首尾点
          "D_start": N, "D_end": N,   // D 的首尾点
          "E_start": N, "E_end": N,   // E 的首尾点
          "F_start": N, "F_end": N,   // F 的首尾点
          "assembly_order": "A,E,C,F,B,D"  // 装配顺序
        }
        通过各线的首尾点 ID 可以清晰知道共点关系：相同 ID 的端点即为相连。
        例如 A_end == D_start 表示 A 的尾端和 D 的首端是同一个点。
        调用方可通过对比端点 ID 自行判断连接关系。
    """
    return send_post_request("IdentifyType2Roles", {
        "teDomainId": teDomainId,
        "engineDomainIds": engineDomainIds,
        "fuselageDomainIds": fuselageDomainIds,
        "upperSurfaceDomainIds": upperSurfaceDomainIds,
        "lowerSurfaceDomainIds": lowerSurfaceDomainIds
    })


def IdentifyLeadingEdge(
    upperSurfaceDomainIds: str,
    lowerSurfaceDomainIds: str,
    fuselageDomainIds: str,
    wingTipDomainIds: str):
    """识别机翼前缘线及其与机身、翼梢的共点关系。

    通过机翼上表面、下表面、机身和翼梢的网格面ID集合，
    自动找出前缘线（机翼上表面与下表面的共边），
    并判断前缘线与机身、翼梢的共点关系（首点还是尾点）。

    使用场景：部件分割完成后，已有机翼上/下表面、机身、翼梢的面ID集合。

    Args:
        upperSurfaceDomainIds: 机翼上表面的网格面ID列表，逗号分隔，如 "1,2,3"。
        lowerSurfaceDomainIds: 机翼下表面的网格面ID列表，逗号分隔，如 "4,5,6"。
        fuselageDomainIds: 机身的网格面ID列表，逗号分隔，如 "7,8"。
        wingTipDomainIds: 翼梢的网格面ID列表，逗号分隔，如 "9,10"。

    Returns:
        JSON字符串，格式：
        {
          "leading_edge_ids": [101, 102, 103],
          "fuselage_adjacent": {
            "connector_id": 101,
            "common_point": "start"
          },
          "wing_tip_adjacent": {
            "connector_id": 103,
            "common_point": "end"
          }
        }
        失败时返回 {"status": "error", "message": "错误描述"}。
    """
    return send_post_request("IdentifyLeadingEdge", {
        "upperSurfaceDomainIds": upperSurfaceDomainIds,
        "lowerSurfaceDomainIds": lowerSurfaceDomainIds,
        "fuselageDomainIds": fuselageDomainIds,
        "wingTipDomainIds": wingTipDomainIds
    })

