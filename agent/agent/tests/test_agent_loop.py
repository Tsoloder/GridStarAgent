"""回归测试：验证 agent_loop 产出的事件序列与 fixture 预期匹配。"""
import asyncio
from pathlib import Path
from types import SimpleNamespace

import pytest

from .conftest import (
    MockLLMClient,
    MockMcpBridge,
    MockSkillRegistry,
    MockContextManager,
    load_fixture,
)


@pytest.fixture(autouse=True)
def _isolate_sessions_dir(tmp_path, monkeypatch):
    """把会话落盘钉在沙箱里。

    多数用例自己 monkeypatch 了 ``session_mod.SESSIONS_DIR``，漏掉的那几个
    会往真实数据目录写一个只含 "hi" 的空会话。跑几轮测试，用户的会话列表里
    就多出几十个打不开的幽灵会话——测试不该动用户数据，所以这里兜底。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path / "sessions")
from config import config_from_dict
from session import Session


def test_base_prompt_path_points_to_agent_prompt():
    import agent_loop

    assert agent_loop._PROMPT_DIR == Path(__file__).resolve().parents[2] / "prompt"
    assert "CFD" in agent_loop._load_base_prompt()
    # 这份文件是整份基础系统提示词（身份 + 全局工具执行规则 + 输出格式），
    # 不是 CFD 专属流程。旧名 cfd_workflow.md 会让人以为它只在 CFD 流程里生效。
    assert (agent_loop._PROMPT_DIR / "base_prompt.md").is_file()


def test_base_prompt_falls_back_to_the_legacy_filename(tmp_path, monkeypatch):
    """改名不能变成"文件没了就静默空提示词"。

    回退分支存在的理由：代码与 prompt 目录分批更新时（先拉代码、后拉数据），
    或者打包漏了文件时，_load_base_prompt 返回空串会让整份系统提示词消失，
    而那是个安静得没有任何报错的故障。
    """
    import agent_loop

    monkeypatch.setattr(agent_loop, "_PROMPT_DIR", tmp_path)
    assert agent_loop._load_base_prompt() == ""

    (tmp_path / "cfd_workflow.md").write_text("legacy body", encoding="utf-8")
    assert agent_loop._load_base_prompt() == "legacy body"

    (tmp_path / "base_prompt.md").write_text("current body", encoding="utf-8")
    assert agent_loop._load_base_prompt() == "current body"

    (tmp_path / "base_prompt.md").unlink()
    assert agent_loop._load_base_prompt() == "legacy body"


def test_plan_scope_rule_is_stated_wherever_the_pipeline_is_listed():
    """计划范围：用户只要求表面网格时，不得把整条流水线一次列进计划。

    来自一次真实事故——用户说"我已经导入了数模，帮我生成表面网格"，模型却建了
    10 个阶段一路列到"空间网格生成"。三处语料同时在推它：base_prompt.md §5.1 给了
    一份 8 阶段的成品示例（可原样照抄）、cad-to-mesh.md 写"本流程的默认阶段计划"、
    SKILL.md 的顺序闸门只说"必须执行"而没限定范围。任何一处退回旧措辞，模型就会
    重新把计划撑大，所以三处一起钉住。
    """
    import agent_loop

    assert "计划范围" in agent_loop._load_base_prompt()
    # 范围规则是全局规则（§2 全局工具执行规则），不是 update_plan 的工具说明；
    # 它必须显式压过 Skill 文档里的"默认阶段/必经阶段"清单，否则下一次换个
    # Skill 又会照抄那份清单。
    assert "### 2.7 任务范围" in agent_loop._load_base_prompt()
    assert "Skill 文档里的流程清单是能力清单，不是本次任务清单" in agent_loop._load_base_prompt()
    # 工具描述是模型在决定 phases 内容时最近的一处语料
    assert "只列用户这次要求的阶段" in agent_loop.UPDATE_PLAN_TOOL.description

    repo_root = Path(__file__).resolve().parents[3]
    workflow = (agent_loop._PROMPT_DIR / "base_prompt.md").read_text(encoding="utf-8")
    assert "计划范围" in workflow
    assert '不是"必须把这些步都列进计划"' in workflow
    # 示例必须是"只要求表面网格"时的四阶段形态，不能退回整条链
    assert '"id": "surface"' in workflow
    assert '"id": "anisotropy"' not in workflow
    assert '"id": "space"' not in workflow

    reference = (repo_root / "res" / "skills" / "cfd-meshing-workflow"
                 / "references" / "cad-to-mesh.md").read_text(encoding="utf-8")
    assert "不是默认计划" in reference
    assert "这条管的是执行顺序，不是计划范围" in reference
    assert "一旦计划列入了本阶段且前置条件满足" in reference
    # 状态枚举必须与 task_ledger.PLAN_STATUSES 一致：写成 completed/active 会被
    # normalize_phase 静默降级成 pending，刚做完的阶段会显示成"还没开始"
    assert "标记 `import` 阶段为 `done`、下一阶段为 `in_progress`" in reference

    skill = (repo_root / "res" / "skills" / "cfd-meshing-workflow"
             / "SKILL.md").read_text(encoding="utf-8")
    assert '闸门管的是"做的时候不许跳步"，不管"计划要列多长"' in skill
    assert "表面网格生成成功后若要继续往下做" in skill


def test_screen_readonly_tools_skip_plan_and_approval_gates():
    """截图/切前台只读：auto 模式不必先建计划，manual 模式不弹审批卡。

    这两道门禁都靠 _is_query_tool 分流（auto 模式见 _only_readonly_calls，
    manual 模式见工具执行分支），而它原先只认 Get/Query/... 前缀，
    CaptureGridStarWindow 因此被当成操作类工具拦下过。
    """
    import agent_loop

    for name in ("CaptureGridStarWindow", "ActivateGridStarWindow",
                 "FindGridStarWindow", "GetUIElementInfo"):
        assert agent_loop._is_query_tool(name), name
    for name in ("ClickAtPoint", "DragAtPoint", "ScrollAtPoint", "ClickUIElement",
                 "TypeTextInUIElement", "SendKeyboardShortcut"):
        assert not agent_loop._is_query_tool(name), name
    assert agent_loop._only_readonly_calls([{"name": "CaptureGridStarWindow"}])
    assert not agent_loop._only_readonly_calls(
        [{"name": "CaptureGridStarWindow"}, {"name": "ClickAtPoint"}])


def test_capture_tool_is_declared_without_arguments(monkeypatch):
    """模型侧零参数：截图不该为参数确认多花一轮，也不该有机会传错参数。

    实现体 _capture_gridstar 仍保留 window_title / activate / clean 三个开关，
    脚本与自测要验 PrintWindow 回退路径时用得上。但模型侧那三项行为是定死的：
    不做标题过滤、截图前先把 GridStar 切到前台、不走 PrintWindow 自绘。这里断言
    的是生效值而不是写法，改成显式传参或依赖默认值都行，值不对就红。
    """
    import inspect
    import sys

    tools_dir = Path(__file__).resolve().parents[2] / "tools"
    if str(tools_dir) not in sys.path:
        sys.path.insert(0, str(tools_dir))
    sa = pytest.importorskip("screen_automation",
                             reason="桌面依赖（mss/uiautomation）未安装")

    assert list(inspect.signature(sa.CaptureGridStarWindow).parameters) == []
    inner = inspect.signature(sa._capture_gridstar).parameters
    assert {"window_title", "activate", "clean"} <= set(inner)

    names = ("window_title", "activate", "clean")
    defaults = {"window_title": "", "activate": True, "clean": False}
    calls = []

    def fake(*args, **kwargs):
        passed = dict(zip(names, args))
        passed.update(kwargs)
        calls.append({**defaults, **passed})
        return "{}"

    monkeypatch.setattr(sa, "_capture_gridstar", fake)
    sa.CaptureGridStarWindow()
    assert calls == [defaults]


def _make_config():
    return config_from_dict({
        "version": 1,
        "default_model": "test/test-model",
        "providers": [{
            "id": "test", "name": "Test", "base_url": "http://localhost:11434",
            "api_key": "test-key", "default_api": "openai-chat",
        }],
        "models": [{"provider": "test", "id": "test-model", "context_window": 32000}],
    })


def _make_session(session_id="test-session"):
    import uuid
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, session_id)) if session_id else str(uuid.uuid4())
    now = "2026-08-14T10:00:00"
    return Session(id=sid, title="Test", created_at=now, updated_at=now)


def test_screenshot_gate_follows_capabilities_vision():
    """截图能不能送达模型，按配置里的 capabilities.vision 判定。"""
    import agent_loop

    config = config_from_dict({
        "version": 1,
        "default_model": "test/text-only",
        "providers": [{
            "id": "test", "name": "Test", "base_url": "http://localhost:11434",
            "api_key": "test-key", "default_api": "openai-chat",
        }],
        "models": [
            {"provider": "test", "id": "text-only", "context_window": 32000,
             "capabilities": {"vision": False}},
            {"provider": "test", "id": "sees-images", "context_window": 32000,
             "capabilities": {"vision": True}},
        ],
    })
    assert agent_loop._model_sees_images(config, "test/text-only") is False
    assert agent_loop._model_sees_images(config, "test/sees-images") is True
    # 配置里查不到这个模型时按「看不到」处理（fail-closed）：
    # 误判成看得到会让模型拿文件名/工程名猜类型，代价比多问一句大得多。
    assert agent_loop._model_sees_images(config, "test/absent") is False


def test_screenshot_note_tells_the_truth_about_vision():
    import agent_loop

    delivered = agent_loop._screenshot_note("test/sees-images", r"C:\uploads\a.png", True)
    # 不再替网关承诺「一定送达」：vision 是表单默认值，未经实测。
    # 只说「已按图片附加」，并要求模型看不到时自曝、改用问用户。
    assert "图片形式发送" not in delivered
    assert "a.png" in delivered
    assert "看不到" in delivered
    assert "ask_user_question" in delivered

    blocked = agent_loop._screenshot_note("test/text-only", r"C:\uploads\a.png", False)
    assert "不会送达模型" in blocked
    assert "a.png" in blocked and "test/text-only" in blocked
    assert "不要重复截图" in blocked
    # 看不到图时的退路是"问用户"，不是"拿工程数据猜"
    assert "ask_user_question" in blocked
    assert "改用工程数据" not in blocked


@pytest.mark.asyncio
async def test_domain_detection_names_the_screenshot_as_the_only_evidence(tmp_path, monkeypatch):
    """模型类型只能看主视口截图定，参数记忆不是依据。

    现场：真实的导弹参数（growthRate 1.2 / controlLayer 50）躺在
    <tool_parameter_memory> 里，模型据此宣称"从当前工程已确认的参数记录判断，
    当前是导弹类模型"——而主视口截图其实已经送达（该轮请求比上一轮多出约
    1100 input token，正好是一张 2560x1552 图片的价钱）。旧文案只写了
    "通过视觉分析主视口的模型是飞机还是导弹"，既没说这是唯一依据，
    也没说参数记忆不算依据，于是模型挑了旁边那个更好读的文本线索。
    """
    import session as session_mod
    import tool_memory

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    monkeypatch.setattr(tool_memory, "MEMORY_PATH", tmp_path / "tool_memory.json")
    tool_memory.remember("GenerateANisoDomainGrid", {"controlLayer": 50, "growthRate": 1.2})

    runtime = _ScriptedRuntime([_text_events("好的。")])
    await _run_direct(runtime, "帮我生成表面网格", config=_vision_config(True))
    prompt = runtime.system_prompts[0]

    # 参数记忆确实作为独立段落注入了，否则这条测试什么也没验到
    # （注意 agent_loop 里 <domain_detection> 排在 <tool_parameter_memory> 之前，
    #  用 "controlLayer" in prompt 判断会被 domain_detection 自己的措辞蒙混过去）
    assert '<tool name="GenerateANisoDomainGrid">' in prompt
    assert '"controlLayer": 50' in prompt

    assert "<domain_detection>" in prompt
    assert "这是唯一的判断依据" in prompt
    # 显式点名参数记忆：不点名，模型会把它归进"工程数据"这一类合法线索
    assert "不绑定任何工程" in prompt
    assert "不要用它们「认出」这次在做什么" in prompt
    # 先观察再分类：说不清特征就不许下结论
    assert "说不清特征就不要下结论" in prompt
    # 旧退路把"改用工程数据判断"写成了正常路径，必须消失
    assert "改用工程数据" not in prompt
    # 看不到图时问用户，并保持原有的 set_active_category 链路
    assert "ask_user_question" in prompt
    assert "set_active_category" in prompt
    # 截到一张被别的窗口压住的图时，先激活再重截，而不是硬着头皮下结论
    assert "occluded_warning" in prompt
    assert "ActivateGridStarWindow" in prompt


def _vision_config(vision: bool):
    """一份 capabilities.vision 明确的配置，用来验领域识别那段提示词怎么注入。"""
    return config_from_dict({
        "version": 1,
        "default_model": "test/vision-model",
        "providers": [{
            "id": "test", "name": "Test", "base_url": "http://localhost:11434",
            "api_key": "test-key", "default_api": "openai-chat",
        }],
        "models": [{"provider": "test", "id": "vision-model", "context_window": 32000,
                    "capabilities": {"vision": vision}}],
    })


@pytest.mark.asyncio
async def test_visionless_model_is_asked_for_the_skill_domain(tmp_path, monkeypatch):
    """模型看不了图时，领域识别的那条主路径必须被就地换掉，改由用户给领域。

    现场：<domain_detection> 的第 2 步写的是「看主视口里的模型是飞机还是导弹。
    **这是唯一的判断依据。**」。模型被配置成 capabilities.vision=false（网关会
    静默丢图）时，这一步无法执行，而主路径旁边全是更好读的文本线索（工程名、
    <tool_parameter_memory>、历史会话），模型于是挑一个凑结论——识别结果直接
    决定后面套哪个领域的 Skill 与参数，猜错整条流程都偏。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    runtime = _ScriptedRuntime([_text_events("好的。")])
    await _run_direct(runtime, "帮我生成表面网格", config=_vision_config(False))
    prompt = runtime.system_prompts[0]

    assert "<vision_disabled_domain_detection>" in prompt
    assert "capabilities.vision=false" in prompt
    assert "ask_user_question" in prompt
    assert "set_active_category" in prompt
    # 分类的取值必须与界面的「专业分类」下拉一致，否则用户选的与模型设的不是一回事
    assert '"aircraft"' in prompt and '"missile"' in prompt
    assert "专业分类" in prompt
    # 退路要指出界面位置，而不是让用户自己去猜在哪儿选
    assert "/ 菜单" in prompt or "/` 菜单" in prompt
    # auto 模式不能被"计划收尾前不得停下询问"挡回来
    assert "auto" in prompt
    # 用户上传的图片同样看不到：不许假装看过
    assert "不要假装看过" in prompt
    # 截图那一步要被显式降级，否则模型仍会按主路径反复截图
    assert "不要为了识别模型类型去调 CaptureGridStarWindow 看图" in prompt
    assert "不要反复截图" in prompt
    # 0/1/2 步与「确认主视口」整段都要消失：只换第 2 步那一行不够，
    # 模型会照主路径去截图、去重截到「能看清」，而它其实什么也看不到。
    assert "把桌面截图分组打开" not in prompt
    assert "ActivateGridStarWindow 把它切到" not in prompt
    assert "这是唯一的判断依据" not in prompt
    # 硬闸门段落必须排在通用识别引导之前：反过来的话模型先读到的是"看主视口判断"
    assert prompt.index("<vision_disabled_domain_detection>") < prompt.index("<domain_detection>")
    assert "不要截图" in prompt
    # 问用户时别把问题写死成"是飞机还是导弹"：要说清缺的是领域/技能分类
    assert "无法自动识别模型类型" in prompt
    # 旧措辞：把"改用工程数据判断"写成正常路径
    assert "改用工程数据" not in prompt

    # 用户给出的领域就是"用哪个领域的 Skill"，提示词里要说清这一点
    assert "请用户给出本次按哪个领域处理" in prompt


