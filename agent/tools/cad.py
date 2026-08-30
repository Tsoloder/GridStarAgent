"""GridStar cad 业务域工具。"""

from client import send_post_request


def UGCADSurfaceTranslate(surfaceIDs: str, startPoint: str, lastPoint: str, isCopy: int):
    """超面平移.

    Args:
        surfaceIDs: 选择的超面 ID,可以为"0"或者"0,5,6".
        startPoint: 当前移动的首点坐标,例如 [1,5,7].
        lastPoint: 当前移动的尾点坐标,例如 [1,5,7].
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGCADSurfaceTranslate", {"surfaceIDs":surfaceIDs,"startPoint":startPoint,"lastPoint":lastPoint,"isCopy":isCopy})


def UGCADSurfaceRotate(surfaceIDs: str, startPoint: str, endpoint: str, rotate_angle: float, rotate_times: int, isCopy: int):
    """超面旋转.

    Args:
        surfaceIDs: 选择的超面 ID,可以为"0"或者"0,5,6".
        startPoint: 轴起点坐标,例如 [1,5,7].
        endpoint: 轴尾点坐标,例如 [1,5,7].
        rotate_angle: 旋转角度.
        rotate_times: 旋转次数.
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGCADSurfaceRotate", {"surfaceIDs":surfaceIDs,"startPoint":startPoint,"endpoint":endpoint,"rotate_angle":rotate_angle,"rotate_times":rotate_times,"isCopy":isCopy})


def UGCADSurfaceScale(surfaceIDs: str, zoomCenter: str, isCopy: int):
    """超面缩放.

    Args:
        surfaceIDs: 选择的超面 ID,可以为"0"或者"0,5,6".
        zoomCenter: 缩放中心坐标,例如 [1,5,7].
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGCADSurfaceScale", {"surfaceIDs":surfaceIDs,"zoomCenter":zoomCenter,"isCopy":isCopy})


def UGCADSurfaceMirror(surfaceIDs: str, useSymmetry: int, isCopy: int):
    """超面镜像.

    Args:
        surfaceIDs: 选择的超面 ID,可以为"0"或者"0,5,6".
        useSymmetry: 对称面,默认值为 0。0 表示 XY 面,1 表示 ZX 面,2 表示 ZY 面.
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGCADSurfaceMirror", {"surfaceIDs":surfaceIDs,"useSymmetry":useSymmetry,"isCopy":isCopy})


def UGSurfaceProcessing(edgeIDs: str, type: int, tolerance: float, minLenth: float):
    """碎面处理.

    Args:
        edgeIDs: 选择的超边 ID,可以为"0"或者"0,5,6".
        type: 当前操作的类型,-1 表示合并,2 表示打散,3 表示删除.
        tolerance: 面积比.
        minLenth: 最小边长.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSurfaceProcessing", {"edgeIDs":edgeIDs,"type":type,"tolerance":tolerance,"minLenth":minLenth})


def UGDamageRepari(edgeIDs: str, repairPattern: int, fillStyle: float):
    """碎面修复.

    Args:
        edgeIDs: 选择的超边 ID,可以为"0"或者"0,5,6".
        repairPattern: 修复模式,-1 表示合并,2 表示打散,3 表示删除.
        fillStyle: 填充方式.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGDamageRepari", {"edgeIDs":edgeIDs,"repairPattern":repairPattern,"fillStyle":fillStyle})


def DeleteFC(ids: str, flag: int):
    """删除数模线.

    Args:
        ids: 选择的数模线 ID,可以为"0"或者"0,5,6".
        flag: 是否删除关联的数模面,默认值为 0。0 表示不删除,1 表示删除.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("DeleteFC", {"ids":ids,"flag":flag})


def DeleteNbsFace(ids: str, flag: int):
    """删除数模面.

    Args:
        ids: 选择的数模面 ID,可以为"0"或者"0,5,6".
        flag: 是否删除关联的数模线,默认值为 0。0 表示不删除,1 表示删除.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("DeleteNbsFace", {"ids":ids,"flag":flag})


def UGSpitAssemblyCreateNewGroup(groupName: str, strDiagon: float, strDiagonMin: float, strAngle: float):
    """创建新的分部件组.

    Args:
        groupName: 分部件组名.
        strDiagon: 分部件组目标尺寸.
        strDiagonMin: 分部件组最小尺寸.
        strAngle: 分部件组曲率自适应角度.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSpitAssemblyCreateNewGroup", {"groupName":groupName,"strDiagon":strDiagon,"strDiagonMin":strDiagonMin,"strAngle":strAngle})


def UGSpitAssemblyMoveNodesToNewGroup(selectType: str, groupName: str, ids: str):
    """将选择的超边或者超面分配到指定分部件组中.

    Args:
        selectType: 选择的对象类型,0 表示超边,1 表示超面.
        groupName: 目标分部件组名.
        ids: 选择的对象 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSpitAssemblyMoveNodesToNewGroup", {"selectType":selectType,"groupName":groupName,"ids":ids})


