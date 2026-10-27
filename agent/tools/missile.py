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
        各对翼弦长与翼厚度，缓存供 `GetMissileDimensions()` 读取。因此
        **必须先跑本工具，`GetMissileDimensions()` 才有值**（否则返回 `{}`）。

        失败时返回 status="error"。
    """
    return send_post_request("ClassifyMissile", {
        "serverHost": serverHost,
        "serverPort": serverPort,
        "outputDir": outputDir
    }, timeout=360)


def MissileSurfMeshGenerate():
    """导弹表面网格生成 + 网格线合并（碎线合并），按导弹口径逐组自动设置尺寸。

    后端接口名 `MissileSurfMeshGenerate`，无入参。内部两步：
      1. `[UGSur]` 表面网格生成
      2. `DomainAPI::MergeEdges` 网格线合并（碎线合并，remshType=false 走标准 Remesh）

    🔴 **尺寸口径全部由后端从 `SPIDER::Data` 取**（先跑 `ClassifyMissile` 或
    `GetMissileDimensions` 写回），组名决定取哪一口径 —— **Agent 侧不传任何尺寸**：

    | 分组名 | 目标尺寸 | 说明 |
    |---|---|---|
    | `nose`（球头，R>0） | `min(0.01×D, 0.25×R)` | 球头公式**用原始弹径 D**，不用 surface 口径 |
    | `nose`（尖头，R=0） | `0.005×D` | |
    | `fuselage` / `tail` | `0.05×D_tail` | 弹尾口径 |
    | `wing*N` | `0.02×C_root(N)` | 第 N 对翼，N = 组名末尾数字；`C_root(N)` = `wings[N−1].root_chord` |

    所有分组：最小尺寸 = 目标 × 1/10，曲率自适应角度 = 10。

    ⚠️ 口径缺失的组**保持原值**（不报错、不中断）——`wing*N` 取不到该对弦长时跳过该组。

    🔴 **前置条件（顺序不可颠倒）**：
      1. 数模已导入（`ImportCADFile`）
      2. `ClassifyMissile` 已完成（分组已建 + 尺寸已实算缓存）
      3. `GetMissileDimensions` 已调用（尺寸写回 `SPIDER::Data`）

    推荐的完整导弹流程顺序：`ClassifyMissile` → `GetMissileDimensions`
    → **`MissileSurfMeshGenerate`** → `MissileBodyAnisoProcessBodyAnisotropy`（弹身）
    → `MissileWingAnisoProcessWingAnisotropy`（弹翼）。🔴 **各向异性阶段先弹身、后弹翼，顺序固定。**

    Returns:
        成功时返回 `"true"`（后端 `testSurf()` 为 void，无结构化返回值）。
        🔴 本工具**不做返回后核验**：返回 `"true"` 即视为该阶段结束，
        不查询网格统计、不比对尺寸数值。副作用：模型树已刷新、全部外部可见对象已选中。
    """
    return send_post_request("MissileSurfMeshGenerate", {})


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


def CreateMissileFarFieldBlock():
    """创建导弹弓形外场（一键，无参数）。

    后端接口名 `CreateMissileFarFieldBlock`。取代此前 Agent 侧的
    「查外场 → 取默认参数 → 查分组面 ID → 查网格线 → 查端点 → 自己推轴向 →
    再调 UGBlockCreate」这一串（6 次往返 + 一段几何推理），全部收进后端一次完成：

    1. **弹头方向**：`nose` 分组与 `fuselage` 分组各自的网格包围盒中心比较，
       取弹体最长轴为主轴、比较该轴上两中心的先后，映射为 `chooseParam` 第 1 位
       **`0=+X` `1=−X` `2=+Y` `3=−Y` `4=+Z` `5=−Z`**（= 2×轴号 + 正向 0 / 负向 1）。
    2. **`geoParam`**：外场长度 = **`1.5 × 全弹长`**，另外 3 位 = **`1.2 × 全弹长`**。
    3. **`centerCoor`**：模型包围盒中心（自动排除外场面 `OUTFILED` 与加密源面 `SOURCE`）。
    4. **`meshSizeOrDimension`**：默认网格尺寸 × (外场长度 / 默认 geoParam)，等比缩放。
    5. 拼 `[UGBlockCreate]` 参数并执行（`chooseParam` 第 2 位 `0`=外场、第 3 位 `3`=弓形）。

    🔴 **前置条件（顺序不可颠倒）**：
      1. 数模已导入，且**表面网格已生成**（外场需要封闭的物面网格才能体填充）
      2. `ClassifyMissile` 已完成 —— 后端靠 `nose` / `fuselage` 分组定弹头方向
      3. `GetMissileDimensions` 已调用 —— 全弹长写回 `SPIDER::Data`，否则返回 failed

    🔴 **不做返回后核验**：返回 `status="success"` 即视为外场已建，
    直接进入空间网格（`UGUGSp`）阶段；不查外场面数量、不核对包围盒。
    唯失败路径需要处理：`status="failed"` 时按 `message` 排查。

    Returns:
        JSON 字符串，格式：
        - 成功：`{"status":"success","geoParam":"1.5L,1.2L,1.2L,1.2L",
          "chooseParam":"<方向位>,0,3","centerCoor":"x,y,z",
          "meshSizeOrDimension":<缩放后网格尺寸>,"headDirection":<0-5>}`
          （回显后端**实际使用**的参数，便于排查；**不要求 Agent 据此复算**）
        - 失败：`{"status":"failed","message":"..."}`，常见 message：
          - `fuselage_length is 0: run ClassifyMissile + GetMissileDimensions first`
          - `empty model bounding box`（无物面网格）
          - `UGBlockCreate returned false (see sealing check)`
            → 转 `missile-mesh-sealing-check.md` 排查不封闭线
    """
    return send_post_request("CreateMissileFarFieldBlock", {})


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

    返回参数（后端 snake_case 字段；参数能拿到就全部保留，不删除）：
    - fuselage_length: 全弹长（外场基准，弓形外场 = 1.2× 全弹长）
    - body_diameter: 弹径 D（头锥/弹体交接处口径；🔴 球头公式与各向异性用此值）
    - body_diameter_tail: 弹尾口径
    - body_diameter_surface: 🔴 **表面网格有效弹径**（后端已按失配规则算好）——
      `|D − D_tail| > 0.7 × D_tail` 时为尾口径，否则 = D。🔴 **表面网格非球头部分用此值；
      球头公式例外**，仍用原始 `body_diameter` + `nose_radius`
    - nose_radius: 球头半径（球头网格尺寸基准 R；尖头时为 0）
    - wing_root_chord: 翼根弦长（= `wings[0].root_chord`，第 1 对便捷入口）
    - wing_thickness: 🔴 **翼厚度（翼后缘处）**（= `wings[0].thickness`，第 1 对便捷入口）
    - fin_root_chord: 舵根弦长（🔴 当前型号无舵，但接口如有返回则保留该参数）
    - fin_tip_chord: 舵梢弦长（同上）
    - fin_clearance_height: 舵底间隙高度（舵底到弹体距离 H；同上）

    🔴 **已移除的散标量键**：`wing_tip_chord` / `wing_half_span` —— 2026-10-21 起
    不再是尺寸键（值都在 `wings[]` 里），不再写回后端 `Data`。

    ★ 多对弹翼（`wings` 数组，2026-10-16 新增，2026-10-21 加 `thickness`）：
    `wing_root_chord` / `wing_thickness` 两个标量键描述的是**第 1 对**弹翼。
    多对翼时改用 `wings` 数组按对取值 —— 下标 0 即第 1 对（与标量键同源，两处数值必须一致）：

    - wings[i].root_chord: 第 i+1 对弹翼的翼根弦长 C_root（单位 mm）
    - wings[i].tip_chord:  第 i+1 对弹翼的翼梢弦长 C_tip
    - wings[i].half_span:  第 i+1 对弹翼的半翼展 b/2（🔴 仅报告字段，不参与任何计算）
    - wings[i].thickness:  🔴 第 i+1 对弹翼的**翼厚度（翼后缘处）**（单位 mm）——
      根部区尾缘带先按径向方向拆成单片翼，逐片沿翼面法向（径向切向）取
      2%~98% 分位跨度，多片取中位。**弹翼各向异性的首层高度/端间距基准**

    数组长度 = 分割实算出的翼对数（按组名 `wing*N` 的 N 推出），与模型实际对数一致。

    分部件命名（多对翼）：`wingLeadingEdgeN` / `wingTrailingEdgeN` /
    `wingSideSurfaceN` / `wingTipN`，N 从 1 开始（第 N 对翼，`wingTipN` 仅梯形翼存在）。

    > 🔴 **本接口还有副作用**：返回值会写回后端 `SPIDER::Data`，供
    > `MissileSurfMeshGenerate`（表面网格逐组尺寸）、
    > `MissileWingAnisoProcessWingAnisotropy`（弹翼各向异性）与
    > `MissileBodyAnisoProcessBodyAnisotropy`（弹身各向异性，读标量 `body_diameter_tail`）读取。
    > 🔴 **三者之前都必须先调用本接口**，否则弹翼按对数组为空 / 弹尾口径为 0、各向异性工具守卫 `failed`。

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
          "wing_thickness": 40.0,
          "fin_root_chord": 600.0,
          "fin_tip_chord": 400.0,
          "fin_clearance_height": 50.0,
          "wings": [
            {"root_chord": 1200.0, "tip_chord": 800.0, "half_span": 2500.0, "thickness": 40.0},
            {"root_chord": 1100.0, "tip_chord": 700.0, "half_span": 2100.0, "thickness": 36.0}
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


def MissileWingAnisoProcessWingAnisotropy():
    """导弹弹翼各向异性处理主入口 — 每对弹翼独立处理，依次执行全部子步骤并汇总。

    后端接口名 `MissileWingAnisoProcessWingAnisotropy`（`MissileWingAnisotropicProcessor`），
    替换旧一站式 `MissileAnisoProcessMissileAnisotropy`。内部 5 步：

    | 步骤 | 功能 |
    |---|---|
    | 1 | 尺寸与对数（按对取根弦长 + 厚度） |
    | 2 | 短边/长边分类（每对 4 类短边 / 7 类长边） |
    | 3 | 分布设置（短边双曲正切；长边增长分布） |
    | 4 | 各向异性线组创建（每对两组 A/B，type=1） |
    | 5 | 各向异性域生成（组A 增长率 1 / 组B 增长率 1.2，type=0 generateType=2） |

    分组命名（数字后缀 = 第几对弹翼 N = 1,2,...）：
      `wingLeadingEdgeN` / `wingTrailingEdgeN` / `wingSideSurfaceN` / `wingTipN`
      （`wingTipN` 仅梯形翼存在；三角翼缺失属正常，不影响其余位置）

    尺寸口径（全部取自后端 `SPIDER::Data`，**Agent 侧不传参**）：
      - 首尾端间距 / 各向异性首层高度 = `0.1 × 该对弹翼厚度`（`thickness`，翼后缘处厚度）
      - 长边分布中间值 = `0.02 × 该对翼根弦长`
      - 增长率：组A（窄面）＝ 1；组B（侧面/结合处）＝ 1.2；层数统一 50

    🔴 **尺寸来源（2026-10-22 起）**：弹翼尺寸按对读 `SPIDER::Data` 的数组
      `m_wingRootChords[N−1]` / `m_wingThicknesses[N−1]`。这两个数组由
      `CalculateDimensions()` 实算、经 `GetMissileDimensions` 接口写回。
      🔴 **因此 `GetMissileDimensions` 必须先调用**，否则数组为空、本工具守卫 `failed`。
      （旧文档所述「直读尺寸缓存 `wings`、不经 `SPIDER::Data` 中转」**已作废**。）

    ⚠️ **不含弹尾加密**（旧一站式 step6 已随旧工具弃用）。弹尾各向异性由后续
      独立工具处理，本工具不做、也不应期待返回里有 `step6_tail`。

    🔴 **🔴 执行顺序：本工具（弹翼）必须在 `MissileBodyAnisoProcessBodyAnisotropy`
      （弹身）**之后**执行。** 各向异性阶段固定两步：**① 弹身
      `MissileBodyAnisoProcessBodyAnisotropy` → ② 弹翼 `MissileWingAnisoProcessWingAnisotropy`**。
      🔴 **不得调换、不得只调其中一个** —— 漏一个该部分就没有各向异性。

    前置条件：
      - 🔴 `ClassifyMissile` 与 `GetMissileDimensions` 均已调用（尺寸已写回后端）
      - 导弹表面网格已生成（`MissileSurfMeshGenerate`）
      - 🔴 **弹身各向异性已完成**（`MissileBodyAnisoProcessBodyAnisotropy`）
      - 上述带序号弹翼子面分组（`wingSideSurfaceN` 等）已存在

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","message":"...","steps":{
            "step1_dimensions":..., "step2_classify":..., "step3_distribution":...,
            "step4_aniso_line_groups":..., "step5_aniso_domains":...}}
        🔴 **只看顶层 `status` 判定成败**；`steps.*` 是执行信息，其中出现 `failed`
        不代表整体失败，不得据此判定失败或重复执行。
    """
    return send_post_request("MissileWingAnisoProcessWingAnisotropy", {})


def MissileBodyAnisoProcessBodyAnisotropy():
    """导弹弹身各向异性处理主入口 — 对 `fuselage` 组做各向异性，依次执行全部子步骤并汇总。

    后端接口名 `MissileBodyAnisoProcessBodyAnisotropy`（`MissileBodyAnisotropicProcessor`），
    内部 5 步：

    | 步骤 | 功能 |
    |---|---|
    | 1 | 尺寸（弹尾口径 `m_bodyDiameterTail`，mm） |
    | 2 | 弹身网格线分类（A~E 五类） |
    | 3 | 分布设置（A / B / E 三类平滑增长分布） |
    | 4 | 各向异性线组创建（2 组，type=1） |
    | 5 | 各向异性域生成（2 组面，type=0 generateType=2） |

    弹身网格线分类（"属于弹身"即 `fuselage` 组）：
      - **A** 与【弹尾】和【弹翼后缘】同时共点的弹身线
      - **B** 与【弹翼前缘】和【弹翼后缘】同时共点的弹身线
      - **C** 弹尾 ∩ 弹身 交线
      - **D** 弹头 ∩ 弹身 交线（并记录其首尾端间距平均值）
      - **E** 与【弹头】共点的弹身线

    分布设置（平滑分布 ReDimension[6]）：
      - **A**：首尾端间距 = 与之共点的弹翼后缘线端间距；增长分布 1.2/1.2
      - **B**：前缘相邻端取前缘线端间距、后缘相邻端取后缘线端间距；增长分布 1.2/1.2
      - **E**：与弹头共点端 = D 的首尾端间距平均值，另一端**不变**；增长分布 1.2/1.2
      - 中间值：A/B 与 E **同为 `0.05 × 弹尾口径`**
        （🔴 2026-10-26 由旧的 `0.025×`（A/B）、`0.04×`（E）统一改成 `0.05×`，旧比例已作废）

    各向异性线组（type=1，共 2 组，增长率统一 1.2、层数统一 50）：
      - `bodyTailJunction`（C：弹尾∩弹身）：面 = 与弹尾相交的弹身面 + 弹尾面；
        首层高度 = A 的端间距
      - `bodyNoseJunction`（D：弹头∩弹身）：面 = 弹头面 + 其余未处理的弹身面；
        首层高度 = D 的首尾端间距平均值
      - 首层高度取不到正值时**兜底退 `0.05 × 弹尾口径`**，不报错、不失败

    🔴 **尺寸来源**：标量 `SPIDER::Data::m_bodyDiameterTail`（弹尾口径，mm），
      由 `GetMissileDimensions` 接口写回。🔴 **因此 `GetMissileDimensions` 必须先调用**，
      否则为 0、本工具守卫 `step1_dimensions` 直接 `failed`。

    🔴 **🔴 执行顺序：本工具（弹身）必须先于 `MissileWingAnisoProcessWingAnisotropy`（弹翼）执行。**
      各向异性阶段固定两步：**① 弹身 `MissileBodyAnisoProcessBodyAnisotropy` → ② 弹翼
      `MissileWingAnisoProcessWingAnisotropy`**。🔴 **不得调换、不得只调其中一个**
      —— 漏一个该部分就没有各向异性，顺序颠倒不符合规定流程。

    前置条件：
      - 🔴 `ClassifyMissile` 与 `GetMissileDimensions` 均已调用（分组已建、尺寸已写回）
      - 导弹表面网格已生成（`MissileSurfMeshGenerate`）
      - `fuselage` / `tail` / `nose` / `wing*N` 分组已存在

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","message":"...","steps":{
            "step1_dimensions":..., "step2_classify":..., "step3_distribution":...,
            "step4_aniso_line_groups":..., "step5_aniso_domains":...}}
        🔴 **只看顶层 `status` 判定成败**；`steps.*` 是执行信息。
    """
    return send_post_request("MissileBodyAnisoProcessBodyAnisotropy", {})
