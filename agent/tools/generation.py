"""GridStar generation 业务域工具。"""

from client import send_post_request


def UGSur(type: int, ids: str, targetSize: float, minSize: float, adaptAngle: float, way: int):
    """表面网格生成.操作成功后,应该提示用户使用“体创建”功能,或者将选择的网格进行加密或者稀疏操作.

    如果是希望网格变密,则增大全局目标尺寸。网格变稀疏,则是减小全局目标尺寸.

    Args:
        type: 选择对象类型,0 表示超面,1 表示网格面.
        ids: 选择的对象 ID（根据 type 的值决定是什么对象）,例如"5,6,7".
        targetSize: 全局目标尺寸.
        minSize: 全局最小尺寸.
        adaptAngle: 曲率自适应角度.
        way: 选择的生成方案,默认值为 0。0 表示组合法,1 表示狭长面,2 表示四边形占优.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSur", {"type":type,"ids":ids,"targetSize":targetSize,"minSize":minSize,"adaptAngle":adaptAngle,"way":way})


def UGBlockCreate(geoParam: str, chooseParam: str, centerCoor: str, meshType: int, meshSizeOrDimension: float):
    """体创建.

    Args:
        geoParam: 字符串为 4 个浮点型参数组成,分别代表长度、半径、短轴、长轴,可以为"0.22,5.1,6.55,5.44".
        chooseParam: 字符串为 3 个整型参数组成,分别代表头部方向、体类型、外场类型,可以为"0,1,2"。默认值为"0,0,0"
                    其中头部方向值：0 表示 +X,1 表示 -X,2 表示 +Y,3 表示 -Y,4 表示 +Z,5 表示 -Z。
                    体类型值：0 表示外场,1 表示加密区域。
                    外场形状值：0 表示球形,1 表示立方体,2 表示圆柱,3 表示弓形。
        centerCoor: 中心点坐标,例如"5,6,7".
        meshType: 值为 0 或 1,用于控制参数 meshSizeOrDimension 的值,默认值为0.
        meshSizeOrDimension: 当 meshType 的值为 0 表示给定尺寸,为 1 表示期望点数,期望点数默认值为41.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGBlockCreate", {"geoParam":geoParam,"chooseParam":chooseParam,"centerCoor":centerCoor,"meshType":meshType,"meshSizeOrDimension":meshSizeOrDimension})


def UGUGSp(generateWay: int, ids: str, layer: int, growRate: float, caliperFirst: float, diffusionFactor: float):
    """生成空间网格.

    Args:
        generateWay: 生成方法,默认值为0。0 表示减层法, 1 表示综合法,2 表示层推法,.
        ids: 选择的网格块 ID,例如"5,6,7"
        layer: 附面层层数,默认值为60.
        growRate: 增长率,默认值为1.2.
        caliperFirst: 第一层厚度,默认值为0.001.
        diffusionFactor: 扩散因子,默认值为0.5.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGUGSp", {"generateWay":generateWay,"ids":ids,"layer":layer,"growRate":growRate,"caliperFirst":caliperFirst,"diffusionFactor":diffusionFactor})


def GenerateSurMeshBySpitAssemblyGroupProperty(ids: str, targetSize: float, minSize: float, adaptAngle: float, way: int, groupProperty: str):
    """根据分部件分组的属性生成表面网格.

    Args:
        ids: 选择的超面 ID,例如"5,6,7".
        targetSize: 全局目标尺寸.
        minSize: 全局最小尺寸.
        adaptAngle: 曲率自适应角度.
        way: 选择的生成方案,默认值为 0。0 表示组合法,1 表示狭长面,2 表示四边形占优.
        groupProperty: 此属性的内容为工具 GetAllSpitAssemblyGroupProperty() 返回的 JSON 字符串.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("GenerateSurMeshBySpitAssemblyGroupProperty", {"ids":ids,"targetSize":targetSize,"minSize":minSize,"adaptAngle":adaptAngle,"way":way,"groupProperty":groupProperty})