def UGSpitAssemblyRenameGroup(preName: str, newName: str):
    """重命名分部件组.

    Args:
        preName: 旧的分部件组名称.
        newName: 新的分部件组名称.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSpitAssemblyRenameGroup", {"preName":preName,"newName":newName})


def UGSpitAssemblyDeleteGroup(groupName: str):
    """删除分部件组.

    Args:
        groupName: 分部件组名称.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGSpitAssemblyDeleteGroup", {"groupName":groupName})


def TranslateSurface(faceIDs: str, isCopy: str, startPoint: str, endPoint: str):
    """数模面平移.

    Args:
        faceIDs: 选择的数模面 ID,可以为"0"或者"0,5,6".
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.
        startPoint: 轴起点坐标,例如 [1,5,7].
        endPoint: 轴尾点坐标,例如 [1,5,7].

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("TranslateSurface", {"faceIDs":faceIDs,"isCopy":isCopy,"startPoint":startPoint,"endPoint":endPoint})


def UGDelRedundantDom(selectedID: str, domType: int, precisio: float, overlapRatio: float):
    """删除多余面（重面）.

    Args:
        selectedID: 选择的超面 ID,可以为"0"或者"0,5,6".
        domType: 面的类型,1 表示重面,2 表示 topo 错误面,3 表示内部面.
        precisio: 重面检测距离值.
        overlapRatio: 重面检测比例值.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGDelRedundantDom", {"selectedID":selectedID,"domType":domType,"precisio":precisio,"overlapRatio":overlapRatio})


def UGRepairRedundantDom(selectedID: str, domType: int, precisio: float, overlapRatio: float):
    """修复多余面（重面）.

    Args:
        selectedID: 选择的超面 ID,可以为"0"或者"0,5,6".
        domType: 面的类型,1 表示重面,2 表示 topo 错误面,3 表示内部面.
        precisio: 重面检测距离值.
        overlapRatio: 重面检测比例值.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGRepairRedundantDom", {"selectedID":selectedID,"domType":domType,"precisio":precisio,"overlapRatio":overlapRatio})


def CADIntersect(idsA: str, idsB: str):
    """提取交线.

    Args:
        idsA: 选择的数模面 ID,可以为"0"或者"0,5,6".
        idsB: 选择的数模面 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("CADIntersect", {"idsA":idsA,"idsB":idsB})


def UGDeleteSubassembly(ids: str):
    """组件删除.

    Args:
        ids: 选择的超面 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("UGDeleteSubassembly", {"ids":ids})


def CreateCoons(ids: str):
    """创建双曲性曲面.

    Args:
        ids: 选择的超边 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("CreateCoons", {"ids":ids})


def ManualExtractConnector(ids: str, precision: float):
    """手动提取边界线.

    Args:
        ids: 选择的超边 ID,可以为"0"或者"0,5,6".
        precision: 合并精度.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ManualExtractConnector", {"ids":ids,"precision":precision})


def AutoExtractConnector(ids: str):
    """自动提取边界线.

    Args:
        ids: 选择的超边 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("AutoExtractConnector", {"ids":ids})


def CADSurfaceScale(surfaceIDs: str, isCopy: int, xyz: str):
    """数模面缩放.

    Args:
        surfaceIDs: 选择的数模面 ID,可以为"0"或者"0,5,6".
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.
        xyz: 当前缩放尺寸,表示 x、y、z 三个轴方向的缩放比例,可以为 [0.1,0.5,2.3].

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("CADSurfaceScale", {"surfaceIDs":surfaceIDs,"isCopy":isCopy,"xyz":xyz})


def CADSurfaceRotate(faceIDs: str, isCopy: int, startPoint: str, endpoint: str, rotate_angle: float, rotate_times: int):
    """数模面旋转.

    Args:
        faceIDs: 选择的超面 ID,可以为"0"或者"0,5,6".
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.
        startPoint: 轴起点坐标,例如 [1,5,7].
        endpoint: 轴尾点坐标,例如 [1,5,7].
        rotate_angle: 旋转角度.
        rotate_times: 旋转次数.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("CADSurfaceRotate", {"faceIDs":faceIDs,"isCopy":isCopy,"startPoint":startPoint,"endpoint":endpoint,"rotate_angle":rotate_angle,"rotate_times":rotate_times})


def CADSurfaceMirror(faceIDs: str, isCopy: int, coords: str):
    """数模面镜像.

    Args:
        faceIDs: 选择的数模面 ID,可以为"0"或者"0,5,6".
        isCopy: 是否复制,默认值为 0。0 表示不复制,1 表示复制.
        coords: 确定平面的三个坐标,其形式为 [1,2,3,5,6,4,2,6,2]。其中每三个数字代表一个点的 X、Y、Z 坐标.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("CADSurfaceMirror", {"faceIDs":faceIDs,"isCopy":isCopy,"coords":coords})


def DealWatertight(tolenrance: float):
    """水密性处理，默认处理所有超边。处理完成后返回剩余自由边信息。

    Args:
        tolenrance: 公差。用户未给出公差时，需要通过 GetDealWatertightTolenrance 工具获取公差。

    Returns:
        JSON字符串，包含：status(成功/失败), free_edge_count(自由边数),
        free_edge_pairs_in_tol(公差内可合并对数), has_free_edges(是否存在自由边).
    """
    return send_post_request("DealWatertight", {"tolenrance":tolenrance})