@pytest.mark.asyncio
async def test_image_capable_model_keeps_the_original_domain_path(tmp_path, monkeypatch):
    """能看图的模型拿到的仍是原来那段：新闸门只该在看不到图时出现。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    runtime = _ScriptedRuntime([_text_events("好的。")])
    await _run_direct(runtime, "帮我生成表面网格", config=_vision_config(True))
    prompt = runtime.system_prompts[0]

    assert "<domain_detection>" in prompt
    assert "这是唯一的判断依据" in prompt
    # 能看图时 0/1/2 步与「确认主视口」都在，主路径没被换掉
    assert "把桌面截图分组打开" in prompt
    assert "ActivateGridStarWindow 把它切到" in prompt
    assert "<vision_disabled_domain_detection>" not in prompt
    assert "capabilities.vision=false" not in prompt
    # 主路径的第 2 步不该被"改用问用户"的插入语污染
    assert "不要截图" not in prompt


@pytest.mark.asyncio
async def test_screenshot_tool_scope_is_pinned_to_domain_detection(tmp_path, monkeypatch):
    """截图工具只服务领域识别：巡检界面、核对操作结果这些用法要被文案挡掉。

    现场：CaptureGridStarWindow 在提示词里有两条通道——<domain_detection> 叫它截图认
    模型，同一段末尾又教它"需要操作界面时"用截图定位。模型于是把它当通用界面巡检
    工具：分类早就定了还反复截图去核对按钮位置，一张 2560x1552 的图约合 1100 input
    token，几轮下来上下文全喂给了同一张界面图。这条限制得四头同时钉：base_prompt 的
    §2.8、注入的 <domain_detection>、工具自己的描述（模型决定调不调它时最近的一处
    语料）、以及 screen 工具组的说明。任何一处退回旧措辞，模型都会重新拿它巡检。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    import agent_loop

    base = agent_loop._load_base_prompt()
    assert "### 2.8 截图工具的用途限制" in base
    assert "当前只服务一件事" in base
    assert "确认 GridStar 里导入的是什么领域的模型" in base
    # §1 那句领域识别要指向 §2.8，否则只读 §1 的人不知道还有这条限制
    assert "`CaptureGridStarWindow` 当前只服务这一步" in base
    # 限制要给出唯一例外，否则模型会把"不许截图"理解成界面操作也不能做
    assert "唯一例外：用户明确要求操作界面时" in base

    runtime = _ScriptedRuntime([_text_events("好的。")])
    await _run_direct(runtime, "帮我生成表面网格", config=_vision_config(True))
    prompt = runtime.system_prompts[0]
    domain = prompt[prompt.index("<domain_detection>"):]
    assert "这个工具当前只用来确认导入的模型属于哪个领域" in domain
    assert "用户明确要求你操作界面才走这一段" in domain
    assert "分类一旦确定就不再调它" in domain

    # 看不到图的那份闸门同样收口，不留"换个模型就能拿它巡检"的口子
    runtime = _ScriptedRuntime([_text_events("好的。")])
    await _run_direct(runtime, "帮我生成表面网格", config=_vision_config(False))
    assert "这个工具当前只用于确认导入的模型属于哪个领域" in runtime.system_prompts[0]

    # 工具描述：模型在决定"要不要调它"时读到的那一段
    import sys

    tools_dir = Path(__file__).resolve().parents[2] / "tools"
    if str(tools_dir) not in sys.path:
        sys.path.insert(0, str(tools_dir))
    sa = pytest.importorskip("screen_automation",
                             reason="桌面依赖（mss/uiautomation）未安装")
    description = sa.CaptureGridStarWindow.__doc__ or ""
    assert "用途限定" in description
    assert "只用来确认 GridStar 里导入的是什么领域的模型" in description
    assert "分类一旦确定（用户选过或已 set_active_category）就不需要再截图" in description

    # 工具分组说明：模型用 GetToolGroups 挑分组时读到的就是这一段
    agent_dir = Path(__file__).resolve().parents[2]
    if str(agent_dir) not in sys.path:
        sys.path.insert(0, str(agent_dir))
    import registry

    screen = [g for g in registry.TOOL_GROUPS if g["id"] == "screen"]
    assert screen, "screen 分组缺失：screen_automation 刚导入成功，注册表里就该有它"
    group = screen[0]["description"]
    assert "当前用途范围" in group
    assert "只用于确认 GridStar 里导入的是什么领域的模型" in group


