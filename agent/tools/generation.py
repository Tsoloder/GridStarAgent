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
        generateWay: 生成方法,0 表示构造法,1 表示层推法.
        ids: 选择的网格块 ID,例如"5,6,7"
        layer: 附面层层数,默认值为40.
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


def GenerateANisoDomainGrid(anisoGroupsJson: str):
    """生成各向异性网格，支持多个各向异性组同时设置。

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

