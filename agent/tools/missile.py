"""GridStar 导弹业务域工具。

导弹模块专用工具函数集合。
与飞机模块（advanced.py / query.py）完全分离，不修改飞机任何代码。
所有函数均为 thin wrapper，通过 send_post_request 调用 GridStar 服务端（127.0.0.1:1313）。

对齐 t.py 知识库 7 模块工作流。
"""

import json

from client import send_post_request


# ===== 导弹 advanced 类工具 =====


def ProcessWithServerForMissile(serverHost: str, serverPort: int, outputDir: str):
    """导弹 5 类 AI 部件分割：对当前已导入的导弹数模执行水密性处理 → 表面网格生成 → 点云导出 → 远程 AI 分割(5类) → 几何法翼面子部件分割 → 各部件自动染色 → 清除网格。

    分割类别（5 类）：
    - nose(弹头)：球头 / 尖锐头部
    - body(弹体)
    - fin(弹翼)：三角翼 / 梯形翼
    - rudder(舵)：控制部件，底部与弹体有间隙，含舵轴/尾舵
    - tail(尾部)：尾部收段

    fin 组进一步拆分为：
    - finLeadingEdge / finTrailingEdge / finUpperSurface / finLowerSurface / finTip / finRoot

    rudder 组进一步拆分为：
    - rudderLeadingEdge / rudderTrailingEdge / rudderUpperSurface / rudderLowerSurface / rudderTip / rudderRoot

    使用场景：导弹数模文件已通过 ImportCADFile 导入到 GridStar 中。
    执行后返回 JSON 格式的部件分割结果（含翼面子部件）。

    Args:
        serverHost: 远程推理服务器 IP 地址。
        serverPort: 远程推理服务器端口号。
        outputDir: 点云文件和分割结果 JSON 的输出目录。

    Returns:
        成功时返回 status="success"，result 为以下格式的列表（共 12 个分组，5 大类 + 7 子面）：

        [  # ===== 5 大类 =====
          {"group_name": "nose",  "faces": [...]},   # 弹头（球头或尖头）
          {"group_name": "body",  "faces": [...]},   # 弹体
          {"group_name": "fin",   "faces": [...]},   # 弹翼（大类，含以下 6 个细分）
          {"group_name": "rudder","faces": [...]},   # 舵（大类，含以下 6 个细分）
          {"group_name": "tail",  "faces": [...]},   # 尾部
          # ===== fin 子面 =====
          {"group_name": "finLeadingEdge",   "faces": [...]},  # 翼前缘面
          {"group_name": "finTrailingEdge",  "faces": [...]},  # 翼后缘面
          {"group_name": "finUpperSurface",  "faces": [...]},  # 翼上表面
          {"group_name": "finLowerSurface",  "faces": [...]},  # 翼下表面
          {"group_name": "finTip",           "faces": [...]},  # 翼梢面（梯形翼才有）
          {"group_name": "finRoot",          "faces": [...]},  # 翼根部面
          # ===== rudder 子面 =====
          {"group_name": "rudderLeadingEdge",  "faces": [...]},  # 舵前缘面
          {"group_name": "rudderTrailingEdge", "faces": [...]},  # 舵后缘面
          {"group_name": "rudderUpperSurface", "faces": [...]},  # 舵上表面
          {"group_name": "rudderLowerSurface", "faces": [...]},  # 舵下表面
          {"group_name": "rudderTip",          "faces": [...]},  # 舵梢面
          {"group_name": "rudderRoot",         "faces": [...]},  # 舵根部面
        ]

        faces 为整数 face_id 列表，对应数模原始面。该结果直接用于后续 UGSpitAssembly 分组操作。
        失败时返回 status="error"。
    """
    return send_post_request("ProcessWithServerForMissile", {
        "serverHost": serverHost,
        "serverPort": serverPort,
        "outputDir": outputDir
    }, timeout=360)


def DetermineFinTEDirection(finTeDomainId: int, finTipDomainId: int, longIds: str):
    """导弹翼后缘方向判定：判断两条长边哪端靠近翼梢。

    封装了后缘面与翼梢面的网格线拓扑匹配逻辑，
    调用者不需要关心网格线间的几何关系细节。

    Args:
        finTeDomainId: 翼后缘网格面 ID（来自 GetSpliteAssemlyDomains("finTrailingEdge")）。
        finTipDomainId: 翼梢网格面 ID（来自 GetSpliteAssemlyDomains("finTip")）。
        longIds: MergeEdgesByDomain 返回的 longids，直接传入，如 "[101, 102]"。
            调用链：先用 GetSpliteAssemlyDomains("finTrailingEdge") 拿后缘面 domain，
            再调 MergeEdgesByDomain(domainId) → 取返回的 longids 字段，直接传入本参数。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "intersection_connector_id": 105,   # 与翼梢面共点的网格线 ID
          "longEdge_tip_end": ["end", "start"] # 两条长边各自靠近翼梢的一端（"start"或"end"）
        }
        失败时返回 status="error"。
    """
    return send_post_request("DetermineFinTEDirection", {
        "finTeDomainId": finTeDomainId,
        "finTipDomainId": finTipDomainId,
        "longIds": longIds
    })


