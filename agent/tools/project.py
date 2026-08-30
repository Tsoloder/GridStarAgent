"""GridStar project 业务域工具。"""

from client import send_post_request


def OpenSpdFile(filename: str):
    """打开工程文件.

    Args:
        filename: 要打开的工程文件路径.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("OpenSpdFile", {"filename":filename})


def SaveSpdFile(filename: str):
    """保存工程文件.

    Args:
        filename: 工程文件输出的路径.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("SaveSpdFile", {"filename":filename})


def SaveToAnotherSpdFile(filename: str):
    """另存工程文件.

    Args:
        filename: 工程文件输出的路径.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("SaveToAnotherSpdFile", {"filename":filename})


def ImportCADFile(filename: str, angle: float, targetSize: float, minSize: float, unit: int, append: int):
    """打开数模文件.

    Args:
        filename: 数模文件输入的路径.
        angle: 自适应角度,默认值10.
        targetSize: 目标尺寸,默认值0.05.
        minSize: 最小尺寸,默认值0.015.
        unit: 导入单位,默认值为 0。0 表示由文件定义,1 表示微米,2 表示毫米,3 表示厘米,4 表示米,5 表示千米,6 表示密耳,7 表示英尺,8 表示英里
        append: 是否追加,1 表示追加,0 表示覆盖.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ImportCADFile", {"append":append,"filename":filename,"angle":angle,"targetSize":targetSize,"minSize":minSize,"unit":unit})


def ImportGridFile(append: int, filename: str):
    """打开网格文件.

    Args:
        append: 是否追加,1 表示追加,0 表示覆盖.
        filename: 网格文件输入的路径.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ImportGridFile", {"append":append,"filename":filename})


def ExportGrid(outType: str, objType: str, name: str, outIDs: str, dataType: int, precision: int, unit: str):
    """导出网格文件.

    Args:
        outType: 导出的文件格式,支持"CGNS_3.21"、"CGNS_2.54"、"Gridgen"、"Plot3D".
                导出文件格式为"CGNS_3.21"或"CGNS_2.54"时,dataType、precision、unit 三个参数为默认值.
        objType: 导出的网格对象,"Connector"表示网格线,"Domain"表示网格面,"Block"表示网格块.
        name: 导出的文件名.
        outIDs: 导出的网格对象 id.
        dataType: 导出的数据格式,默认值为 0。0 表示二进制,1 表示十进制,2 表示无格式.
        precision: 导出数据精度类型,默认值为 1。0 表示单精度,1 表示双精度.
        unit: 导出单位类型,默认值为 0。0 表示不转换,1 表示米,2 表示毫米,3 表示英寸.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExportGrid", {"outType":outType,"objType":objType,"name":name,"outIDs":outIDs,"dataType":dataType,"precision":precision,"unit":unit})


def ClearData():
    """清除所有数据.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ClearData", {})


def ClearGridData():
    """清除网格数据.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ClearGridData", {})


def Undo():
    """撤销操作.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("Undo", {})


def Redo():
    """重做操作.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("Redo", {})


def ExportCADFile(filename: str, ids: str):
    """导出数模.

    Args:
        filename: 数模文件输出的路径.
        ids: 选择的数模面 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExportCADFile", {"filename":filename,"ids":ids})


def ExportSolver_CFX(name: str, outIDs: str, dataType: int, precision: int):
    """导出 CFX 结算器文件.

    Args:
        name: 导出的文件名.
        outIDs: 导出的网格对象 id.
        dataType: 导出的数据格式,默认值为 0。0 表示二进制,1 表示十进制,2 表示无格式.
        precision: 导出数据精度类型,默认值为 1。0 表示单精度,1 表示双精度.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExportSolver_CFX", {"name":name,"outIDs":outIDs,"dataType":dataType,"precision":precision})


def ExportSolver_FTS(name: str, outIDs: str, dataType: int, precision: int):
    """导出 FTS 结算器文件.

    Args:
        name: 导出的文件名.
        outIDs: 导出的网格对象 id,可以为"0"或者"0,5,6".
        dataType: 导出的数据格式,默认值为 0。0 表示二进制,1 表示十进制,2 表示无格式.
        precision: 导出数据精度类型,默认值为 1。0 表示单精度,1 表示双精度.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExportSolver_FTS", {"name":name,"outIDs":outIDs,"dataType":dataType,"precision":precision})


def ExportSolver(outType: str, objType: str, name: str, outIDs: str):
    """导出结算器文件.

    Args:
        outType: 导出的文件格式,支持的类型为"CGNS_3.21"、"CGNS_2.54"、"CGNS_Hybrid"、"PMB3D"、"CFD++"、"FluentMesh".
        name: 导出的文件名.
        outIDs: 导出的网格对象 id,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExportSolver", {"outType":outType,"name":name,"outIDs":outIDs})

