"""MCP 工具注册表。

工具按业务域分组（TOOL_GROUPS），供客户端按需启用，避免每轮请求都携带全部工具 schema。
"""

import json

from tools import project
from tools import query
from tools import cad
from tools import mesh
from tools import generation
from tools import boundary
from tools import quality
from tools import advanced

# 分组 id 需保持稳定：客户端会缓存并按 id 启用分组。
TOOL_GROUPS = (
    {
        "id": "project",
        "description": "工程文件管理：打开/保存/另存 SPD、导入导出 CAD 与网格文件、清空数据、撤销重做、导出求解器格式（CFX/FTS）。",
        "tools": (
            project.OpenSpdFile,
            project.SaveSpdFile,
            project.SaveToAnotherSpdFile,
            project.ImportCADFile,
            project.ImportGridFile,
            project.ExportGrid,
            project.ExportCADFile,
            project.ExportSolver_CFX,
            project.ExportSolver_FTS,
            project.ExportSolver,
            project.ClearData,
            project.ClearGridData,
            project.Undo,
            project.Redo,
        ),
    },
    {
        "id": "query",
        "description": "只读查询：获取对象 ID、网格线起终点与点数、当前选中对象、屏幕法向、模型几何参数、默认参数、分部件分组属性、网格面类型筛选、最近消息等 GridStar 当前状态。",
        "tools": (
            query.GetStartAndEndPointByConnector,
            query.GetCurrentSelectedIDs,
            query.GetAllObjectByType,
            query.GetScreenNormal,
            query.GetModelParameters,
            query.GetDealWatertightTolenrance,
            query.GetAllSpitAssemblyGroupProperty,
            query.GetGenerateSurMeshDefaultParam,
            query.GetCreateBlockDefaultParam,
            query.GetSpliteAssemlyDomains,
            query.GetConnectorsByDomain,
            query.GetConnectorStartAndEndUnitLenth,
            query.GetPointCount,
            query.GetNewConnectorId,
            query.GetDomainsByType,
        ),
    },
    {
        "id": "cad",
        "description": "CAD 几何处理：曲面平移/旋转/缩放/镜像、曲面加工与损伤修复、水密处理、求交、手动与自动提线、Coons 曲面、分部件分组增删改查与节点移动。",
        "tools": (
            cad.UGCADSurfaceTranslate,
            cad.UGCADSurfaceRotate,
            cad.UGCADSurfaceScale,
            cad.UGCADSurfaceMirror,
            cad.UGSurfaceProcessing,
            cad.UGDamageRepari,
            cad.DeleteFC,
            cad.DeleteNbsFace,
            cad.UGSpitAssemblyCreateNewGroup,
            cad.UGSpitAssemblyMoveNodesToNewGroup,
            cad.UGSpitAssemblyRenameGroup,
            cad.UGSpitAssemblyDeleteGroup,
            cad.TranslateSurface,
            cad.UGDelRedundantDom,
            cad.UGRepairRedundantDom,
            cad.CADIntersect,
            cad.UGDeleteSubassembly,
            cad.CreateCoons,
            cad.ManualExtractConnector,
            cad.AutoExtractConnector,
            cad.CADSurfaceScale,
            cad.CADSurfaceRotate,
            cad.CADSurfaceMirror,
            cad.DealWatertight,
        ),
    },
    {
        "id": "mesh",
        "description": "网格拓扑编辑：网格线/网格面/网格块的删除、平移、旋转、镜像、缩放、拼接、装配、分裂，端点移动，点数设置与复制，间距分布（均匀/平滑/自定义/反转/平均/匹配）控制。",
        "tools": (
            mesh.DeleteConnector,
            mesh.DeleteDomain,
            mesh.DeleteBlock,
            mesh.TranslateMesh,
            mesh.UGZoomMesh,
            mesh.UGMirrorSur,
            mesh.UGRotateSurf,
            mesh.JoinConnector,
            mesh.UGJoinDomain,
            mesh.UGHalfModelLine,
            mesh.MoveEndPoint,
            mesh.AssembleDomain,
            mesh.UGAssembleDomain,
            mesh.UGInitialDomain,
            mesh.UGReDimensionSetSpecifiedValue,
            mesh.UGReDimensionCopy,
            mesh.UGReDimensionMatch,
            mesh.UGReDimensionAverageDistribution,
            mesh.UGReDimensionInversionDistribution,
            mesh.UGReDimensionConfigDistribution,
            mesh.UGReDimensionSmoothDistribution,
            mesh.UGSplitConnector,
            mesh.TranslateConnector,
            mesh.CreateConnector,
            mesh.TSplitConnector,
            mesh.SplitBlock,
            mesh.SetConnectorPointCount,
            mesh.CopyConnectorPointCount,
            mesh.SetConnectorAverageDistribution,
            mesh.SetConnectorSmoothDistribution,
            mesh.SetConnectorConfigDistribution,
            mesh.AssembleConnectorsToDomain,
            mesh.MergeEdgesByDomain,
        ),
    },
    {
        "id": "generation",
        "description": "网格生成：表面网格（含按分部件分组属性生成）、体网格/块创建、长窄面网格、各向异性域网格。",
        "tools": (
            generation.UGSur,
            generation.UGBlockCreate,
            generation.UGUGSp,
            generation.GenerateSurMeshBySpitAssemblyGroupProperty,
            generation.GenerateLongAndNarrowFaceGrid,
            generation.GenerateANisoDomainGrid,
        ),
    },
    {
        "id": "boundary",
        "description": "边界条件：边界条件分组的添加与删除、属性配置、保存到计算域。",
        "tools": (
            boundary.BorderConditionAddGroup,
            boundary.BorderConditionDeleteGroup,
            boundary.BorderConditioConfigProperty,
            boundary.BorderConditionSaveDataToDomain,
        ),
    },
    {
        "id": "quality",
        "description": "网格质量检查：网格线、网格面、网格块的质量检查与评估。",
        "tools": (
            quality.ExamineConnector,
            quality.ExamineBlock,
            quality.ExamineDomain,
        ),
    },
    {
        "id": "advanced",
        "description": "高级批处理：调用服务端组合流程（ProcessWithServer）、尾缘面分类、类型1方向判定、类型2角色识别、前缘识别。",
        "tools": (
            advanced.ProcessWithServer,
            advanced.ClassifyTrailingEdgeDomains,
            advanced.DetermineDirectionForType1,
            advanced.IdentifyType2Roles,
            advanced.IdentifyLeadingEdge,
        ),
    },
)