def IdentifyFinLeadingEdge(
    finUpperIds: str,
    finLowerIds: str,
    bodyIds: str,
    finTipIds: str):
    """识别导弹翼前缘线及其与弹体、翼梢的共点关系。

    通过翼上表面、下表面、弹体和翼梢的网格面 ID 集合，
    自动找出前缘线（翼上表面与下表面的共边），
    并判断前缘线与弹体、翼梢的共点关系（首点还是尾点）。

    使用场景：表面网格生成后，已有翼上/下表面、弹体、翼梢的 domain ID 集合。
    调用前需先通过 GetSpliteAssemlyDomains 获取各分组的 domain ID 列表。

    Args:
        finUpperIds: 翼上表面的网格面 ID 列表，逗号分隔，如 "1,2,3"。
            来源：GetSpliteAssemlyDomains("finUpperSurface") → domains 字段，用逗号拼接。
        finLowerIds: 翼下表面的网格面 ID 列表，逗号分隔，如 "4,5,6"。
            来源：GetSpliteAssemlyDomains("finLowerSurface")。
        bodyIds: 弹体的网格面 ID 列表，逗号分隔，如 "7,8"。
            来源：GetSpliteAssemlyDomains("body")。
        finTipIds: 翼梢的网格面 ID 列表，逗号分隔，如 "9,10"。
            来源：GetSpliteAssemlyDomains("finTip")。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "leading_edge_ids": [101, 102, 103],   # 前缘线 ID 列表（finUpper 与 finLower 的共边）
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
    return send_post_request("IdentifyFinLeadingEdge", {
        "finUpperIds": finUpperIds,
        "finLowerIds": finLowerIds,
        "bodyIds": bodyIds,
        "finTipIds": finTipIds
    })


def CreateMissileFarField(bodyLength: float, headRadius: float):
    """创建导弹弓形外场。

    弓形外场规则（对齐 t.py 模块 6）：
    - 外场尺寸：1 倍弹体长度
    - 头部距离：1.5 倍头部半径

    场景三（无外场 + 全模）和场景四（无外场 + 半模）时调用。
    场景一/二已有外场时不需要调用。

    Args:
        bodyLength: 弹体长度（外场基准），来自 GetMissileModelParameters 的 bodyLength 字段。
        headRadius: 头部半径（球头半径 R，用于计算头部距离），
            来自 GetMissileModelParameters 的 ballNoseRadius 字段。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "farFieldId": 50,                    # 创建的外场 domain ID
          "farFieldName": "MissileFarField",   # 外场名称
          "farFieldSize": 3000.0,              # 实际外场尺寸（= bodyLength）
          "headDistance": 45.0                 # 头部距离（= 1.5 × headRadius）
        }
        失败时返回 status="error"。
    """
    return send_post_request("CreateMissileFarField", {
        "bodyLength": bodyLength,
        "headRadius": headRadius
    })


def CheckMissileMeshQuality(domainIds: str = "", blockIds: str = ""):
    """导弹网格质量综合检查。

    面网格质量标准（对齐 t.py 模块 7.1）：
    - 除去各向异性单元，最小角 > 10°
    - 若 < 10°：查看位置，模型特征导致的可不处理

    体网格质量标准（对齐 t.py 模块 7.2）：
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


def GetMissileModelParameters():
    """获取导弹全部关键几何参数。

    需先运行导弹版 AI 分割（ProcessWithServerForMissile）方能获取有效值。

    返回参数（对齐 t.py 模块 2）：
    - bodyDiameter (D): 弹径，头部锥体与弹体交接处直径或弹尾直径（单位：mm）
    - ballNoseRadius (R): 球头半径（单位：mm），sharp 头时可能为 0
    - finRootChord (C_root): 翼/舵根部弦长，前缘到尾缘距离（单位：mm）
    - finTipChord (C_tip): 翼/舵梢部弦长（单位：mm），三角翼时 C_tip ≈ 0
    - rudderGapHeight (H): 舵底间隙高度，舵底到弹体距离（单位：mm）
    - characteristicLength (L): 特征长度，软件自行计算（单位：mm）
    - bodyLength: 弹体长度（单位：mm），用于外场基准
    - noseType: 头部类型，"ball"（球头）或 "sharp"（尖锐头部）

    Returns:
        成功时返回 status="success"，result 为 JSON 字符串：
        {
          "bodyDiameter": 300.0,
          "ballNoseRadius": 30.0,
          "finRootChord": 200.0,
          "finTipChord": 100.0,
          "rudderGapHeight": 5.0,
          "characteristicLength": 300.0,
          "bodyLength": 3000.0,
          "noseType": "ball"
        }
        失败时返回 status="error"。
    """
    raw = send_post_request("GetMissileModelParameters", {})
    if isinstance(raw, dict) and raw.get("status") == "success":
        return raw.get("result", raw)
    return raw