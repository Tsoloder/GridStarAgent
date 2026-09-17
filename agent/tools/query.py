"""GridStar query 业务域工具。"""

from client import send_post_request


def GetStartAndEndPointByConnectors(connector_ids: list[int]):
    """批量获取网格线的起点和尾点坐标.

    传入多个网格线 ID，返回每个网格线的起终点坐标.

    Args:
        connector_ids: 网格线 ID 列表，例如 [1, 2, 3].

    Returns:
        返回 JSON 格式的查询结果,例如：
        {"connectors":[{"id":1,"start_point":{"pointID":"12","coordinates":[1,2,3,5,6,4]},
        "end_point":{"pointID":"21","coordinates":[1,2,3,5,6,4]}}, ...]}
    """
    return send_post_request("GetStartAndEndPointByConnectors", {"connector_ids":connector_ids})


def GetCurrentSelectedIDs(type: int):
    """根据对象类型获取当前选中的对象 ID.

    Args:
        type: 对象类型:
            1: 数模线
            2: 数模面
            3: 超边
            4: 超面
            5: 网格线
            6: 网格面
            7: 网格块

    Returns:
        返回 ID 的集合,例如{"info":[5,6,7]},如果为空则表示没有选择对象.
    """
    return send_post_request("GetCurrentSelectedIDs", {"type":type})


def GetCurrentSelectedIDsByTypes(types: list[int]):
    """根据多个对象类型获取当前选中的对象 ID.

    传入多个类型，返回每个类型对应的选中对象 ID 集合.

    Args:
        types: 对象类型列表，例如 [5, 6] 表示同时获取网格线和网格面的选中对象.
            1: 数模线  2: 数模面  3: 超边  4: 超面  5: 网格线  6: 网格面  7: 网格块

    Returns:
        返回 JSON 格式的查询结果,例如：
        {"results":[{"type":5,"ids":[5,6,7]},{"type":6,"ids":[8,9,10]}]}
    """
    return send_post_request("GetCurrentSelectedIDsByTypes", {"types":types})


def GetAllObjectByType(type: int):
    """根据类型获取所有的的对象ID.

    Args:
        type: 对象类型:
            1: 数模线
            2: 数模面
            3: 超边
            4: 超面
            5: 网格线
            6: 网格面
            7: 网格块

    Returns:
        返回 ID 的集合,例如{"info":[5,6,7]},如果为空则表示没有此类型的对象.
    """
    return send_post_request("GetAllObjectByType", {"type":type})


def GetAllObjectByTypes(types: list[int]):
    """根据多个类型获取所有的对象 ID.

    传入多个类型，返回每个类型对应的所有对象 ID 集合.

    Args:
        types: 对象类型列表，例如 [5, 6] 表示同时获取所有网格线和网格面的 ID.
            1: 数模线  2: 数模面  3: 超边  4: 超面  5: 网格线  6: 网格面  7: 网格块

    Returns:
        返回 JSON 格式的查询结果,例如：
        {"results":[{"type":5,"ids":[1,2,3]},{"type":6,"ids":[4,5,6]}]}
    """
    return send_post_request("GetAllObjectByTypes", {"types":types})


def GetScreenNormal():
    """获取当前屏幕的法向量.

    Returns:
        返回当前屏幕法向量,格式为{"normal":[0,5,6]}.
    """
    return send_post_request("GetScreenNormal", {})


def GetModelParameters():
    """获取模型全部关键几何参数：机身长度、翼根弦长、翼尖弦长、机翼半展长、特征长度、平均气动弦长(MAC).
    需先运行部件分割(SegmentPart)方能获取机身长度/翼根/翼尖/半展长等有效值，否则返回0.

    Returns:
        包含所有关键几何参数的JSON字符串.
    """
    import json
    raw = send_post_request("GetModelParameters", {})
    if raw.get("status") == "success":
        try:
            parsed = json.loads(raw["result"])
            return json.dumps(parsed, indent=2, ensure_ascii=False)
        except:
            return raw["result"]
    return raw


def GetDealWatertightTolenrance():
    """获取水密性处理公差.

    Returns:
        水密性处理的公差.
    """
    return send_post_request("GetDealWatertightTolenrance", {})


