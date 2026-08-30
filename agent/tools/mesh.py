"""GridStar mesh 业务域工具。"""

from client import send_post_request


def DeleteConnector(conIDs: str):
    """删除网格线.

    Args:
        conIDs: 选择的网格线 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("DeleteConnector", {"conIDs":conIDs})


def DeleteDomain(domainIDs: str, isDeleteConnector: int):
    """删除网格面.

    Args:
        domainIDs: 选择的网格面 ID,可以为"0"或者"0,5,6".
        isDeleteConnector: 是否删除关联的网格线,默认值为 0。0 表示不删除,1 表示删除.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("DeleteDomain", {"domainIDs":domainIDs,"isDeleteConnector":isDeleteConnector})


def DeleteBlock(blockIDs: str, isDeleteDomain: int, isDeleteConnector: int):
    """删除网格块.

    Args:
        blockIDs: 选择的网格块 ID,可以为"0"或者"0,5,6".
        isDeleteDomain: 是否删除所属网格面,默认值为 0。0 表示不删除,1 表示删除.
        isDeleteConnector: 是否删除关联的网格面所属的网格线,默认值为 0。0 表示不删除,1 表示删除.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("DeleteBlock", {"blockIDs":blockIDs,"isDeleteDomain":isDeleteDomain,"isDeleteConnector":isDeleteConnector})


def TranslateMesh(isBlock: str, domainIDs: str, isCopy: str, isRebuild: str, startPoint: str, endPoint: str):
    """网格平移.

    Args:
        isBlock: 是否操作网格块标志,默认值 0。0 表示不是,1 表示是.
        domainIDs: 选择的网格面 ID,可以为"0"或者"0,5,6".
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.
        isRebuild: 是否重建,默认值为 0。0 表示不重建,1 表示重建.
        startPoint: 轴起点坐标,例如 [1,5,7].
        endPoint: 轴尾点坐标,例如 [1,5,7].

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("TranslateMesh", {"isBlock":isBlock,"domainIDs":domainIDs,"isCopy":isCopy,"isRebuild":isRebuild,"startPoint":startPoint,"endPoint":endPoint})


def UGZoomMesh(domIDs: str, dZoomMeshParam: int, isCopy: int, selectType: int):
    """网格面缩放.

    Args:
        domIDs: 选择的网格面 ID,可以为"0"或者"0,5,6".
        dZoomMeshParam: 当前缩放尺寸,表示 x、y、z 三个轴方向的缩放比例,可以为 [0.1,0.5,2.3].
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.
        selectType: 当前操作的对象类型,1 表示网格面,2 表示网格块.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGZoomMesh", {"domIDs":domIDs,"dZoomMeshParam":dZoomMeshParam,"isCopy":isCopy,"selectType":selectType})


def UGMirrorSur(selectType: int, domIDs: str, useSymmetry: int, symmetry: int, coords: str, isCopy: int):
    """网格面镜像.

    Args:
        selectType: 当前操作的对象类型,2 表示网格面,3 表示网格块.
        domIDs: 选择的网格面 ID,可以为"0"或者"0,5,6".
        useSymmetry: 是否是对称面操作,默认值为 0。0 表示不是,1 表示是.
        symmetry: 对称面的类型,0 表示 XY 面,1 表示 ZX 面,2 表示 ZY 面.
        coords: 确定平面的三个坐标,其形式为 [1,2,3,5,6,4,2,6,2]。其中每三个数字代表一个点的 X、Y、Z 坐标.
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGMirrorSur", {"selectType":selectType,"domIDs":domIDs,"useSymmetry":useSymmetry,"symmetry":symmetry,"coords":coords,"isCopy":isCopy})


def UGRotateSurf(selectType: int, domIDs: str, startPoint: str, endPoint: str, angle: float, times: int, isCopy: int):
    """网格面旋转.

    Args:
        selectType: 当前操作的对象类型,1 表示网格面,2 表示网格块.
        domIDs: 选择的网格面 ID,可以为"0"或者"0,5,6".
        startPoint: 轴起点坐标,例如 [1,5,7].
        endPoint: 轴尾点坐标,例如 [1,5,7].
        angle: 旋转角度,比如需要旋转的角度为 num 度,则此参数为 (num/180)*3.1415926.
        times: 旋转次数.
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGRotateSurf", {"selectType":selectType,"domIDs":domIDs,"startPoint":startPoint,"endPoint":endPoint,"angle":angle,"times":times,"isCopy":isCopy})


def UGJoinConnector(ids: str):
    """网格线合并.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGJoinConnector", {"cnIDs":ids})