def GenerateLongAndNarrowFaceGrid(ids: str):
    """生成狭长面网格

    Args:
        ids: 网格面 ID,例如"5,6,7".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("GenerateLongAndNarrowFaceGrid", {"ids":ids})


def WingAnisoProcessWingAnisotropy(wing_chord_factor: float = 0.001,
                                   wing_te_chord_factor: float = 0.001,
                                   wing_root_factor: float = 0.001,
                                   wing_tip_factor: float = 0.001,
                                   middle_factor: float = 0.02,
                                   fuselage_middle_factor: float = 0.04,
                                   growth_rate: float = 1.2,
                                   control_layer: int = 50):
    """机翼各向异性处理主入口 — 依次执行全部 7 步子流程并汇总结果。

    包含：尺寸数据获取 → 翼段检测 → 交线识别 → 第一组平滑分布 → 前缘线平滑分布 → 各向异性线组创建 → 各向异性域生成。
    前置条件：表面网格已生成，分部件分组已完成（wingUpper/wingLower/wingTip/wingTrailingEdge/fuselage/engine 等分组已存在）。

    Args:
        wing_chord_factor: 机翼前缘侧尺寸系数（默认 0.001，即 0.1%×MAC；前缘端间距、前缘线组首层高度）。
        wing_te_chord_factor: 机翼后缘侧尺寸系数（默认 0.001，即 0.1%×MAC；后缘端间距、后缘线组首层高度）。
        wing_root_factor: 翼根尺寸系数（默认 0.001，即 0.1%×半展长；机翼-机身结合处、前缘线机身侧端）。
        wing_tip_factor: 翼梢尺寸系数（默认 0.001，即 0.1%×半展长；翼梢结合处、前缘线翼梢侧端）。
        middle_factor: MAC 分布中间值系数（默认 0.02，即 2%×MAC）。
        fuselage_middle_factor: 机身侧分布中间值系数（默认 0.04，即 4%×MAC）。
        growth_rate: 各向异性域增长率（默认 1.2）。
        control_layer: 各向异性域控制层数（默认 50）。

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","mac":<double>,"wing_segments":<int>,
         "group1_connector_ids":[...],"leading_edge_ids":[...],
         "trailing_edge_excluded_ids":[...],
         "group1_distribution":"success|failed",
         "leading_edge_distribution":"success|failed",
         "message":"..."}
    """
    return send_post_request("WingAnisoProcessWingAnisotropy",
                             {"wing_chord_factor":wing_chord_factor,
                              "wing_te_chord_factor":wing_te_chord_factor,
                              "wing_root_factor":wing_root_factor,
                              "wing_tip_factor":wing_tip_factor,
                              "middle_factor":middle_factor,
                              "fuselage_middle_factor":fuselage_middle_factor,
                              "growth_rate":growth_rate,
                              "control_layer":control_layer})


def NacelleAnisoProcessNacelleAnisotropy(nacelle_chord_factor: float = 0.0015,
                                         nacelle_te_chord_factor: float = 0.0015,
                                         pylon_joint_factor: float = 0.0015,
                                         span_factor: float = 0.001,
                                         middle_factor: float = 0.02,
                                         growth_rate: float = 1.2,
                                         control_layer: int = 50):
    """吊舱各向异性处理主入口 — 依次执行全部子步骤并汇总结果。

    包含：尺寸数据获取 → 网格线分类 → 9 类分布设置 → 支架前/后缘线分布 → 各向异性线组创建 → 各向异性域生成 → 狭长面处理。
    前置条件：表面网格已生成，发动机子部件分割已完成（engineInner/engineOuter/enginePylonInner/enginePylonOuter/enginePylon/engineTrailingEdge/enginePylonTrailingEdge 等分组已存在）。

    Args:
        nacelle_chord_factor: 吊舱/支架前缘、吊舱、支架后缘尺寸系数（默认 0.0015，即 0.15%×MAC）。
        nacelle_te_chord_factor: 吊舱后缘侧尺寸系数（默认 0.0015，即 0.15%×MAC；后缘端、TE 侧线组）。
        pylon_joint_factor: 支架与机翼/吊舱结合处尺寸系数（默认 0.0015，即 0.15%×MAC）。
        span_factor: 半展长尺寸系数（默认 0.001，即 0.1%×半展长；各向异性域首层）。
        middle_factor: MAC 分布中间值系数（默认 0.02，即 2%×MAC）。
        growth_rate: 各向异性域增长率（默认 1.2）。
        control_layer: 各向异性域控制层数（默认 50）。

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","mac":<double>,
         "steps":{"step1_dimensions":{...},"step2_classify":{...},...}}
    """
    return send_post_request("NacelleAnisoProcessNacelleAnisotropy",
                             {"nacelle_chord_factor":nacelle_chord_factor,
                              "nacelle_te_chord_factor":nacelle_te_chord_factor,
                              "pylon_joint_factor":pylon_joint_factor,
                              "span_factor":span_factor,
                              "middle_factor":middle_factor,
                              "growth_rate":growth_rate,
                              "control_layer":control_layer})


def TailAnisoProcessTailAnisotropy(local_chord_factor: float = 0.001,
                                   local_chord_middle_factor: float = 0.04,
                                   span_factor: float = 0.001,
                                   mac_factor: float = 0.001,
                                   middle_factor: float = 0.02,
                                   growth_rate: float = 1.2,
                                   control_layer: int = 50):
    """尾翼各向异性处理主入口 — 依次执行全部子步骤并汇总结果。

    包含：尺寸数据获取 → 尾翼线分类 → 分布设置 → 短边设点数 →
          各向异性线组创建 → 各向异性域生成 → 狭长面处理。
    前置条件：表面网格已生成，尾翼子部件分割已完成
    （verticalTailRight/verticalTailLeft/verticalTailTip/verticalTailTrailingEdge/
     horizontalTailUpper/horizontalTailLower/horizontalTailTip/horizontalTailTrailingEdge
     等分组已存在）。

    Args:
        local_chord_factor: 当地弦长尺寸系数（默认 0.001，即 0.1%×当地弦长）。
        local_chord_middle_factor: 当地弦长分布中间值系数（默认 0.04，即 4%×当地弦长）。
        span_factor: 半展长尺寸系数（默认 0.001，即 0.1%×半展长）。
        mac_factor: MAC 尺寸系数（默认 0.001，即 0.1%×MAC）。
        middle_factor: MAC 分布中间值系数（默认 0.02，即 2%×MAC）。
        growth_rate: 各向异性域增长率（默认 1.2）。
        control_layer: 各向异性域控制层数（默认 50）。

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","steps":{...}}
    """
    return send_post_request("TailAnisoProcessTailAnisotropy",
                             {"local_chord_factor":local_chord_factor,
                              "local_chord_middle_factor":local_chord_middle_factor,
                              "span_factor":span_factor,
                              "mac_factor":mac_factor,
                              "middle_factor":middle_factor,
                              "growth_rate":growth_rate,
                              "control_layer":control_layer})


def RemainingAnisoProcessRemainingAnisotropy(growth_ratio: float = 1.2):
    """其余面各向异性处理主入口。

    在机翼、吊舱、尾翼各向异性处理后，对剩余的所有网格面
    （机身、机尾、弹头等未做处理的网格面）进行各向异性处理。

    前置条件：机翼、吊舱、尾翼各向异性处理已完成。

    Args:
        growth_ratio: 各向异性域增长率（默认 1.2）。

    Returns:
        JSON 字符串，格式：
        {"status":"success|failed","domain_count":<int>}
    """
    return send_post_request("RemainingAnisoProcessRemainingAnisotropy",
                             {"growth_ratio":growth_ratio})

def GenerateANisoDomainGrid(anisoGroupsJson: str):
    """【底层原子工具】按给定参数生成各向异性域网格，支持多个各向异性组同时设置。

    与 WingAnisoProcessWingAnisotropy / NacelleAnisoProcessNacelleAnisotropy / TailAnisoProcessTailAnisotropy / RemainingAnisoProcessRemainingAnisotropy
    这四个一键流程工具不同，本工具是「通用设置原语」：
    - 不自动判断部件类型，由调用者指定网格线和参数
    - 不包含尺寸数据获取、线分类、分布计算等前置步骤
    - 适合局部精细控制、非标部件调整、或作为流程工具的内部步骤

    使用方式：先通过手动分析或流程工具确定各向异性组参数，再调用本工具一次性下发。

    Args:
        anisoGroupsJson: JSON 字符串，包含多个各向异性组的信息。
            外层为 JSON 数组，每个元素包含：
            - objects: 对象列表，每个对象含 domainID（网格面ID）和 connectorIDs（网格线ID数组）
            - firstHigh: 首层高度
            - growthRate: 增长率
            - controlLayer: 控制层数
            示例：[{"objects":[{"domainID":5,"connectorIDs":[6,7,8]}],"firstHigh":0.001,"growthRate":1.2,"controlLayer":40}]

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("GenerateANisoDomainGrid", {"anisoGroupsJson": anisoGroupsJson})
