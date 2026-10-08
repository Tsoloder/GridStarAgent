"""GridStar 导弹业务域工具。

导弹模块专用工具函数集合。
与飞机模块（advanced.py / query.py）完全分离，不修改飞机任何代码。
所有函数均为 thin wrapper，通过 send_post_request 调用 GridStar 服务端（127.0.0.1:1313）。

对齐需求文件 7 模块工作流。
"""

import json

from client import send_post_request


# ===== 导弹 advanced 类工具 =====


def ClassifyMissile(serverHost: str, serverPort: int, outputDir: str):
    """导弹 AI 部件分割（后端接口名 ClassifyMissile）：对当前已导入的导弹数模执行水密性处理 → 表面网格生成 → 点云导出 → 远程 AI 分割 → 几何法子部件分割 → 各部件自动染色 → 清除网格。

    分割类别（主体 3 类 + 弹翼子面）：
    - nose(弹头)：球头 / 尖锐头部
    - fuselage(弹体)
    - tail(尾部)：尾部收段
    - 弹翼(wing)无主组，按 需求文件 1.4 拆分为（3 个基本子面 + 1 条件性子面）：
      wingLeadingEdge / wingTrailingEdge / wingSideSurface（+ wingTip 仅梯形翼存在，三角翼无）
    - 🔴 当前型号无舵、无舵轴（2026-10-08 确认）：不产出 fin* 子面、finshaft 分组

    使用场景：导弹数模文件已通过 ImportCADFile 导入到 GridStar 中。
    执行后返回 JSON 格式的部件分割结果（含翼面子部件）。

    Args:
        serverHost: 远程推理服务器 IP 地址（默认 "7.31.130.92"）。
        serverPort: 远程推理服务器端口号（默认 9009）。
        outputDir: 点云文件和分割结果 JSON 的输出目录（默认 "D:/A_GridStarCode/output"）。

    Returns:
        成功时返回 status="success"，result 为以下格式的列表（共 7 个分组：nose/fuselage/tail 3 主体类 + 3 个弹翼基本子面 + 1 个条件性子面；wingTip 仅梯形翼时才额外返回）：

        [  # ===== 主体类 =====
          {"group_name": "nose",      "faces": [...]},   # 弹头（球头或尖头）
          {"group_name": "fuselage",  "faces": [...]},   # 弹体
          {"group_name": "tail",      "faces": [...]},   # 尾部
          # ===== 弹翼子面（无 wing 主组；需求文件 1.4：前缘面、后缘面、翼侧面；翼梢面仅梯形翼）=====
          {"group_name": "wingLeadingEdge",  "faces": [...]},  # 翼前缘面
          {"group_name": "wingTrailingEdge", "faces": [...]},  # 翼后缘面
          {"group_name": "wingSideSurface",  "faces": [...]},  # 翼侧面
          {"group_name": "wingTip",          "faces": [...]},  # 翼梢面（仅梯形翼存在，三角翼无）
          # ===== 注：当前型号无舵(fin*)、无舵轴(finshaft) =====
        ]

        faces 为整数 face_id 列表，对应数模原始面。该结果直接用于后续 UGSpitAssembly 分组操作。
        失败时返回 status="error"。
    """
    return send_post_request("ClassifyMissile", {
        "serverHost": serverHost,
        "serverPort": serverPort,
        "outputDir": outputDir
    }, timeout=360)


def DetermineWingTEDirection(wingTeDomainId: int, wingTipDomainId: int, longIds: str):
    """导弹翼后缘方向判定：判断两条长边哪端靠近翼梢。

    封装了后缘面与翼梢面的网格线拓扑匹配逻辑，
    调用者不需要关心网格线间的几何关系细节。

    Args:
        wingTeDomainId: 弹翼后缘网格面 ID（来自 GetAllSpitAssemblyGroupProperty 解析的 ["wingTrailingEdge"].domain[].ids）。
        wingTipDomainId: 弹翼梢部网格面 ID（来自 GetAllSpitAssemblyGroupProperty 解析的 ["wingTip"].domain[].ids）。
        longIds: MergeEdgesByDomain 返回的 longids，直接传入，如 "[101, 102]"。
            调用链：先用 GetAllSpitAssemblyGroupProperty 拿后缘面 domain，
            再调 MergeEdgesByDomain(domainId) → 取返回的 longids 字段，直接传入本参数。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "intersection_connector_id": 105,   # 与翼梢面共点的网格线 ID
          "longEdge_tip_end": ["end", "start"] # 两条长边各自靠近翼梢的一端（"start"或"end"）
        }
        失败时返回 status="error"。
    """
    return send_post_request("DetermineWingTEDirection", {
        "wingTeDomainId": wingTeDomainId,
        "wingTipDomainId": wingTipDomainId,
        "longIds": longIds
    })