def UGJoinDomain(ids: str):
    """网格面合并.

    Args:
        ids: 选择的网格面 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGJoinDomain", {"domIDs":ids})


def UGHalfModelLine(cnIDs: str, symmetry: int):
    """设置半模边界线.

    Args:
        cnIDs: 选择的网格线 ID,可以为"0"或者"0,5,6".
        symmetry: 对称面,默认值为 0。0 表示 ZY 面,1 表示 ZX 面,2 表示 XY 面.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGHalfModelLine", {"cnIDs":cnIDs,"symmetry":symmetry})


def MoveEndPoint(pointId: str, normal: str, position: str):
    """移动端点.

    Args:
        pointId: 选择的网格点的 ID.
        normal: 当前屏幕的法向量.
        position: 移动后的端点坐标,可以为 [0.1,0.5,2.3].

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("MoveEndPoint", {"pointId":pointId,"normal":normal,"position":position})


def AssembleDomain(ids: str):
    """结构网格面自动装配.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("AssembleDomain", {"ids":ids})


def UGAssembleDomain(ids: str):
    """非结构网格面手动装配.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6",网格线要按连接顺序（首尾点相连）排布.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGAssembleDomain", {"ids":ids})


def UGInitialDomain(ids: str):
    """网格面初始化.

    Args:
        ids: 选择的网格面 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGInitialDomain", {"ids":ids})


def UGReDimensionSetSpecifiedValue(ids: str, number: int):
    """点数分布：设置网格线点数为指定值.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        number: 网格点数.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionSetSpecifiedValue", {"ids":ids,"number":number})


def UGReDimensionCopy(ids: str, targetId: int):
    """点数分布：拷贝网格点数.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        targetId: 被拷贝的网格线 ID.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionCopy", {"ids":ids,"targetId":targetId})


def UGReDimensionMatch(ids: str, targetId: int):
    """点数分布：匹配网格点数.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        targetId: 目标网格线 ID.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionMatch", {"ids":ids,"targetId":targetId})


def UGReDimensionAverageDistribution(ids: str):
    """点数分布：平均分布.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionAverageDistribution", {"ids":ids})


def UGReDimensionInversionDistribution(ids: str):
    """点数分布：分布反向.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionInversionDistribution", {"ids":ids})


def UGReDimensionConfigDistribution(ids: str, headspace: float, tailspace: float, params:str, mindValue: float):
    """点数分布：设置增长分布.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        headspace: 首端间距.
        tailspace: 尾端间距.
        params: 分布参数配置,严格格式为 "headRate,headLayer,tailRate,tailLayer"。
                      每项为浮点数,例如："1.5,3,2.0,4"。
                      headRate/headLayer: 首端增长率与层数.
                      tailRate/tailLayer: 尾端增长率与层数.
                      不支持空格、中文逗号或科学计数法.
        mindValue: 中间值.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionConfigDistribution", {"ids":ids,"headspace":headspace,"tailspace":tailspace,"params":params,"mindValue":mindValue})


def UGReDimensionSmoothDistribution(ids: str, headspace: float, tailspace: float, params:str, mindValue: float):
    """点数分布：平滑分布.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        headspace: 首端间距.
        tailspace: 尾端间距.
        params: 分布参数配置,严格格式为 "headRate,headLayer,tailRate,tailLayer"。
                      每项为浮点数,例如："1.5,3,2.0,4"。
                      headRate/headLayer: 首端增长率与层数.
                      tailRate/tailLayer: 尾端增长率与层数.
                      不支持空格、中文逗号或科学计数法.
        mindValue: 中间值.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGReDimensionSmoothDistribution", {"ids":ids,"headspace":headspace,"tailspace":tailspace,"params":params,"mindValue":mindValue})


def UGSplitConnector(ids: str, splitWay: int, value: float):
    """网格线分割.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        splitWay: 切分方式,0 表示网格点,1 表示任意点.
        value: 当 splitWay 的值为 0 时,此参数表示分割点的位置；为 1 时表示切分值.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSplitConnector", {"ids":ids,"splitWay":splitWay,"value":value})


def TranslateConnector(ids: str, startPoint: str, endPoint: str, isCopy: str):
    """网格线平移.

    Args:
        ids: 选择的网格线 ID,可以为"0"或者"0,5,6".
        startPoint: 轴起点坐标,例如 [1,5,7].
        endPoint: 轴尾点坐标,例如 [1,5,7].
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("TranslateConnector", {"ids":ids,"startPoint":startPoint,"endPoint":endPoint,"isCopy":isCopy})


def CreateConnector(startPoint: str, endPoint: str):
    """创建网格线.

    Args:
        startPoint: 轴起点坐标,例如 [1,5,7].
        endPoint: 轴尾点坐标,例如 [1,5,7].

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("CreateConnector", {"startPoint":startPoint,"endPoint":endPoint})