@pytest.mark.asyncio
async def test_screenshot_result_keeps_the_tool_own_field_report(tmp_path, monkeypatch):
    """截图工具如实报出的现场字段必须原样送到模型手里，只摘掉 base64。

    现场：GridStar 被压在别的窗口下面，CaptureGridStarWindow 老老实实回了
    occluded_warning（"截图时 GridStar 不是前台窗口，画面里可能包含压在上面的
    其它窗口…先调用 ActivateGridStarWindow"）和 is_foreground=false。
    旧实现把 result 整块重建成 {width,height,format,note,image_delivered}，
    这些字段连同 base64 一起没了：模型拿到一张根本没有 GridStar 视口的图，
    也没有任何解释，而 <domain_detection> 偏偏叫它去读 result 里的
    is_foreground——那个键已经不存在了。于是它拿提示词里的参数记忆猜类型。
    """
    import base64
    import json

    import agent_loop
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    monkeypatch.setattr(agent_loop, "UPLOADS_DIR", tmp_path / "uploads")

    png = base64.b64encode(b"\x89PNG\r\n\x1a\n" + b"0" * 64).decode("ascii")
    shot = json.dumps({
        "status": "success",
        "result": {
            "image": png, "width": 2560, "height": 1552, "format": "png",
            "method": "region",
            "capture_rect": {"left": 0, "top": 0, "width": 2560, "height": 1552},
            "coord_space": {"max": 1000},
            "window": {"hwnd": 134538, "is_iconic": False},
            "is_foreground": False,
            "occluded_warning": "截图时 GridStar 不是前台窗口，画面里可能包含压在上面的其它窗口",
        },
    })

    runtime = _ScriptedRuntime([
        _tool_call_events("CaptureGridStarWindow", {}),
        _text_events("看到了。"),
        _text_events("看到了。"),
    ])
    # 还原事故现场：当时用的 deepseek-v4.1-flash 是 capabilities.vision = true，
    # 图片确实送达了——正因为送到了，模型才会拿着一张没有 GridStar 的截图去找别的线索。
    vision_config = config_from_dict({
        "version": 1,
        "default_model": "test/test-model",
        "providers": [{
            "id": "test", "name": "Test", "base_url": "http://localhost:11434",
            "api_key": "test-key", "default_api": "openai-chat",
        }],
        "models": [{"provider": "test", "id": "test-model", "context_window": 32000,
                    "capabilities": {"vision": True}}],
    })
    events = await _run_direct(
        runtime, "帮我生成表面网格", interaction_mode="auto", config=vision_config,
        mcp=MockMcpBridge({"CaptureGridStarWindow": shot}),
    )
    result = next(e for e in events if e["type"] == "tool_result")["result"]
    data = json.loads(result)["result"]

    # base64 必须摘掉：留着会让会话文件背上几十万字符
    assert "image" not in data
    # 现场字段一个都不能少，模型全靠它们判断"这张图能不能用"
    assert data["is_foreground"] is False
    assert data["occluded_warning"]
    assert data["capture_rect"]["width"] == 2560
    assert data["coord_space"]["max"] == 1000
    assert data["method"] == "region"
    assert data["window"]["hwnd"] == 134538
    assert data["image_delivered"] is True
    # 文案不再替网关承诺「一定送达」：vision 是未实测的声明，
    # 只说已按图片附加，并要求模型看不到时自曝、改用问用户。
    assert "按图片附加到本轮请求" in data["note"]
    assert "看不到" in data["note"]