def IdentifyWingLeadingEdge(
    wingSideIds: str,
    wingLeadingEdgeIds: str,
    fuselageIds: str,
    wingTipIds: str):
    """识别导弹翼前缘线及其与弹体、翼梢的共点关系。

    通过翼侧面、前缘面、弹体和翼梢的网格面 ID 集合，
    自动找出前缘线（翼侧面与前缘面的公共边），
    并判断前缘线与弹体、翼梢的共点关系（首点还是尾点）。

    使用场景：表面网格生成后，已有翼侧面、前缘面、弹体、翼梢的 domain ID 集合。
    调用前需先通过 GetAllSpitAssemblyGroupProperty 解析各分组的 domain ID 列表
    （该接口返回 list[{组名:{line:[...], domain:[{ids:[...]}]}}]，取 domain[].ids 拼接）。

    Args:
        wingSideIds: 弹翼侧面的网格面 ID 列表，逗号分隔，如 "1,2,3"。
            来源：GetAllSpitAssemblyGroupProperty 中 ["wingSideSurface"].domain[].ids，用逗号拼接。
        wingLeadingEdgeIds: 弹翼前缘面的网格面 ID 列表，逗号分隔，如 "4,5"。
            来源：GetAllSpitAssemblyGroupProperty 中 ["wingLeadingEdge"].domain[].ids。
        fuselageIds: 弹体的网格面 ID 列表，逗号分隔，如 "7,8"。
            来源：GetAllSpitAssemblyGroupProperty 中 ["fuselage"].domain[].ids。
        wingTipIds: 弹翼梢部的网格面 ID 列表，逗号分隔，如 "9,10"。
            来源：GetAllSpitAssemblyGroupProperty 中 ["wingTip"].domain[].ids。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "leading_edge_ids": [101, 102, 103],   # 前缘线 ID 列表（翼侧面与前缘面的公共边）
          "body_adjacent": {                       # 前缘线与弹体的共点关系
            "connector_id": 101,                  # 与弹体共点的前缘线 ID
            "common_point": "start"               # 共点位于前缘线的哪一端（"start"或"end"）
          },
          "fin_tip_adjacent": {                    # 前缘线与翼梢的共点关系
            "connector_id": 103,                  # 与翼梢共点的前缘线 ID
            "common_point": "end"
          }
        }
        失败时返回 status="error"。
    """
    return send_post_request("IdentifyWingLeadingEdge", {
        "wingSideIds": wingSideIds,
        "wingLeadingEdgeIds": wingLeadingEdgeIds,
        "fuselageIds": fuselageIds,
        "wingTipIds": wingTipIds
    })


def CreateMissileFarField(fuselageLength: float, noseRadius: float):
    """创建导弹弓形外场。

    弓形外场规则（对齐 需求文件 模块 6）：
    - 外场尺寸：1 倍全弹长
    - 头部距离：1.5 倍球头半径

    场景三（无外场 + 全模）和场景四（无外场 + 半模）时调用。
    场景一/二已有外场时不需要调用。

    Args:
        fuselageLength: 全弹长（外场基准尺寸），来自 GetMissileDimensions 的 fuselage_length 字段。
        noseRadius: 球头半径 R（用于计算头部距离），
            来自 GetMissileDimensions 的 nose_radius 字段。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "farFieldId": 50,                    # 创建的外场 domain ID
          "farFieldName": "MissileFarField",   # 外场名称
          "farFieldSize": 3000.0,              # 实际外场尺寸（= fuselageLength）
          "headDistance": 45.0                 # 头部距离（= 1.5 × noseRadius）
        }
        失败时返回 status="error"。
    """
    return send_post_request("CreateMissileFarField", {
        "fuselageLength": fuselageLength,
        "noseRadius": noseRadius
    })