def TSplitConnector(ids: str):
    """T 型线分割.

    Args:
        ids: 分割的网格线 id,可以为"0"或者"0,5"。此参数中只能含有两个元素,
            第一个元素是被分割的网格线 id,后一个元素是分割线 id.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("TSplitConnector", {"ids":ids})


def SplitBlock(ids: str, direction: int, splitValue: str):
    """网格块分割.

    Args:
        ids: 选择的网格块 ID,可以为"0"或者"0,5,6".
        direction: 分割方向,1 表示 J 方向,2 表示 K 方向,3 表示 L 方向.
        splitValue: 分割点的位置,其形式为 [1,2,3,5,6].

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("SplitBlock", {"ids":ids,"direction":direction,"splitValue":splitValue})


def SetConnectorPointCount(ids: str, number: int):
    """设置网格线点数。

    Args:
        ids: 网格线 ID,逗号分隔,如 "12,13".
        number: 点数.

    Returns:
        JSON 字符串，格式：{"success":true,"new_id":136}。
        success 为 true 表示成功，new_id 为该操作创建的新网格线 ID；
        失败时返回 {"success":false,"new_id":-1}。
    """
    return send_post_request("SetConnectorPointCount", {"ids": ids, "number": number})


def CopyConnectorPointCount(sourceId: str, targetIds: str):
    """拷贝网格线点数（从源网格线拷贝到目标网格线组）。

    ⚠️ sourceId 是点数来源(拷贝源),targetIds 是接收点数的目标组,切勿搞反。

    Args:
        sourceId: 点数来源的网格线 ID(拷贝源).
        targetIds: 目标网格线 ID 列表,逗号分隔,如 "12,13".

    Returns:
        JSON 字符串，格式：{"success":true,"new_id":136}。
        success 为 true 表示成功，new_id 为该操作创建的新网格线 ID；
        失败时返回 {"success":false,"new_id":-1}。
    """
    return send_post_request("CopyConnectorPointCount", {"sourceId": sourceId, "targetIds": targetIds})


def SetConnectorAverageDistribution(ids: str):
    """平均分布网格线。

    Args:
        ids: 网格线 ID,逗号分隔,如 "12,13".

    Returns:
        JSON 字符串，格式：{"success":true,"new_id":136}。
        success 为 true 表示成功，new_id 为该操作创建的新网格线 ID；
        失败时返回 {"success":false,"new_id":-1}。
    """
    return send_post_request("SetConnectorAverageDistribution", {"ids": ids})


def SetConnectorSmoothDistribution(ids: str, headspace: float, tailspace: float, params: str, mindValue: float):
    """平滑分布网格线。

    Args:
        ids: 网格线 ID,逗号分隔,如 "12,13".
        headspace: 首端间距.
        tailspace: 尾端间距.
        params: 分布参数,格式 "headRate,headLayer,tailRate,tailLayer",如 "1.2,10,1.2,10".
        mindValue: 中间值.

    Returns:
        JSON 字符串，格式：{"success":true,"new_id":136}。
        success 为 true 表示成功，new_id 为该操作创建的新网格线 ID；
        失败时返回 {"success":false,"new_id":-1}。
    """
    return send_post_request("SetConnectorSmoothDistribution", {
        "ids": ids, "headspace": headspace, "tailspace": tailspace,
        "params": params, "mindValue": mindValue
    })


def SetConnectorConfigDistribution(ids: str, headspace: float, tailspace: float, params: str, mindValue: float):
    """配置分布网格线。

    Args:
        ids: 网格线 ID,逗号分隔,如 "12,13".
        headspace: 首端间距.
        tailspace: 尾端间距.
        params: 分布参数,格式 "headRate,headLayer,tailRate,tailLayer",如 "1.2,10,1.2,10".
        mindValue: 中间值.

    Returns:
        "true" 代表成功,"false" 代表失败.
    """
    return send_post_request("SetConnectorConfigDistribution", {
        "ids": ids, "headspace": headspace, "tailspace": tailspace,
        "params": params, "mindValue": mindValue
    })


def AssembleConnectorsToDomain(ids: str):
    """装配结构网格面。

    将指定的网格线集合装配为一个新的结构网格面。

    Args:
        ids: 网格线 ID 列表,逗号分隔,如 "12,13,14,15".

    Returns:
        "true" 代表成功,"false" 代表失败.
    """
    return send_post_request("AssembleConnectorsToDomain", {"ids": ids})


def MergeEdgesByDomain(id: int):
    """处理"后缘面"分部件分组的碎边,输出两个长网格线和两个短网格线

    Args:
        id: 网格面的ID。

    Returns:
        返回 JSON 格式的查询结果,"longids"代表两个长网格线的ID,"shortids"代表两个短网格线的ID,例如：
        {"longids":[4,31],"shortids":[55,22]}
        失败时返回 "false"。
    """
    return send_post_request("MergeEdgesByDomain", {"id":id})