@pytest.mark.asyncio
async def test_historical_images_degrade_to_text_while_the_new_one_is_sent(tmp_path, monkeypatch):
    """图片只活一轮：旧消息里的图退化成占位文本，本轮上传的图才真的发出去。

    对齐 openhanako message-sanitizer 的策略。旧实现把历史里所有图片每轮重新
    读盘、重新编码、重新发送，同一张图问十个问题就付十次视觉 token，还把窗口
    一路顶到上游 400。
    """
    import agent_loop
    import document_loader
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    fake_png = b"\x89PNG\r\n\x1a\n" + b"0" * 64
    old_shot = tmp_path / "old.png"
    new_shot = tmp_path / "new.png"
    old_shot.write_bytes(fake_png)
    new_shot.write_bytes(fake_png)

    session = _make_session(None)
    session.append_user("先看这张旧图", [],
                        [{"kind": "image", "name": "old.png", "path": str(old_shot)}])
    session.append_assistant("看过了。")

    runtime = _ScriptedRuntime([
        _text_events("好的。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    async for _ in agent_loop.run_agent_loop(
        session=session,
        user_message="再看这张新图",
        base_system_prompt="base",
        config=_vision_config(True),
        mcp=MockMcpBridge({}),
        ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(),
        attachments=[{"kind": "image", "name": "new.png", "path": str(new_shot)}],
        model_runtime=runtime,
    ):
        pass

    sent = runtime.messages[0]
    users = [m for m in sent if m.get("role") == "user"]

    # 旧消息的图片不能再作为图片出现，只能留下占位文本
    old_content = users[0]["content"]
    assert isinstance(old_content, str)
    assert document_loader.HISTORICAL_IMAGE_NOTICE in old_content

    # 本轮上传的图片必须真的作为图片发出去
    new_content = users[-1]["content"]
    assert isinstance(new_content, list)
    assert any(part.get("type") == "image" for part in new_content)
    assert document_loader.HISTORICAL_IMAGE_NOTICE not in "".join(
        str(part.get("text", "")) for part in new_content if isinstance(part, dict))


@pytest.mark.asyncio
async def test_replayed_screenshot_degrades_after_it_was_sent_once(tmp_path, monkeypatch):
    """截图只活一轮：同一条 user 消息上累加的截图，发过的那张下一轮退化成占位。

    屏幕自动化一次任务会连截几十张图，全都追加到同一条 user 消息上。旧实现每轮
    把这一串图重新读盘、重新编码、重新发送——第 N 轮就背上 N 张图的视觉 token，
    一路把窗口顶爆。
    """
    import base64
    import json

    import agent_loop
    import document_loader
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    monkeypatch.setattr(agent_loop, "UPLOADS_DIR", tmp_path / "uploads")

    png = base64.b64encode(b"\x89PNG\r\n\x1a\n" + b"0" * 64).decode("ascii")
    shot = json.dumps({"status": "success", "result": {
        "image": png, "width": 100, "height": 100, "format": "png"}})

    runtime = _ScriptedRuntime([
        _tool_call_events("CaptureGridStarWindow", {}),
        _tool_call_events("CaptureGridStarWindow", {}),
        _text_events("好的。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    await _run_direct(runtime, "帮我看看画面", interaction_mode="auto",
                      config=_vision_config(True),
                      mcp=MockMcpBridge({"CaptureGridStarWindow": shot}))

    def _last_user(messages):
        return [m for m in messages if m.get("role") == "user"][-1]

    # 第一张截图在它到达的那一轮真的作为图片发出去了
    first_shot_turn = _last_user(runtime.messages[1])["content"]
    assert any(part.get("type") == "image" for part in first_shot_turn)

    # 第二张截图到达时，第一张已经发过一轮，必须退化成占位文本、只发新的那张
    second_shot_turn = _last_user(runtime.messages[2])["content"]
    images = [part for part in second_shot_turn if part.get("type") == "image"]
    assert len(images) == 1
    assert document_loader.HISTORICAL_IMAGE_NOTICE in "".join(
        str(part.get("text", "")) for part in second_shot_turn if isinstance(part, dict))


def _patch_stream_runtime(monkeypatch, agent_loop, stream_chat):
    async def stream_runtime(runtime, model_key, messages, system_prompt, tools):
        async for event in stream_chat(
            messages=messages, system_prompt=system_prompt, tools=tools
        ):
            yield event

    monkeypatch.setattr(agent_loop, "_stream_runtime", stream_runtime)


async def _run_fixture(fixture_name: str, ledger=None) -> list:
    fixture = load_fixture(fixture_name)
    inp = fixture["input"]
    session = _make_session(inp["session_id"])
    mock_llm = MockLLMClient(fixture["mock_llm_responses"])
    mock_mcp = MockMcpBridge(
        fixture.get("mock_mcp_results", {}),
        tool_schemas=fixture.get("mock_tool_schemas", {}),
    )

    import agent_loop

    events = []
    stream = agent_loop.run_agent_loop(
        session=session,
        user_message=inp["message"],
        base_system_prompt=inp.get("system_prompt", ""),
        config=_make_config(),
        mcp=mock_mcp,
        ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(),
        selected_skills=inp.get("selected_skills", []),
        request_tool_approval=None,
        attachments=inp.get("attachments"),
        display_content=inp.get("display_content", ""),
        interaction_mode=inp.get("interaction_mode", "manual"),
        model_override=inp.get("model_id"),
        model_runtime=mock_llm,
        ledger=ledger,
    )
    async for event in stream:
        events.append(event)
        if len(events) > 100:
            break
    return events


def _event_types(events):
    return [event["type"] for event in events]


def _full_text(events):
    return "".join(event.get("delta", "") for event in events if event["type"] == "text_chunk")


@pytest.mark.asyncio
async def test_simple_chat():
    events = await _run_fixture("simple_chat")
    types = _event_types(events)
    assert "heartbeat" in types
    assert "text_chunk" in types
    assert types[-1] == "done"
    assert "tool_call" not in types
    assert "你好" in _full_text(events)
    assert "GridStar" in _full_text(events)


@pytest.mark.asyncio
async def test_tool_call_flow():
    events = await _run_fixture("tool_call_flow")
    types = _event_types(events)
    assert types.index("tool_call") < types.index("tool_result")
    assert types[-1] == "done"
    assert "导入完成" in _full_text(events)
    tool_call = next(event for event in events if event["type"] == "tool_call")
    assert tool_call["name"] == "ImportCAD"
    assert tool_call["args"]["file"] == "NACA0012.txt"
    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "NACA0012" in tool_result["result"]


@pytest.mark.asyncio
async def test_auto_mode_flow(tmp_path, monkeypatch):
    import uuid
    import session as session_mod
    from task_ledger import TaskLedger

    # 隔离会话数据目录，避免污染真实台账/消息文件
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "test-auto-001"))
    ledger = TaskLedger(sid)

    events = await _run_fixture("auto_mode_flow", ledger=ledger)
    types = _event_types(events)

    # update_plan 是首个执行的工具（内置、不走 MCP/审批）
    first_tool = next(e for e in events if e["type"] == "tool_call")
    assert first_tool["name"] == "update_plan"
    assert first_tool.get("internal") is True
    # plan_updated 在 update_plan 的 tool_result 前发出，且早于外部工具执行
    plan_events = [e for e in events if e["type"] == "plan_updated"]
    assert len(plan_events) == 2
    first_update_idx = types.index("plan_updated")
    assert types.index("tool_call") < first_update_idx
    assert plan_events[0]["plan"]["id"] == "mesh-main"
    assert plan_events[0]["plan"]["phases"][0]["status"] == "in_progress"
    # 全量替换：第二次更新后所有阶段完成
    assert all(p["status"] == "done" for p in plan_events[1]["plan"]["phases"])
    assert plan_events[1]["plan"]["phases"][0]["note"] == "模型树获取完成"

    assert "tool_result" in types
    assert types[-1] == "done"
    assert "模型树获取完成" in _full_text(events)

    # 台账自动记账：update_plan 与外部工具均入账且全部成功
    tools_called = [call["tool"] for call in ledger.calls]
    assert "update_plan" in tools_called
    assert "GetModelTree" in tools_called
    assert all(call["ok"] for call in ledger.calls)
    # 外部调用归属活动阶段
    get_tree = next(c for c in ledger.calls if c["tool"] == "GetModelTree")
    assert get_tree["phase"] == "CAD导入"


@pytest.mark.asyncio
async def test_auto_mode_defer_internal_tools(tmp_path, monkeypatch):
    """门禁暂缓执行类工具时，同批次的内部工具（read_skill_resource）应放行执行。"""
    import uuid
    import session as session_mod
    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "test-auto-002"))
    ledger = TaskLedger(sid)

    events = await _run_fixture("auto_mode_defer_internal", ledger=ledger)
    types = _event_types(events)
    assert types == load_fixture("auto_mode_defer_internal")["expected_event_types"]

    # 第一轮：内部工具放行并正常执行
    first_tool = next(e for e in events if e["type"] == "tool_call")
    assert first_tool["name"] == "read_skill_resource"
    assert first_tool.get("internal") is True

    # 执行类工具全程只出现一次（首轮被暂缓，第二轮建计划后才真正执行）
    exec_calls = [i for i, e in enumerate(events)
                  if e["type"] == "tool_call"
                  and e["name"] == "GenerateSurMesh"]
    assert len(exec_calls) == 1
    assert types.index("plan_updated") < exec_calls[0]

    # 计划事件正常发出
    plan_events = [e for e in events if e["type"] == "plan_updated"]
    assert len(plan_events) == 1
    assert plan_events[0]["plan"]["id"] == "surmesh-main"

    assert types[-1] == "done"

    # 台账：内部工具与被暂缓后重发的执行类工具均入账，被暂缓的首次调用不入账
    tools_called = [call["tool"] for call in ledger.calls]
    assert "read_skill_resource" in tools_called
    assert "GenerateSurMesh" in tools_called
    assert tools_called.count("GenerateSurMesh") == 1
    assert all(call["ok"] for call in ledger.calls)

    # 历史完整性：同一批次的 assistant 消息只应落盘一次，且其中每个 tool_call
    # 都必须有配对的 tool 消息。早先把整批落盘一次、内部工具裁剪后又落盘一次，
    # 同一个 call id 出现在两条 assistant 消息里，先落盘那条永远等不到结果，
    # 下一轮请求会被 provider 以「tool_calls 必须紧跟 tool 消息」拒掉。
    messages = session_mod._read_jsonl(
        str(session_mod.session_dir(sid) / "messages.jsonl")
    )
    assert messages, "会话消息未落盘"
    persisted_ids = [
        call.get("id")
        for message in messages
        if message.get("role") == "assistant"
        for call in (message.get("tool_calls") or [])
    ]
    assert len(persisted_ids) == len(set(persisted_ids)), \
        "同一个 tool_call id 被重复落盘：%s" % persisted_ids

    answered = {
        message.get("tool_call_id")
        for message in messages
        if message.get("role") == "tool"
    }
    for message in messages:
        if message.get("role") != "assistant":
            continue
        pending = {call.get("id") for call in (message.get("tool_calls") or [])}
        if pending:
            assert pending <= answered, \
                "assistant 的 tool_call 缺少配对的 tool 结果：%s" % (pending - answered)


@pytest.mark.asyncio
async def test_auto_mode_readonly_query_exempt_from_plan_gate(tmp_path, monkeypatch):
    """单步只读查询豁免建计划门禁：直接执行，不暂缓、不建计划、不多烧一次请求。"""
    import json
    import uuid
    import session as session_mod
    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, "test-auto-003"))
    ledger = TaskLedger(sid)

    events = await _run_fixture("auto_mode_readonly_query_exempt", ledger=ledger)
    types = _event_types(events)

    # 门禁未触发：查询工具首轮就真正执行，没有 [deferred] 占位结果
    query_call = next(e for e in events if e["type"] == "tool_call")
    assert query_call["name"] == "ListMeshFaces"
    query_result = next(e for e in events if e["type"] == "tool_result")
    assert "[deferred]" not in query_result["result"]
    assert "face_1" in query_result["result"]

    # 只读查询不需要阶段计划，也不该为补计划再花一次请求
    assert "plan_updated" not in types
    assert ledger.plan is None
    assert types.count("traj_request_start") == 2
    assert types[-1] == "done"

    # 台账照常记账（无活动阶段时归属为空）
    assert [call["tool"] for call in ledger.calls] == ["ListMeshFaces"]
    assert ledger.calls[0]["phase"] == ""

    # 缓存用量透传到 done 事件并落盘，前端才能显示真实命中率
    assert events[-1]["cache_read_tokens"] == 21504 * 2
    stored = [json.loads(line) for line in
              (tmp_path / sid / "messages.jsonl").read_text(encoding="utf-8").splitlines()]
    usage = next(m["usage"] for m in stored
                 if m.get("role") == "assistant" and m.get("usage"))
    assert usage["cache_read"] == 21504 * 2
    assert usage["model"] == "test-model"