def GetAllSpitAssemblyGroupProperty():
    """获取所有分部件分组的属性.

    Returns:
        返回 JSON 字符串,示例如下:
        [{"jiyiqianyuan":{"line":[{"targetSize":12.34,"minSize":56.78,"angle":56.78,"ids":[1,2,3]}],
        "domain":[{"targetSize":6.7,"minSize":33.44,"angle":56.78,"ids":[1,2,3]}]}},
        {"jitou":{"line":[{"targetSize":12.34,"minSize":56.78,"angle":56.78,"ids":[1,2,3]}],
        "domain":[{"targetSize":6.7,"minSize":33.44,"angle":56.78,"ids":[1,2,3]}]}}].
        该 JSON 用于 MCP 工具配置,通过分部件组名称（如"jiyiqianyuan"、"jitou"）区分不同的分部件组,
        其中 line 表示线组,domain 表示面组,targetSize 为目标尺寸,minSize 为最小尺寸,
        angle 表示部件曲率自适应角度,ids 表示该分部件组下对象的 id 集合.
    """
    return send_post_request("GetAllSpitAssemblyGroupProperty", {})


def GetGenerateSurMeshDefaultParam():
    """获取表面网格生成默认参数.

    Returns:
        返回默认参数的 JSON 字符串,其格式为：{"targetSize": "2.55","minSize": "7.133","adaptAngle": "10"}。
        其中"targetSize"为全局目标尺寸,"minSize"为全局最小尺寸,"adaptAngle"为曲率自适应角度.
    """
    return send_post_request("GetGenerateSurMeshDefaultParam", {})


def GetCreateBlockDefaultParam():
    """获取体创建默认参数.

    Returns:
        返回默认参数的 JSON 字符串,其格式为：{"geoParam": "4,5,1,4","chooseParam": "0,0,0","centerCoor": "0,0,0","meshType", "0","meshSize": "22"}。
    """
    return send_post_request("GetCreateBlockDefaultParam", {})


def GetSpliteAssemlyDomainsBatch(group_names: list[str]):
    """批量获取指定分部件分组的网格面。

    传入多个分部件分组名称，返回每个分组名对应的网格面 ID 集合。

    Args:
        group_names: 分部件分组名称列表，例如 ["jiyiqianyuan", "jitou"]。

    Returns:
        返回 JSON 格式的查询结果,例如：
        {"groups":[{"groupName":"jiyiqianyuan","domain_ids":[4,31,55]},{"groupName":"jitou","domain_ids":[6,7,8]}]}
    """
    return send_post_request("GetSpliteAssemlyDomainsBatch", {"group_names":group_names})


def GetConnectorsByDomains(domain_ids: list[int]):
    """批量获取网格面下属的网格线集合.

    传入多个网格面 ID，返回每个面及其下属的网格线 ID 集合.

    Args:
        domain_ids: 网格面 ID 列表，例如 [1, 2, 3].

    Returns:
        返回 JSON 格式的查询结果，例如：
        {"domains":[{"domain_id":1,"connector_ids":[4,31,55]},{"domain_id":2,"connector_ids":[5,6,7]}]}
    """
    return send_post_request("GetConnectorsByDomains", {"domain_ids":domain_ids})


def GetConnectorsStartAndEndUnitLenth(connector_ids: list[int]):
    """批量获取网格线的首端间距和尾端间距.

    传入多个网格线 ID，返回每个网格线的首尾段间距.

    Args:
        connector_ids: 网格线 ID 列表，例如 [1, 2, 3].

    Returns:
        返回 JSON 格式的查询结果，例如：
        {"connectors":[{"id":1,"start":12.2,"end":55.4},{"id":2,"start":8.1,"end":33.2}]}
    """
    return send_post_request("GetConnectorsStartAndEndUnitLenth", {"connector_ids":connector_ids})


def GetPointCount(id: int):
    """获取网格线点数。

    Args:
        id: 网格线的 ID。

    Returns:
        JSON 字符串，格式：{"count": N}。
        失败时返回 "false"。
    """
    return send_post_request("GetPointCount", {"id": id})


def GetNewConnectorId():
    """获取网格线最新的ID

    Returns:
        返回 JSON 格式的结果,"id"代表最新的ID是多少
        {"id":12}
        失败时返回 "false"。
    """
    return send_post_request("GetNewConnectorId", {})


def GetDomainsByType(domain_type: int):
    """按类型筛选并获取所有网格面的 ID.

    Args:
        domain_type: 网格面类型:
            1: 物面 (OBJECT)
            3: 外场面 (OUTFILED)

    Returns:
        返回符合该类型的所有网格面 ID 集合,例如 {"info": [1, 2, 3]}.
    """
    return send_post_request("GetDomainsByType", {"domain_type": domain_type})


def GetRecentMessages(count: int):
    """获取最近的消息.

    Args:
        count: 获取的消息条数.

    Returns:
        返回 JSON 字符串,格式如下:
        {"messages":[{"time":"[2026-09-01,15:32:00] ","message":"xxx","level":1}, ...]}
        其中 level 含义: 0=debug, 1=info, 2=warning, 3=error, 4=fault.
        如果没有任何消息,返回 {"messages":[]}.
    """
    return send_post_request("GetRecentMessages", {"count":count})

