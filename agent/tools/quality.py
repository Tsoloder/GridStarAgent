"""GridStar quality 业务域工具。"""

from client import send_post_request


def ExamineConnector(connectorIDs: str, type: int):
    """网格线质量检查.

    Args:
        connectorIDs: 待检查的网格线 ID,可以为"0"或者"0,5,6".
        type: 检查类型,当前只能设置为 1.

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExamineConnector", {"connectorIDs":connectorIDs,"type":type})


def ExamineBlock(examType: str, ids: str):
    """网格块质量检查.

    此功能不需要选择网格块.

    Args:
        examType: 检查类型,默认值为 ExamineBlockNone。其参数有以下类型
            ExamineBlockNone
            ExamineVolume                                  //体积
            ExamineMaximumIncludeAngle                      //最大角
            ExamineMinimumIncludeAngle                      //最小角
            ExamineCentroidSkewness                         //质心扭值
            ExamineEquiangleSkewness                        //角度比例
            ExamineJacobian                                 //雅克比
            ExamineLengthI                                  //L方向长度
            ExamineLengthK                                  //L方向长度
            ExamineLengthL                                  //L方向长度
            ExamineLengthRatioI                             //K方向长度比
            ExamineLengthRatioK                             //K方向长度比
            ExamineLengthRatioL                             //L方向长度比
            ExamineCellCount"                               //单元计数
        ids: 选择的网格块 ID,可以为"0"或者"0,5,6"。如果没有选择网格块,则为"".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExamineBlock", {"examType":examType,"ids":ids})


def ExamineDomain(examType: str, ids: str):
    """网格面质量检查.

    Args:
        examType: 检查类型,默认值为 None。其参数有以下类型
            None,
			OverallSmoothness,				//整体光滑性
			Area,							//面积
			MaxmumAngle,					//最大角
			MinmumAngle,					//最小角
			AspectRatio,					//长宽比（纵横比）
			JLengthRatio,					//J疏密
			JSmoothness,					//J光滑
			KLengthRatio,					//K疏密
			KSmoothness,					//K光滑
			AreaRatio,						//面积比
			CellCount,						//单元计数
			WallSpacing,					//首层高度
			EdgeLength,						//边长
			EquiAreaSkewness,				//等面积偏斜
			EquiAngleSkewness,				//角度扭值
			Warp							//翘曲
        ids: 选择的网格面 ID,可以为"0"或者"0,5,6".

    Returns:
        工具调用结果,"true"代表成功,"false"代表失败.
    """
    return send_post_request("ExamineDomain", {"examType":examType,"ids":ids})