@pytest.mark.asyncio
async def test_format_retry_flow():
    events = await _run_fixture("format_retry_flow")
    chunks = [event for event in events if event["type"] == "text_chunk"]
    assert len(chunks) >= 2
    assert "options" not in chunks[0]["delta"]
    assert "options" in _full_text(events)
    assert events[-1]["type"] == "done"


def _text_events(text: str) -> list:
    return [
        SimpleNamespace(type="text_delta", delta=text),
        SimpleNamespace(type="usage", input_tokens=100, output_tokens=20, total_tokens=120,
                        cache_read_tokens=0, cache_write_tokens=0, reasoning_tokens=0),
    ]


def _tool_call_events(name: str, args: dict) -> list:
    return [
        SimpleNamespace(type="tool_call_end", call_id="call-1", name=name,
                        arguments=args, parse_error=None),
        SimpleNamespace(type="usage", input_tokens=100, output_tokens=20, total_tokens=120,
                        cache_read_tokens=0, cache_write_tokens=0, reasoning_tokens=0),
    ]


async def _run_direct(runtime, message, ledger=None, mcp=None, display_content="",
                      interaction_mode="manual", config=None):
    import agent_loop

    return [event async for event in agent_loop.run_agent_loop(
        _make_session(None), message, "base", config or _make_config(), mcp or MockMcpBridge({}),
        MockContextManager(), MockSkillRegistry(), model_runtime=runtime, ledger=ledger,
        display_content=display_content, interaction_mode=interaction_mode,
    )]