def CheckMissileMeshQuality(domainIds: str = "", blockIds: str = ""):
    """导弹网格质量综合检查。

    面网格质量标准（对齐 需求文件 模块 7.1）：
    - 除去各向异性单元，最小角 > 10°
    - 若 < 10°：查看位置，模型特征导致的可不处理

    体网格质量标准（对齐 需求文件 模块 7.2）：
    - 最大角 ≤ 178°
    - > 178° 需输出数量
    - 禁止 179.9° 单元

    Args:
        domainIds: 需检查的网格面 ID 列表，逗号分隔。为空则检查全部。
        blockIds: 需检查的网格块 ID 列表，逗号分隔。为空则检查全部。

    Returns:
        JSON 字符串,格式:
        {
          "surfaceMinAngle": float,
          "volumeMaxAngle": float,
          "badCellCount": int,
          "passed": bool
        }
    """
    return send_post_request("CheckMissileMeshQuality", {
        "domainIds": domainIds,
        "blockIds": blockIds
    })


# ===== 导弹 query 类工具 =====


def GetMissileDimensions():
    """获取导弹全部关键几何尺寸参数（后端接口名 GetMissileDimensions）。

    需先运行 AI 分割（ClassifyMissile）方能获取有效值。

    返回参数（7 项，后端 snake_case 字段；🔴 2026-10-08 起当前型号无舵，已移除 fin_* 3 项）：
    - fuselage_length: 全弹长（外场基准，弓形外场 = 1× 全弹长）
    - body_diameter: 弹径（主体口径，弹体/尾部/头部网格尺寸基准 D）
    - body_diameter_tail: 弹尾口径
    - nose_radius: 球头半径（球头网格尺寸基准 R；尖头时为 0）
    - wing_root_chord: 翼根弦长（弹翼网格尺寸基准）
    - wing_tip_chord: 翼梢弦长（弹翼梢部网格尺寸）
    - wing_half_span: 半翼展（展向分布用）

    > 命名提示：本模块函数名与参数名统一使用现行命名 —— `wing` = 弹翼、`fuselage` = 弹体；当前型号无舵(`fin`)、无舵轴(`finshaft`)，相关分组/字段不再出现。

    Returns:
        成功时返回 status="success"，result 为 JSON 字符串：
        {
          "fuselage_length": 12500.0,
          "body_diameter": 800.0,
          "body_diameter_tail": 700.0,
          "nose_radius": 50.0,
          "wing_root_chord": 1200.0,
          "wing_tip_chord": 800.0,
          "wing_half_span": 2500.0
        }
        失败时返回 status="error"。
    """
    raw = send_post_request("GetMissileDimensions", {})
    if isinstance(raw, dict) and raw.get("status") == "success":
        return raw.get("result", raw)
    return raw


def GetMissilePartGroups():
    """获取当前模型中已有的导弹部件分组信息（后端接口名 GetMissilePartGroups）。

    需先运行 AI 分割（ClassifyMissile）完成分类。

    后端遍历模型的 AssemblyGroup，过滤出导弹部件分组，统计每组包含的超面（面片）ID。

    Returns:
        成功时返回 status="success"，result 为 JSON 数组：
        [
          {"group_name": "nose",  "face_count": 3, "face_ids": [0, 5, 12]},
          {"group_name": "wing",  "face_count": 6, "face_ids": [...]},
          ...
        ]
        - group_name: 组名（后端 4 类：nose / fuselage / tail / wing；🔴 2026-10-08 起当前型号无舵( fin )无舵轴( finshaft)，已移除）
        - face_count: 该组包含的超面数量
        - face_ids: 该组包含的超面 ID 列表
        失败时返回 status="error"。
    """
    raw = send_post_request("GetMissilePartGroups", {})
    if isinstance(raw, dict) and raw.get("status") == "success":
        return raw.get("result", raw)
    return raw