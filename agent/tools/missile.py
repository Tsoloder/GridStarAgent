"""导弹弹翼各向异性网格处理工具。

对生成好的导弹表面非结构网格的弹翼部分做各向异性处理，
在 SuperFacePointCloudProcessorMissile 完成分部件处理之后调用。

导弹弹翼分组命名规则（数字后缀表示第几对弹翼）：
  wingLeadingEdge1, wingTrailingEdge1, wingSideSurface1, wingTip1
  wingLeadingEdge2, wingTrailingEdge2, wingSideSurface2, wingTip2
  wingLeadingEdge3, wingTrailingEdge3, wingSideSurface3, wingTip3

处理流程（每对弹翼独立处理）：
  1. 获取弹体尺寸（弹径、弹长）
  2. 按对取该对弹翼的弦长，算 MAC（🔴 直读尺寸缓存 `wings` 数组，不经 SPIDER::Data）
  3. 分类网格线（前缘、后缘长/短、翼身交线、翼梢交线）
  4. 分布设置
  5. 创建各向异性线组
  6. 应用各向异性域（含翼后缘面，三角翼/梯形翼一视同仁）
  7. 弹尾加密
"""

from client import send_post_request
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
          # 🔴 带序号：N = 第几对翼（1/2/3），如 wingSideSurface1 / wingLeadingEdge2
          {"group_name": "wingLeadingEdge1",  "faces": [...]},  # 第 1 对翼前缘面
          {"group_name": "wingTrailingEdge1", "faces": [...]},  # 第 1 对翼后缘面
          {"group_name": "wingSideSurface1",  "faces": [...]},  # 第 1 对翼侧面
          {"group_name": "wingTip1",          "faces": [...]},  # 第 1 对翼梢面（仅梯形翼存在）
        ]

        faces 为整数 face_id 列表，对应数模原始面。该结果直接用于后续 UGSpitAssembly 分组操作。

        🔴 **本工具同时产出尺寸**：流水线第 10 步会实算全弹长/弹径/弹尾口径/球头半径/
        各对翼弦长与半翼展，缓存供 `GetMissileDimensions()` 读取。因此
        **必须先跑本工具，`GetMissileDimensions()` 才有值**（否则返回 `{}`）。

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

    ⚠️ **本工具已判定不可用**（两轮实测恒返回 "true" 但 `GetDomainsByType(3)` 仍空，
    外场未实建）。外场统一改走 `UGBlockCreate`（`chooseParam` 第 3 位 = 3 弓形）。
    本函数仅为后端补实现后恢复保留。

    弓形外场规则（2026-10-13 用户改定）：
    - 外场尺寸：1.2 倍全弹长
    - 头部距离：0.2 倍弹径 D（D = `body_diameter`，🔴 **取 GetMissileDimensions 接口值**，
      2026-10-14 起不再使用固定值 1196.076）

    场景三（无外场 + 全模）和场景四（无外场 + 半模）时调用。
    场景一/二已有外场时不需要调用。

    Args:
        fuselageLength: 全弹长（外场基准尺寸），来自 GetMissileDimensions 的 fuselage_length 字段（🔴 单位 mm）。
        noseRadius: 🔴 **参数名与语义已不符，待后端同步**——按新规则头部距离应为
            `0.2 × 弹径 D`（D 取 GetMissileDimensions 的 `body_diameter` 接口值），
            当前形参/线协议字段名仍为 `noseRadius`。
            恢复本工具前须与后端确认改名为 `bodyDiameter`。

    Returns:
        成功时返回 status="success"，result 为 JSON：
        {
          "farFieldId": 50,                    # 创建的外场 domain ID
          "farFieldName": "MissileFarField",   # 外场名称
          "farFieldSize": 3000.0,              # 实际外场尺寸（= 1.2 × fuselageLength）
          "headDistance": 1794.1               # 头部距离（= 0.2 × bodyDiameter）
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

    🔴 **2026-10-17 变更：尺寸恢复为几何实算，必须先跑 ClassifyMissile**。
    后端尺寸不再写死 —— 由分割流水线第 10 步 `CalculateDimensions()` 从点云与
    分割结果实算，并缓存；本接口读同一份缓存。因此：

    - **未跑过 ClassifyMissile → 缓存为空 → 本接口返回 `{}`**（不再是固定数值）
    - 正确顺序：`ClassifyMissile` → `GetMissileDimensions` → 各向异性

    🔴 **单位：全部为毫米（mm）**，**直接传参，禁止再 ×1000**。

    🔴 **本接口返回值是尺寸的唯一权威来源**（2026-10-14 用户裁定）——旧固定值
    D=1196.076 / C_root=3225 / 半翼展=1655.038 **已作废**，一律用接口值。

    返回参数（11 项，后端 snake_case 字段；参数能拿到就全部保留，不删除）：
    - fuselage_length: 全弹长（外场基准，弓形外场 = 1.2× 全弹长）
    - body_diameter: 弹径 D（头锥/弹体交接处口径；🔴 球头公式与各向异性用此值）
    - body_diameter_tail: 弹尾口径
    - body_diameter_surface: 🔴 **表面网格有效弹径**（后端已按失配规则算好）——
      `|D − D_tail| > 0.7 × D_tail` 时为尾口径，否则 = D。🔴 **表面网格非球头部分用此值；
      球头公式例外**，仍用原始 `body_diameter` + `nose_radius`
    - nose_radius: 球头半径（球头网格尺寸基准 R；尖头时为 0）
    - wing_root_chord: 翼根弦长（弹翼网格尺寸基准）
    - wing_tip_chord: 翼梢弦长（弹翼梢部网格尺寸；三角翼按 §M2 钳制）
    - wing_half_span: 半翼展（展向分布用）
    - fin_root_chord: 舵根弦长（🔴 当前型号无舵，但接口如有返回则保留该参数）
    - fin_tip_chord: 舵梢弦长（同上）
    - fin_clearance_height: 舵底间隙高度（舵底到弹体距离 H；同上）

    ★ 多对弹翼（`wings` 数组，2026-10-16 新增）：
    上面三个 `wing_*` 标量键描述的是**第 1 对**弹翼。多对翼时改用 `wings`
    数组按对取值 —— 下标 0 即第 1 对（与上面三个标量键同源，两处数值必须一致）：

    - wings[i].root_chord: 第 i+1 对弹翼的翼根弦长 C_root（单位 mm）
    - wings[i].tip_chord:  第 i+1 对弹翼的翼梢弦长 C_tip
    - wings[i].half_span:  第 i+1 对弹翼的半翼展 b/2（🔴 仅报告字段，不参与任何计算）

    数组长度 = 分割实算出的翼对数（按组名 `wing*N` 的 N 推出），与模型实际对数一致。
    每对翼只需 root_chord + tip_chord 即可确定全部网格尺寸：
    各向异性加密尺寸 = 0.005 × MAC、翼面尺寸 = 0.02 × MAC，
    其中 MAC = (2/3)·C_root·(1+λ+λ²)/(1+λ)，λ = C_tip/C_root。

    分部件命名（多对翼）：`wingLeadingEdgeN` / `wingTrailingEdgeN` /
    `wingSideSurfaceN` / `wingTipN`，N 从 1 开始（第 N 对翼，`wingTipN` 仅梯形翼存在）。

    > 🔴 **本接口还有副作用**：返回值会写回后端 `SPIDER::Data`，供各向异性一站式工具
    > `MissileAnisoProcessMissileAnisotropy()` 读取。🔴 **各向异性阶段之前必须先调用本接口**。
    > 🔴 2026-10-16 起各向异性侧的弹翼尺寸**不再经 `SPIDER::Data` 中转**，改为按对
    > 直读尺寸缓存的 `wings` 数组（`GetWingPairDims`）——该缓存与 `GetMissileDimensions`
    > 同源（都由分割流水线第 10 步写入），`wings` 缺失的对会回退到第 1 对。

    > 命名提示：本模块函数名与参数名统一使用现行命名 —— `wing` = 弹翼、`fuselage` = 弹体；当前型号无舵(`fin`)、无舵轴(`finshaft`)，相关分组不再出现。几何尺寸参数 `fin_*_chord` / `fin_clearance_height` **按后端实际返回值处理**：有值则保留使用，未返回则忽略。

    Returns:
        成功时返回 status="success"，result 为 JSON 字符串：
        {
          "fuselage_length": 12500.0,
          "body_diameter": 800.0,
          "body_diameter_tail": 700.0,
          "body_diameter_surface": 800.0,
          "nose_radius": 50.0,
          "wing_root_chord": 1200.0,
          "wing_tip_chord": 800.0,
          "wing_half_span": 2500.0,
          "fin_root_chord": 600.0,
          "fin_tip_chord": 400.0,
          "fin_clearance_height": 50.0,
          "wings": [
            {"root_chord": 1200.0, "tip_chord": 800.0, "half_span": 2500.0},
            {"root_chord": 1100.0, "tip_chord": 700.0, "half_span": 2100.0}
          ],
          "unit": "mm"
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


def MissileAnisoProcessMissileAnisotropy():
    """导弹弹翼各向异性处理主入口 — 依次执行全部子步骤并汇总结果。

    包含：尺寸数据获取 → 弹翼线分类 → 分布设置 →
          各向异性线组创建 → 各向异性域生成（含翼后缘面）→ 弹尾加密。

    参数基准（需求，见 missile-anisotropic-mesh.md §「各位置参数」）：
      - 加密尺寸 = 0.005 × 当地弦长（首层高度，窄面/侧面/结合处/弹尾一律）
      - 翼面尺寸 = 0.02  × 当地弦长（分布中间值）
      - 增长率：窄面（前缘/后缘/翼梢）＝ 1；侧面/结合处/弹尾 ＝ 1.2
      - 层数统一 50
      - 多对翼 N=1/2/3 逐对各按自身弦长处理；弹尾取弦长最大的那一对翼的参数
      - 🔴 当地弦长源自 GetMissileDimensions 接口值（单位 mm），**无固定值**

    🔴 **尺寸来源（2026-10-16 按对 / 2026-10-17 实算）**：弹体尺寸（D / 弹尾口径 / 球头半径）由后端读
      `GetMissileDimensions` 写回 `SPIDER::Data` 的值；🔴 **弹翼尺寸按对直读尺寸缓存**
      `wings` 数组（`GetWingPairDims`，下标 = 对数−1），**已不经 `SPIDER::Data` 中转**。
      该缓存由 `ClassifyMissile` 第 10 步 `CalculateDimensions()` 实算写入（2026-10-17 起，
      不再有手写固定表），`wings` 缺失的对自动回退到第 1 对。
      🔴 **因此必须先跑 `ClassifyMissile`**，否则缓存为空、本工具因弹体尺寸守卫 `failed`。

    翼后缘面：🔴 不单独处理、不按翼型分支 —— 后缘面已在各向异性域生成步骤内
      一并做各向异性（generateType=2，增长率取窄面 1），**三角翼 / 梯形翼同款，
      只要是翼后缘就做各向异性**。无独立「后缘处理」阶段，也不做狭长面。

    弹尾：🔴 **与翼侧面同套参数**（首层＝加密尺寸、增长率 1.2、层数 50）——
      后端会先建 `tailJunction` 各向异性线组再生成域，使弹尾与弹体结合那一圈
      的密度与翼身结合处一致（2026-10-14 修）。

    前置条件：
      - 🔴 **`GetMissileDimensions` 已调用**（尺寸写回后端；未调用 → 本工具 `failed`）
      - 导弹表面非结构网格已生成
      - 导弹子部件分割已完成
      （fuselage, nose, tail, wingLeadingEdgeN, wingTrailingEdgeN,
       wingSideSurfaceN, wingTipN 等分组已存在）

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","steps":{
            "step1_dimensions":..., "step2_classify":..., "step3_distribution":...,
            "step4_aniso_line_groups":..., "step5_aniso_domains":..., "step6_tail":...}}
    """
    return send_post_request("MissileAnisoProcessMissileAnisotropy", {})