TOOLS = tuple(tool for group in TOOL_GROUPS for tool in group["tools"])

# 默认始终暴露的分组：只读查询与工程文件管理，其余分组按需启用。
DEFAULT_GROUP_IDS = ("query", "project")


def GetToolGroups():
    """获取 GridStar 工具分组目录（工具发现入口）。

    本工具不连接 GridStar，只返回服务端注册表的元数据，用于了解有哪些工具分组、
    每组包含哪些工具。默认仅暴露 query（只读查询）与 project（工程文件管理）两个分组的工具，
    其他分组的工具需要先通过 enable_tool_group 启用后才能调用。

    Returns:
        JSON 字符串，格式如下：
        {"groups":[{"id":"query","description":"只读查询...","tools":["GetPointCount", ...]}, ...],
         "default_enabled":["query","project"]}
        其中 id 为分组标识（enable_tool_group 的入参），description 说明该分组能做什么，
        tools 为该分组下的工具名列表。
    """
    payload = {
        "groups": [
            {
                "id": group["id"],
                "description": group["description"],
                "tools": [tool.__name__ for tool in group["tools"]],
            }
            for group in TOOL_GROUPS
        ],
        "default_enabled": list(DEFAULT_GROUP_IDS),
    }
    return json.dumps(payload, ensure_ascii=False)


def group_id_for_tool(tool_name: str) -> str | None:
    """按工具名反查所属分组 id，未找到返回 None。"""
    for group in TOOL_GROUPS:
        if any(tool.__name__ == tool_name for tool in group["tools"]):
            return group["id"]
    return None


def register_tools(mcp):
    """注册全部 GridStar 工具及工具发现入口。"""
    for tool in TOOLS:
        mcp.tool()(tool)
    mcp.tool()(GetToolGroups)
    return mcp
