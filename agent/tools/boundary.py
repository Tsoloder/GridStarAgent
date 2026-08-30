"""GridStar boundary 业务域工具。"""

from client import send_post_request


def BorderConditionAddGroup(name: str, groupID: int, colorNumber: int):
    """新建边界条件组.

    Args:
        name: 边界条件组名称.
        groupID: 边界条件组 ID.
        colorNumber: 边界条件组颜色编号,取值范围 0-5.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("BorderConditionAddGroup", {"name":name,"groupID":groupID,"colorNumber":colorNumber})


def BorderConditionDeleteGroup(name: str, groupID: int):
    """删除边界条件组.

    Args:
        name: 边界条件组名称.
        groupID: 边界条件组 ID.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("BorderConditionDeleteGroup", {"name":name,"groupID":groupID})


def BorderConditioConfigProperty(name: str, property: int, groupID: int, number: int):
    """设置边界条件组的属性.

    Args:
        name: 边界条件组名称.
        property: 边界条件组属性,-10 表示无边界条件,1 表示传值,2 表示对接,3 表示无粘固壁,
                4 表示粘性固壁,7 表示远场,8 表示对称,10 表示流入,11 表示流出,
                12 表示喷流入口,13 表示喷流出口.
        groupID: 边界条件组 ID.
        number: 修改的是第几个参数,1 表示 name,2 表示 property,3 表示 groupID.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("BorderConditioConfigProperty", {"name":name,"property":property,"groupID":groupID,"number":number})


def BorderConditionSaveDataToDomain(domainIDs: str, name: str, groupID: int, property: int):
    """将边界条件组的属性设置到网格面上.

    Args:
        domainIDs: 选择的网格面 ID,可以为"0"或者"0,5,6".
        name: 边界条件组名称.
        groupID: 边界条件组 ID.
        property: 边界条件组的属性,-10 表示无边界条件,1 表示传值,2 表示对接,3 表示无粘固壁,
                4 表示粘性固壁,7 表示远场,8 表示对称,10 表示流入,11 表示流出,
                12 表示喷流入口,13 表示喷流出口.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("BorderConditionSaveDataToDomain", {"domainIDs":domainIDs,"name":name,"groupID":groupID,"property":property})

