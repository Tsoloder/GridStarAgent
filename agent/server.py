try:
    import importlib.metadata as importlib_metadata
except ImportError:
    import importlib_metadata

try:
    __version__ = importlib_metadata.version("fastmcp")
except importlib_metadata.PackageNotFoundError:
    __version__ = "Unknown version"

"""GridStar MCP Server 启动兼容模块。"""

from fastmcp import FastMCP

from client import headers, send_post_request, url
from registry import register_tools
from tools import advanced, boundary, cad, generation, mesh, project, quality, query

MCP_HOST = "127.0.0.1"
MCP_PORT = 5656

mcp = register_tools(FastMCP())

OpenSpdFile = project.OpenSpdFile
SaveSpdFile = project.SaveSpdFile
SaveToAnotherSpdFile = project.SaveToAnotherSpdFile
ImportCADFile = project.ImportCADFile
ImportGridFile = project.ImportGridFile
ExportGrid = project.ExportGrid
ClearData = project.ClearData
ClearGridData = project.ClearGridData
GetStartAndEndPointByConnector = query.GetStartAndEndPointByConnector
GetCurrentSelectedIDs = query.GetCurrentSelectedIDs
GetAllObjectByType = query.GetAllObjectByType
UGCADSurfaceTranslate = cad.UGCADSurfaceTranslate
UGCADSurfaceRotate = cad.UGCADSurfaceRotate
UGCADSurfaceScale = cad.UGCADSurfaceScale
UGCADSurfaceMirror = cad.UGCADSurfaceMirror
UGSurfaceProcessing = cad.UGSurfaceProcessing
UGDamageRepari = cad.UGDamageRepari
DeleteConnector = mesh.DeleteConnector
DeleteDomain = mesh.DeleteDomain
DeleteBlock = mesh.DeleteBlock
DeleteFC = cad.DeleteFC
DeleteNbsFace = cad.DeleteNbsFace
UGSpitAssemblyCreateNewGroup = cad.UGSpitAssemblyCreateNewGroup
UGSpitAssemblyMoveNodesToNewGroup = cad.UGSpitAssemblyMoveNodesToNewGroup
UGSpitAssemblyRenameGroup = cad.UGSpitAssemblyRenameGroup
UGSpitAssemblyDeleteGroup = cad.UGSpitAssemblyDeleteGroup
BorderConditionAddGroup = boundary.BorderConditionAddGroup
BorderConditionDeleteGroup = boundary.BorderConditionDeleteGroup
BorderConditioConfigProperty = boundary.BorderConditioConfigProperty
BorderConditionSaveDataToDomain = boundary.BorderConditionSaveDataToDomain
GetAllBorderConditionGroups = boundary.GetAllBorderConditionGroups
ExamineConnector = quality.ExamineConnector
TranslateMesh = mesh.TranslateMesh
TranslateSurface = cad.TranslateSurface
UGZoomMesh = mesh.UGZoomMesh
UGMirrorSur = mesh.UGMirrorSur
UGRotateSurf = mesh.UGRotateSurf
UGJoinConnector = mesh.UGJoinConnector
UGJoinDomain = mesh.UGJoinDomain
UGHalfModelLine = mesh.UGHalfModelLine
ExamineBlock = quality.ExamineBlock
ExamineDomain = quality.ExamineDomain
GetScreenNormal = query.GetScreenNormal
MoveEndPoint = mesh.MoveEndPoint
UGDelRedundantDom = cad.UGDelRedundantDom
UGRepairRedundantDom = cad.UGRepairRedundantDom
CADIntersect = cad.CADIntersect
UGDeleteSubassembly = cad.UGDeleteSubassembly
AssembleDomain = mesh.AssembleDomain
UGAssembleDomain = mesh.UGAssembleDomain
UGInitialDomain = mesh.UGInitialDomain
UGReDimensionSetSpecifiedValue = mesh.UGReDimensionSetSpecifiedValue
UGReDimensionCopy = mesh.UGReDimensionCopy
UGReDimensionMatch = mesh.UGReDimensionMatch
UGReDimensionAverageDistribution = mesh.UGReDimensionAverageDistribution
UGReDimensionInversionDistribution = mesh.UGReDimensionInversionDistribution
UGReDimensionConfigDistribution = mesh.UGReDimensionConfigDistribution
UGReDimensionSmoothDistribution = mesh.UGReDimensionSmoothDistribution
UGSur = generation.UGSur
UGSplitConnector = mesh.UGSplitConnector
CreateCoons = cad.CreateCoons
UGBlockCreate = generation.UGBlockCreate
UGUGSp = generation.UGUGSp
TranslateConnector = mesh.TranslateConnector
CreateConnector = mesh.CreateConnector
Undo = project.Undo
Redo = project.Redo
ManualExtractConnector = cad.ManualExtractConnector
AutoExtractConnector = cad.AutoExtractConnector
ExportCADFile = project.ExportCADFile
ExportSolver_CFX = project.ExportSolver_CFX
ExportSolver_FTS = project.ExportSolver_FTS
ExportSolver = project.ExportSolver
TSplitConnector = mesh.TSplitConnector
CADSurfaceScale = cad.CADSurfaceScale
CADSurfaceRotate = cad.CADSurfaceRotate
CADSurfaceMirror = cad.CADSurfaceMirror
SplitBlock = mesh.SplitBlock
DealWatertight = cad.DealWatertight
GetModelParameters = query.GetModelParameters
GetDealWatertightTolenrance = query.GetDealWatertightTolenrance
GetAllSpitAssemblyGroupProperty = query.GetAllSpitAssemblyGroupProperty
GenerateSurMeshBySpitAssemblyGroupProperty = generation.GenerateSurMeshBySpitAssemblyGroupProperty
GetGenerateSurMeshDefaultParam = query.GetGenerateSurMeshDefaultParam
GetCreateBlockDefaultParam = query.GetCreateBlockDefaultParam
ProcessWithServer = advanced.ProcessWithServer
SegmentPartDirect = advanced.SegmentPartDirect
ClassifyTrailingEdgeDomains = advanced.ClassifyTrailingEdgeDomains
DetermineDirectionForType1 = advanced.DetermineDirectionForType1
IdentifyType2Roles = advanced.IdentifyType2Roles
SetConnectorPointCount = mesh.SetConnectorPointCount
CopyConnectorPointCount = mesh.CopyConnectorPointCount
SetConnectorAverageDistribution = mesh.SetConnectorAverageDistribution
SetConnectorSmoothDistribution = mesh.SetConnectorSmoothDistribution
SetConnectorConfigDistribution = mesh.SetConnectorConfigDistribution
AssembleConnectorsToDomain = mesh.AssembleConnectorsToDomain
GetSpliteAssemlyDomains = query.GetSpliteAssemlyDomains
GetConnectorsByDomain = query.GetConnectorsByDomain
MergeEdgesByDomain = mesh.MergeEdgesByDomain
GenerateLongAndNarrowFaceGrid = generation.GenerateLongAndNarrowFaceGrid
GenerateANisoDomainGrid = generation.GenerateANisoDomainGrid
GetConnectorStartAndEndUnitLenth = query.GetConnectorStartAndEndUnitLenth
GetPointCount = query.GetPointCount
GetNewConnectorId = query.GetNewConnectorId
GetDomainsByType = query.GetDomainsByType
GetRecentMessages = query.GetRecentMessages

if __name__ == "__main__":
    mcp.run(transport="sse", host=MCP_HOST, port=MCP_PORT)