@pytest.mark.asyncio
async def test_conversational_turn_skips_format_retry(tmp_path, monkeypatch):
    """寒暄轮没有决策点：不该为凑结构化输出再发一次请求（省一整轮前缀输入）。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([_text_events("好的，随时欢迎再来咨询。")])
    events = await _run_direct(runtime, "下次再说")
    assert len(runtime.exposed_tools) == 1
    assert _event_types(events)[-1] == "done"
    assert "随时欢迎" in _full_text(events)


@pytest.mark.asyncio
async def test_task_reply_without_structure_still_retries(tmp_path, monkeypatch):
    """真实请求仍受格式契约约束：首答复缺结构化块时照旧重试一次。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _text_events("已为你加载当前模型。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "帮我查看当前模型")
    assert len(runtime.exposed_tools) == 2
    assert "options" in _full_text(events)


@pytest.mark.asyncio
async def test_tool_execution_blocks_format_exemption(tmp_path, monkeypatch):
    """本轮执行过工具就仍有决策点：寒暄豁免不生效，缺结构化块照旧重试。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _tool_call_events("ImportCAD", {"file": "wing.step"}),
        _text_events("导入完成，共 128 个数模面。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "谢谢", mcp=MockMcpBridge({"ImportCAD": "ok"}))
    assert len(runtime.exposed_tools) == 3
    assert any(event["type"] == "tool_result" for event in events)


@pytest.mark.asyncio
async def test_active_plan_blocks_format_exemption(tmp_path, monkeypatch):
    """台账里还有未收尾阶段时，寒暄轮也要给结构化出口。"""
    import session as session_mod

    import uuid

    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    ledger = TaskLedger(str(uuid.uuid4()))
    ledger.update_plan("cad-mesh", "CAD 到 CFD 网格生成",
                       [{"id": "a", "title": "阶段A", "status": "in_progress"}])
    runtime = _ScriptedRuntime([
        _text_events("好的。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "谢谢", ledger=ledger)
    assert len(runtime.exposed_tools) == 2
    assert _event_types(events)[-1] == "done"


@pytest.mark.asyncio
async def test_card_answer_blocks_format_exemption(tmp_path, monkeypatch):
    """卡片作答即使文本落在白名单里也不能豁免：那是一次决策答复。

    前端把选项的 value 当消息、label 当 display_content 发过来，所以
    display_content 非空即"这是卡片作答"，必须继续给结构化出口。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _text_events("好的。"),
        _text_events("```json\n{\"options\": [{\"label\": \"继续\", \"value\": \"go\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "谢谢", display_content="谢谢你")
    assert len(runtime.exposed_tools) == 2
    assert _event_types(events)[-1] == "done"


def test_conversational_message_matching():
    """豁免判定必须窄：真实请求、应答词、稍长的寒暄扩展句都不能命中。"""
    import agent_loop

    assert agent_loop._is_conversational_message("你好")
    assert agent_loop._is_conversational_message("  谢谢！ ")
    assert agent_loop._is_conversational_message("下次再说。")
    assert not agent_loop._is_conversational_message("导入模型")
    assert not agent_loop._is_conversational_message("你好，帮我导入模型")
    assert not agent_loop._is_conversational_message("")
    assert not agent_loop._is_conversational_message("你好" * 10)
    # 应答词可能是放行确认，不进白名单
    for ack in ("好的", "好", "收到", "OK", "明白了"):
        assert not agent_loop._is_conversational_message(ack)


def test_plan_in_flight_detection():
    import agent_loop

    assert not agent_loop._plan_in_flight(None)
    assert not agent_loop._plan_in_flight(SimpleNamespace(plan=None))
    assert not agent_loop._plan_in_flight(SimpleNamespace(
        plan={"phases": [{"status": "done"}, {"status": "skipped"}]}))
    assert agent_loop._plan_in_flight(SimpleNamespace(
        plan={"phases": [{"status": "in_progress"}]}))
    # 缺省 status 视为 pending，同样算在途
    assert agent_loop._plan_in_flight(SimpleNamespace(plan={"phases": [{"id": "x"}]}))


@pytest.mark.asyncio
async def test_structured_continuation():
    events = await _run_fixture("structured_continuation")
    types = _event_types(events)
    assert "tool_call" in types
    assert "tool_result" in types
    assert types[-1] == "done"
    chunks = [event for event in events if event["type"] == "text_chunk"]
    assert len(chunks) >= 2
    assert "tool_params" in chunks[0]["delta"]
    assert "导入完成" in _full_text(events)


def test_parse_structured_interaction_confirmed_flag():
    """取消与确认必须解析成不同状态；旧 XML 形态无 confirmed 字段时按已确认处理。"""
    import agent_loop

    confirmed = agent_loop.parse_structured_interaction(
        '<structured_interaction>{"type":"tool_params_confirmed","tool":"ImportCAD",'
        '"confirmed":true,"params":{"file":"wing.txt"}}</structured_interaction>'
    )
    assert confirmed["confirmed"] is True
    assert confirmed["cancelled"] is False
    assert confirmed["tool"] == "ImportCAD"

    cancelled = agent_loop.parse_structured_interaction(
        '<structured_interaction>{"type":"tool_params_confirmed","tool":"ImportCAD",'
        '"confirmed":false,"params":{"file":"wing.txt"}}</structured_interaction>'
    )
    assert cancelled["confirmed"] is False
    assert cancelled["cancelled"] is True

    legacy = agent_loop.parse_structured_interaction(
        '<structured_interaction><tool_params_confirmed tool="ImportCAD">'
        '<params>{"file":"wing.txt"}</params></tool_params_confirmed></structured_interaction>'
    )
    assert legacy["cancelled"] is False
    assert legacy["tool"] == "ImportCAD"

    assert agent_loop.parse_structured_interaction("导入模型并生成网格") == {}


@pytest.mark.asyncio
async def test_tool_params_cancelled_blocks_tool_execution(tmp_path, monkeypatch):
    """用户点「取消」后模型仍调用同一工具：必须被拦截，不能落到 MCP 执行。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    events = await _run_fixture("tool_params_cancelled")
    types = _event_types(events)
    assert types == load_fixture("tool_params_cancelled")["expected_event_types"]

    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "cancelled by user" in tool_result["result"]
    assert "成功导入" not in tool_result["result"]
    assert types[-1] == "done"


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("arguments", "parse_error"),
    [(None, "invalid JSON"), (["not", "object"], None), ("text", None)],
)
async def test_invalid_tool_arguments_are_not_executed(arguments, parse_error):
    from types import SimpleNamespace
    import agent_loop

    class InvalidRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            yield SimpleNamespace(
                type="tool_call_end", call_id="bad-1", name="ImportCAD",
                arguments=arguments, parse_error=parse_error,
            )

    class TrackingMcp(MockMcpBridge):
        def __init__(self):
            super().__init__({"ImportCAD": "unexpected"})
            self.calls = []

        async def call_tool(self, name, args):
            self.calls.append((name, args))
            return "unexpected"

    mcp = TrackingMcp()
    events = [event async for event in agent_loop.run_agent_loop(
        _make_session(None), "import", "base", _make_config(), mcp,
        MockContextManager(), MockSkillRegistry(), model_runtime=InvalidRuntime(),
    )]
    assert mcp.calls == []
    assert not any(event["type"] == "tool_call" for event in events)
    assert events[-1]["type"] == "error"
    assert events[-1]["retryable"] is False


@pytest.mark.asyncio
async def test_upstream_error_classification_is_forwarded():
    from types import SimpleNamespace
    import agent_loop

    class RateLimitedRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            yield SimpleNamespace(type="error", category="rate_limited",
                                  message="Upstream HTTP 429", retryable=True,
                                  status_code=429, retry_after=7.5)

    events = [event async for event in agent_loop._stream_runtime(
        RateLimitedRuntime(), "test/test-model", [], "base", []
    )]

    assert events == [{
        "type": "error", "category": "rate_limited", "message": "Upstream HTTP 429",
        "retryable": True, "status_code": 429, "retry_after": 7.5,
    }]


@pytest.mark.asyncio
async def test_permanent_errors_do_not_advertise_a_retry_window():
    from types import SimpleNamespace
    import agent_loop

    class BadRequestRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            yield SimpleNamespace(type="error", category="upstream_http",
                                  message="Upstream HTTP 400", retryable=False,
                                  status_code=400, retry_after=None)

    events = [event async for event in agent_loop._stream_runtime(
        BadRequestRuntime(), "test/test-model", [], "base", []
    )]

    assert events[-1]["retryable"] is False
    assert events[-1]["status_code"] == 400
    assert "retry_after" not in events[-1]


@pytest.mark.asyncio
async def test_transport_error_escapes_as_retryable():
    import httpx
    import agent_loop

    class BrokenRuntime:
        def context_window(self, model_key):
            return 32000

        async def stream(self, model_key, messages, tools=(), system_prompt=""):
            raise httpx.ConnectError("connection refused")
            yield  # 让 stream 保持异步生成器，异常在首次迭代时抛出

    events = [event async for event in agent_loop.run_agent_loop(
        _make_session(None), "hi", "base", _make_config(), MockMcpBridge({}),
        MockContextManager(), MockSkillRegistry(), model_runtime=BrokenRuntime(),
    )]

    assert events[-1]["type"] == "error"
    assert events[-1]["retryable"] is True  # 网络抖动不该被说成永久失败


# --------------------------------------------------------------------------
# 工具分组按需暴露（GetToolGroups + enable_tool_group + 直呼自动解锁）
# --------------------------------------------------------------------------

_REPLY = '已完成。\n```json\n{"options": [{"label": "继续", "value": "go"}]}\n```'

_GROUPS = [
    {"id": "query", "description": "只读查询", "tools": ["GetPointCount"]},
    {"id": "project", "description": "工程文件管理", "tools": ["OpenSpdFile"]},
    {"id": "generation", "description": "网格生成", "tools": ["UGSur"]},
]

_MCP_RESULTS = {
    # GetToolGroups 不属于任何分组，必须始终暴露（发现入口）
    "GetToolGroups": '{"groups": [], "default_enabled": ["query", "project"]}',
    "GetPointCount": "point_count=42",
    "OpenSpdFile": "opened",
    "UGSur": "sur mesh generated",
}


def _text(delta):
    return SimpleNamespace(type="text_delta", delta=delta)


def _tool(call_id, name, args):
    return SimpleNamespace(type="tool_call_end", call_id=call_id, name=name,
                           arguments=args, parse_error=None)


def _usage():
    return SimpleNamespace(type="usage", input_tokens=10, output_tokens=5,
                           total_tokens=15)


def _done():
    return SimpleNamespace(type="done", stop_reason="stop")


class _ScriptedRuntime:
    """按脚本逐轮产出事件，并记录每轮实际暴露给模型的工具名单。"""

    def __init__(self, script):
        self._script = list(script)
        self.exposed_tools = []
        self.system_prompts = []
        self.messages = []

    def context_window(self, model_key):
        return 32000

    async def stream(self, model_key, messages, tools=(), system_prompt=""):
        self.exposed_tools.append([getattr(t, "name", "") for t in tools])
        self.system_prompts.append(system_prompt)
        self.messages.append(messages)
        index = len(self.exposed_tools) - 1
        for event in (self._script[index] if index < len(self._script) else []):
            yield event


def _grouped_mcp(mock_mcp):
    return mock_mcp(_MCP_RESULTS, tool_groups=_GROUPS,
                    default_group_ids=["query", "project"])


async def _run_grouped(mcp, runtime, tmp_path, monkeypatch, message="生成面网格"):
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    import agent_loop

    return [event async for event in agent_loop.run_agent_loop(
        _make_session(None), message, "base", _make_config(), mcp,
        MockContextManager(), MockSkillRegistry(), model_runtime=runtime,
    )]


@pytest.mark.asyncio
async def test_tool_groups_default_exposure(tmp_path, monkeypatch, mock_mcp):
    """默认只暴露 query/project 两组 + 发现入口，其他分组的工具不进 schema。"""
    runtime = _ScriptedRuntime([[_text(_REPLY), _usage(), _done()]])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    exposed = runtime.exposed_tools[0]
    assert "GetToolGroups" in exposed          # 不属于任何分组，始终暴露
    assert "enable_tool_group" in exposed      # 内部启用工具
    assert "GetPointCount" in exposed          # query（默认组）
    assert "OpenSpdFile" in exposed            # project（默认组）
    assert "UGSur" not in exposed              # generation 未启用
    assert "read_skill" in exposed             # 技能内部工具不受分组影响
    # system prompt 里给出固定的工具发现说明（内容不随已启用分组变化，保护前缀缓存）
    assert "<tool_discovery>" in runtime.system_prompts[0]
    assert events[-1]["type"] == "done"


@pytest.mark.asyncio
async def test_enable_tool_group_exposes_group_next_turn(tmp_path, monkeypatch, mock_mcp):
    """enable_tool_group 是内置工具，启用后下一轮起该分组工具可直接调用。"""
    runtime = _ScriptedRuntime([
        [_tool("c1", "enable_tool_group", {"group_id": "generation"}), _usage(), _done()],
        [_tool("c2", "UGSur", {}), _usage(), _done()],
        [_text(_REPLY), _usage(), _done()],
    ])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    call = next(e for e in events if e["type"] == "tool_call")
    assert call["name"] == "enable_tool_group"
    assert call.get("internal") is True
    result = next(e for e in events if e["type"] == "tool_result")
    assert "已启用工具分组 generation" in result["result"]
    assert "UGSur" in result["result"]

    # 第一轮不含 UGSur，启用后的每一轮都含（本次消息内持续有效）
    assert "UGSur" not in runtime.exposed_tools[0]
    assert "UGSur" in runtime.exposed_tools[1]
    assert "UGSur" in runtime.exposed_tools[2]
    # 启用后直呼成功执行，没有被判为无效工具名
    ugsur = next(e for e in events if e["type"] == "tool_result" and e["name"] == "UGSur")
    assert ugsur["result"] == "sur mesh generated"
    assert not any(e["type"] == "error" for e in events)


@pytest.mark.asyncio
async def test_enable_unknown_tool_group_points_to_catalog(tmp_path, monkeypatch, mock_mcp):
    """未知 group_id 不报错，返回可选分组并提示先查 GetToolGroups。"""
    runtime = _ScriptedRuntime([
        [_tool("c1", "enable_tool_group", {"group_id": "nope"}), _usage(), _done()],
        [_text(_REPLY), _usage(), _done()],
    ])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    result = next(e for e in events if e["type"] == "tool_result")
    assert "未知分组 id" in result["result"]
    assert "GetToolGroups" in result["result"]
    assert "generation" in result["result"]
    # 未启用的分组仍然不暴露
    assert "UGSur" not in runtime.exposed_tools[-1]


@pytest.mark.asyncio
async def test_direct_call_of_hidden_tool_auto_unlocks_group(tmp_path, monkeypatch, mock_mcp):
    """模型不查目录直接直呼未暴露工具：自动启用其分组并放行执行。"""
    runtime = _ScriptedRuntime([
        [_tool("c1", "UGSur", {}), _usage(), _done()],
        [_text(_REPLY), _usage(), _done()],
    ])
    events = await _run_grouped(_grouped_mcp(mock_mcp), runtime, tmp_path, monkeypatch)

    result = next(e for e in events if e["type"] == "tool_result")
    assert result["name"] == "UGSur"
    assert result["result"] == "sur mesh generated"
    assert not any(e["type"] == "error" for e in events)
    # 没有触发"无效工具名"重选提醒
    assert "invalid_tool_name_reminder" not in str(runtime.messages[-1])
    # 解锁的分组在后续请求里保持暴露
    assert "UGSur" in runtime.exposed_tools[1]


@pytest.mark.asyncio
async def test_no_group_info_falls_back_to_full_exposure(tmp_path, monkeypatch, mock_mcp):
    """旧服务端没有分组信息时全量暴露，且不注入 enable_tool_group / 说明块。"""
    runtime = _ScriptedRuntime([[_text(_REPLY), _usage(), _done()]])
    events = await _run_grouped(mock_mcp(dict(_MCP_RESULTS)), runtime, tmp_path, monkeypatch)

    exposed = runtime.exposed_tools[0]
    assert "UGSur" in exposed
    assert "GetPointCount" in exposed
    assert "enable_tool_group" not in exposed
    assert "<tool_discovery>" not in runtime.system_prompts[0]
    assert events[-1]["type"] == "done"


@pytest.mark.asyncio
async def test_bridge_caches_tool_groups_from_server():
    """bridge 缓存服务端分组目录，并剔除目录里实际未注册的名字。"""
    import json as _json
    from mcp_bridge import McpBridge

    bridge = McpBridge("python server.py")
    tools = [SimpleNamespace(name=n)
             for n in ("GetToolGroups", "GetPointCount", "UGSur")]

    async def fake_call(name, args):
        assert name == "GetToolGroups"
        return _json.dumps({
            "groups": [
                {"id": "query", "description": "只读查询",
                 "tools": ["GetPointCount", "GhostTool"]},
                {"id": "generation", "description": "网格生成", "tools": ["UGSur"]},
            ],
            "default_enabled": ["query", "nope"],
        })

    bridge._call_tool_once = fake_call
    await bridge._load_tool_groups(tools)

    assert [g["id"] for g in bridge.tool_groups()] == ["query", "generation"]
    assert bridge.tool_groups()[0]["tools"] == ["GetPointCount"]  # GhostTool 未注册
    assert bridge.default_group_ids() == ["query"]                # nope 不存在
    assert bridge.group_for_tool("UGSur") == "generation"
    assert bridge.group_for_tool("GetToolGroups") is None


@pytest.mark.asyncio
async def test_bridge_without_group_info_stays_empty():
    """旧服务端没有 GetToolGroups 或拉取失败时，分组信息为空（调用方全量暴露）。"""
    from mcp_bridge import McpBridge

    bridge = McpBridge("python server.py")

    async def boom(name, args):
        raise RuntimeError("server has no GetToolGroups")

    bridge._call_tool_once = boom
    await bridge._load_tool_groups([SimpleNamespace(name="UGSur")])
    assert bridge.tool_groups() == []
    assert bridge.default_group_ids() == []

    await bridge._load_tool_groups([SimpleNamespace(name="GetToolGroups"),
                                    SimpleNamespace(name="UGSur")])
    assert bridge.tool_groups() == []
    assert bridge.group_for_tool("UGSur") is None


@pytest.mark.asyncio
async def test_cancel_persists_partial_reply_as_interrupted(tmp_path, monkeypatch):
    """停止本轮对话：已生成的部分文本要落盘并标 interrupted，否则刷新后凭空消失。

    CancelledError 继承自 BaseException，agent_loop 里原有的 except Exception 接不到，
    必须单独兜底，否则 text_acc 随协程栈一起丢掉、轨迹也停在 running。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    import agent_loop

    reached = asyncio.Event()

    async def stream_runtime(runtime, model_key, messages, system_prompt, tools):
        yield {"type": "text_chunk", "delta": "前一半"}
        reached.set()
        await asyncio.Event().wait()   # 模型还在生成，此时用户点了停止
        yield {"type": "text_chunk", "delta": "后一半"}

    monkeypatch.setattr(agent_loop, "_stream_runtime", stream_runtime)

    session = _make_session("cancel-mid-stream")
    stream = agent_loop.run_agent_loop(
        session=session, user_message="你好", base_system_prompt="base",
        config=_make_config(), mcp=MockMcpBridge({}), ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(), model_runtime=MockLLMClient([]),
    )

    async def consume():
        async for _ in stream:
            pass

    task = asyncio.create_task(consume())
    await reached.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    assistants = [m for m in session.messages if m["role"] == "assistant"]
    assert [m["content"] for m in assistants] == ["前一半"]
    assert assistants[0].get("interrupted") is True

    ends = [e for e in session.read_trajectory() if e["type"] == "traj_request_end"]
    assert [e["status"] for e in ends] == ["interrupted"]
    assert ends[0]["content"] == "前一半"


@pytest.mark.asyncio
async def test_cancel_before_first_token_writes_no_empty_reply(tmp_path, monkeypatch):
    """一个字都没生成就停止：不留空气泡，轨迹仍要收尾。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    import agent_loop

    reached = asyncio.Event()

    async def stream_runtime(runtime, model_key, messages, system_prompt, tools):
        reached.set()
        await asyncio.Event().wait()
        yield {"type": "text_chunk", "delta": "never"}

    monkeypatch.setattr(agent_loop, "_stream_runtime", stream_runtime)

    session = _make_session("cancel-before-token")
    stream = agent_loop.run_agent_loop(
        session=session, user_message="你好", base_system_prompt="base",
        config=_make_config(), mcp=MockMcpBridge({}), ctx_mgr=MockContextManager(),
        skill_registry=MockSkillRegistry(), model_runtime=MockLLMClient([]),
    )

    async def consume():
        async for _ in stream:
            pass

    task = asyncio.create_task(consume())
    await reached.wait()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    assert [m for m in session.messages if m["role"] == "assistant"] == []
    ends = [e for e in session.read_trajectory() if e["type"] == "traj_request_end"]
    assert [e["status"] for e in ends] == ["interrupted"]


def test_ask_user_tool_contract():
    """询问工具与 update_plan 同类：校验参数、限制选项数量、非法输入回错误而不抛异常。"""
    import agent_loop

    assert agent_loop.ASK_USER_TOOL_NAME == "ask_user_question"
    assert agent_loop.ASK_USER_TOOL_NAME in agent_loop._NON_EXEC_TOOLS

    payload, error = agent_loop.normalize_ask_user_args({
        "title": "下一步",
        "question": "接下来做什么？",
        "options": [
            {"label": "跑测试", "value": "run_tests", "description": "执行 WebUI 测试"},
            {"label": "提交改动", "value": "commit", "style": "primary"},
        ],
    })
    assert error is None
    assert payload["title"] == "下一步"
    assert payload["question"] == "接下来做什么？"
    assert payload["options"][0] == {"label": "跑测试", "description": "执行 WebUI 测试",
                                     "value": "run_tests", "style": ""}
    assert payload["options"][1]["style"] == "primary"

    # 标题缺省时回落默认值，选项超过上限按 6 项截断
    payload, error = agent_loop.normalize_ask_user_args({
        "question": "选一个",
        "options": [{"label": "选项%d" % index, "value": "v%d" % index} for index in range(8)],
    })
    assert error is None
    assert payload["title"] == "请选择下一步"
    assert len(payload["options"]) == 6

    # 非法参数只回错误文案，交给模型自纠
    for bad in (None, {},
                {"question": "选一个", "options": []},
                {"question": "选一个", "options": [{"label": "只有一个", "value": "a"}]},
                {"question": "选一个", "options": [{"label": "a", "value": "a"}, {"value": "b"}]},
                {"options": [{"label": "a", "value": "a"}, {"label": "b", "value": "b"}]}):
        payload, error = agent_loop.normalize_ask_user_args(bad)
        assert payload is None and error


def test_base_prompt_puts_ask_user_tool_first():
    """主提示词必须把询问工具立为主路径，正文 options 只作降级。

    这两条是"少花一半输入"的前提：工具优先才能让格式重试保持沉默。
    """
    import agent_loop

    prompt = agent_loop._load_base_prompt()
    assert prompt.count("ask_user_question") >= 3
    assert "优先调用 `ask_user_question`" in prompt
    assert "降级路径" in prompt
    # 降级的两个正当用途必须在文中写清，否则模型会随意退回正文 JSON
    assert "tool_params" in prompt and "工具不可用" in prompt


@pytest.mark.asyncio
async def test_injected_format_reminder_requires_tool_first(tmp_path, monkeypatch):
    """每轮注入的格式提醒必须与主提示词同口径：先工具，失败才兜底。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([_text_events("请确认下一步。")])
    await _run_direct(runtime, "帮我查看当前模型")
    system_prompt = runtime.system_prompts[0]
    assert "必须调用 ask_user_question 工具" in system_prompt
    assert "调用失败或被拒绝" in system_prompt
    # 旧的"未使用该工具时…兜底"措辞会把降级说成常规路径，必须消失
    assert "未使用该工具时" not in system_prompt


@pytest.mark.asyncio
async def test_ask_user_tool_ends_turn_with_single_request(tmp_path, monkeypatch):
    """模型走工具路径询问时：一次请求收尾，产出 options_offered 供前端渲染卡片。"""
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([_tool_call_events("ask_user_question", {
        "question": "后缘面处理已完成，接下来做什么？",
        "title": "下一步",
        "options": [
            {"label": "继续体网格块创建", "value": "continue_volume"},
            {"label": "导出当前网格", "value": "export", "style": "primary"},
        ],
    })])
    events = await _run_direct(runtime, "继续")
    types = _event_types(events)
    assert len(runtime.exposed_tools) == 1          # 工具路径不该再多发一次请求
    assert "options_offered" in types
    assert types[-1] == "done"
    offered = next(event for event in events if event["type"] == "options_offered")
    assert offered["question"] == "后缘面处理已完成，接下来做什么？"
    assert [item["label"] for item in offered["options"]] == ["继续体网格块创建", "导出当前网格"]
    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "已向用户发出选择" in tool_result["result"]


@pytest.mark.asyncio
async def test_ask_user_invalid_args_reach_the_model(tmp_path, monkeypatch):
    """参数非法时错误文案必须回到模型手里，不能被一次 MCP 调用结果覆盖。

    回归守卫：询问工具是内置工具，失败分支此前会继续落进 MCP 执行链，
    模型拿到的是 "Mock result / 未知工具" 而不是"options 至少需要 2 项"。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    runtime = _ScriptedRuntime([
        _tool_call_events("ask_user_question", {
            "question": "选一个",
            "options": [{"label": "只有一个", "value": "a"}],
        }),
        _text_events("```json\n{\"options\": [{\"label\": \"重试\", \"value\": \"retry\"}]}\n```"),
    ])
    events = await _run_direct(runtime, "继续", mcp=MockMcpBridge({}))
    tool_result = next(event for event in events if event["type"] == "tool_result")
    assert "options 至少需要 2 项" in tool_result["result"]
    assert "Mock result" not in tool_result["result"]
    assert "options_offered" not in _event_types(events)   # 没发出卡片，模型可以自纠
    assert _event_types(events)[-1] == "done"
    assert len(runtime.exposed_tools) == 2                 # 模型有机会重来一次


@pytest.mark.asyncio
async def test_auto_mode_blocks_ask_user_while_plan_open(tmp_path, monkeypatch):
    """auto 模式计划未收尾时不得停下问用户：工具必须被拒绝而不是挂起等待。"""
    import session as session_mod

    import uuid

    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)
    ledger = TaskLedger(str(uuid.uuid4()))
    ledger.update_plan("cad-mesh", "CAD 到 CFD 网格生成",
                       [{"id": "a", "title": "阶段A", "status": "in_progress"}])
    runtime = _ScriptedRuntime([_tool_call_events("ask_user_question", {
        "question": "继续吗？",
        "options": [{"label": "继续", "value": "go"}, {"label": "停止", "value": "stop"}],
    })])
    events = await _run_direct(runtime, "继续", ledger=ledger, interaction_mode="auto")
    refused = next(event for event in events if event["type"] == "tool_result")
    assert "ask_user_question 不可用" in refused["result"]
    assert "options_offered" not in _event_types(events)


@pytest.mark.asyncio
async def test_set_active_category_notifies_the_frontend(tmp_path, monkeypatch):
    """模型改了会话分类必须推 session_meta：否则 WebUI 的下拉框停在旧值。

    回归守卫两层：
    1. set_active_category 必须推 session_meta，否则界面读自己的 state 永不更新，
       下一轮请求还会带着旧分类把服务端刚设好的值覆盖回去；
    2. 分类只落在 Session 上。meta.json 的唯一写者是 save_session，而它每轮都用
       自己那份 meta 全量覆盖文件；agent_loop 另写一遍非但多余，还会在同一个回合
       里被抹掉。
    """
    import json

    import agent_loop
    import paths as paths_mod
    import session as session_mod

    monkeypatch.setattr(paths_mod, "SESSIONS_DIR", tmp_path)
    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    async def run(category, tag):
        session = _make_session(tag)
        (tmp_path / session.id).mkdir(parents=True, exist_ok=True)
        runtime = _ScriptedRuntime([
            _tool_call_events("set_active_category", {"category": category}),
            _text_events("好。"),
        ])
        return session, [event async for event in agent_loop.run_agent_loop(
            session, "这是导弹", "base", _make_config(), MockMcpBridge({}),
            MockContextManager(), MockSkillRegistry(), model_runtime=runtime,
            interaction_mode="manual",
        )]

    session, events = await run("missile", "category-meta")
    kinds = _event_types(events)
    assert "session_meta" in kinds
    # 事件要排在对应的工具结果之后，前端拿到时状态已经改好了
    assert kinds.index("session_meta") > kinds.index("tool_result")
    assert next(e for e in events if e["type"] == "session_meta")["category"] == "missile"
    # agent_loop 只改内存里的 Session，落盘交给唯一的写者 save_session
    # （app.py 在 tool_result 事件处调用它，写的是同一个 Session 对象）
    assert session.category == "missile"
    session_mod.save_session(session)
    saved = json.loads((tmp_path / session.id / "meta.json").read_text(encoding="utf-8"))
    assert saved["category"] == "missile"
    # 刷新页面/切会话时 GET /sessions/{id} 从 meta.json 恢复，必须能读回来
    assert session_mod.load_session(session.id).category == "missile"

    # 非法分类只把错误文案交给模型，不能推事件、也不能改会话分类
    invalid, refused = await run("submarine", "category-invalid")
    assert "session_meta" not in _event_types(refused)
    assert invalid.category == ""
    assert not (tmp_path / invalid.id / "meta.json").exists()


@pytest.mark.asyncio
async def test_large_failed_tool_result_is_still_recorded_as_failure(tmp_path, monkeypatch):
    """大结果落盘成 stub 之后，失败判定不能跟着丢。

    落盘发生在记账之前：persist 之后 result 只剩一行纯文本 stub，
    _tool_result_failed 解析不出 JSON 会返回 False，于是 status=error 的结果
    被记成成功——阈值 30000→8192 把这个窗口放大了三倍多。
    这条用例同时卡住两件事：历史里存的必须是 stub，台账里必须是失败。
    """
    import json

    import agent_loop
    import session as session_mod
    from context import ContextManager
    from task_ledger import TaskLedger

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    big_error = json.dumps({
        "status": "error",
        "error": "ExecAnisoDomain type=1 failed",
        "steps": {"step4_aniso_line_group": "failed"},
        "detail": "x" * 20000,
    })
    session = _make_session("large-failed-call")
    (tmp_path / session.id).mkdir(parents=True, exist_ok=True)
    ledger = TaskLedger(session.id)

    ctx = ContextManager()
    ctx._sessions_dir = tmp_path
    runtime = _ScriptedRuntime([
        _tool_call_events("GetModelParameters", {"component_ids": [1]}),
        _text_events("好。"),
    ])
    events = [event async for event in agent_loop.run_agent_loop(
        session, "查一下参数", "base", _make_config(),
        MockMcpBridge({"GetModelParameters": big_error}),
        ctx, MockSkillRegistry(), model_runtime=runtime, ledger=ledger,
        interaction_mode="auto",
    )]

    tool_msgs = [m for m in session.messages if m.get("role") == "tool"]
    assert tool_msgs, _event_types(events)
    assert "Result persisted to file" in tool_msgs[-1]["content"]
    assert ledger.calls[-1]["ok"] is False


@pytest.mark.asyncio
async def test_truncation_retry_limit_is_per_turn_not_process_global(tmp_path, monkeypatch):
    """连续截断的重试上限属于本轮对话：上一个会话用满，不得让下一个会话直接放弃。

    早先这个计数器挂在 run_agent_loop 的函数对象上，是进程级全局且从不重置，
    进程里累计两次截断之后所有会话都失去重试机会。
    """
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    first = await _run_fixture("truncation_retry_limit")
    assert _event_types(first).count("traj_request_start") == 3
    assert any(
        event["type"] == "error" and "连续截断超过 2 次" in event.get("message", "")
        for event in first
    )

    # 计数器若残留，第二次只会发一次请求就报「连续截断超过 2 次」
    second = await _run_fixture("truncation_retry_limit")
    assert _event_types(second).count("traj_request_start") == 3
    assert any(
        event["type"] == "error" and "连续截断超过 2 次" in event.get("message", "")
        for event in second
    )


@pytest.mark.asyncio
async def test_done_event_carries_context_usage(tmp_path, monkeypatch):
    """上下文用量圆环的数据源：done 事件要带上最近一次请求的 input 与模型窗口。

    注意不能是 _usage_totals["input"]（本轮多次请求之和），那是累计量。
    """
    import uuid
    import session as session_mod

    monkeypatch.setattr(session_mod, "SESSIONS_DIR", tmp_path)

    events = await _run_fixture("simple_chat")
    done = next(event for event in events if event["type"] == "done")
    assert done["context_used"] > 0
    assert done["context_window"] == 32000

    # 同一份数据也要落到 assistant 消息的 usage 里，切会话时才能恢复圆环
    sid = str(uuid.uuid5(uuid.NAMESPACE_URL, load_fixture("simple_chat")["input"]["session_id"]))
    messages = session_mod._read_jsonl(str(session_mod.session_dir(sid) / "messages.jsonl"))
    assistant = [m for m in messages if m.get("role") == "assistant"][-1]
    assert assistant["usage"]["context_used"] > 0

