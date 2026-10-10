from pathlib import Path

from fastapi.testclient import TestClient

from app import WEBUI_DIR, app


client = TestClient(app)


def test_ui_redirects_to_trailing_slash():
    response = client.get("/ui", follow_redirects=False)

    assert response.status_code in {307, 308}
    assert response.headers["location"] == "/ui/"


def test_ui_serves_index_and_static_assets():
    index = client.get("/ui/")
    css = client.get("/ui/style.css")
    javascript = client.get("/ui/app.js")

    assert index.status_code == 200
    assert 'style.css?v=' in index.text
    assert 'app.js?v=' in index.text
    assert css.status_code == 200
    assert "text/css" in css.headers["content-type"]
    assert javascript.status_code == 200
    assert "javascript" in javascript.headers["content-type"]


def test_webui_contains_existing_api_and_sse_contracts():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    for endpoint in (
        '"/health"', '"/config/models"', '"/skills"', '"/sessions"',
        '"/chat/stream"', '"/workflows/run"', 'tool-approvals',
    ):
        assert endpoint in script
    for event in (
        "text_chunk", "reasoning_chunk", "plan_updated", "tool_call",
        "tool_result", "tool_approval_required", "skill_loaded", "error", "done",
        "workflow_started", "workflow_step", "workflow_done",
    ):
        assert event in script
    # phase_plan 保留仅用于渲染旧会话历史（向后兼容，不再是协议）
    for interaction in ("options", "tool_params", "workflow", "phase_plan"):
        assert interaction in script


def test_webui_groups_tool_calls_in_collapsible_details():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 工具调用仍是逐条可折叠的 details，只是外层改挂到卡片底部的「工具调用」过程行
    assert 'document.createElement("details")' in script
    assert 'className = "tool-item"' in script
    assert 'processRail(parent, "tools")' in script
    assert 'data-proc="tools"' in script
    assert ".proc-row{" in stylesheet
    assert ".tool-item>summary{" in stylesheet


def test_webui_contains_model_settings_center_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    assert 'id="open-settings"' in index
    assert 'aria-label="打开设置"' in index
    assert index.index('id="open-settings"') > index.index('id="skill-select"')
    for tab in ('data-settings-tab="models"', 'data-settings-tab="skills"', 'data-settings-tab="mcp"'):
        assert tab in index
    assert 'class="provider-sidebar"' in index
    for endpoint in ('request("/config")', 'request("/config/providers/test"', 'request("/config/providers/models"'):
        assert endpoint in script
    for contract in ("discoveredModels", "testingProviderId", "readingProviderId", "revision", "validateSettings", "addModel", "manual-model-id", "data-model-field=\"api\""):
        assert contract in script


def test_webui_uses_config_response_contract_and_preserves_saved_credentials():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    assert 'const payload = {revision:state.settings.revision,config:state.settings.draft}' in script
    assert 'data.config && data.config.default_model' in script
    assert 'defaultModel = data.default_model || defaultModel' in script
    assert 'selectModel(models.value.default_model || "")' in script
    assert "default_index" not in script
    assert "refreshModels(data.models, data.default_model)" not in script
    assert 'return {provider:{...provider' in script
    assert 'provider.api_key !== "********"' in script
    assert 'provider.api_key === "********" ? ""' in script
    assert 'provider.api_key = ""' in script


def test_webui_uses_in_page_dialogs_instead_of_native_blocks():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    assert "confirm(" not in script
    assert "prompt(" not in script
    assert "alert(" not in script
    assert "function showDialog(" in script
    assert 'className = "modal-backdrop dialog-backdrop"' in script
    assert ".confirm-dialog{" in stylesheet
    assert ".dialog-actions{" in stylesheet


def test_webui_approval_card_renders_editable_params_from_schema():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 审批参数按工具 schema 展开成可编辑条目，与工具参数询问共用输入框上方的浮层卡
    assert "function coerceSchemaValue(" in script
    assert "event.schema && event.schema.properties" in script
    assert "function approvalEntries(" in script
    assert "function openApprovalOverlay(" in script
    assert "function queueApproval(" in script
    assert "else if (type === \"tool_approval_required\") { queueApproval(id, event); }" in script
    # 审批不再在对话流里单独成卡
    assert 'className = "approval-card"' not in script
    assert ".choice-card.approval" in stylesheet


def test_webui_model_listbox_has_static_provider_groups():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    assert 'role="combobox"' in index
    assert 'role="listbox"' in index
    assert 'className = "model-group"' in script
    assert 'setAttribute("role","group")' in script
    assert 'setAttribute("role","option")' in script
    # 分组标题展示供应商名称，而不是供应商 ID（ID 仅作无名称时的回退）
    group = script[script.index("function renderModelList"):script.index("function setConnection")]
    assert "items[0].provider_name || provider" in group
    assert "escapeHtml(label)" in group
    assert 'document.createElement("details")' not in script[script.index("function renderModelList"):script.index("function setConnection")]
    assert ".model-listbox{" in stylesheet
    assert ".model-group-label{" in stylesheet


def test_webui_mcp_panel_lists_backend_tools():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # MCP 面板从"即将支持"占位改为工具列表容器
    assert 'id="panel-mcp"' in index
    assert 'class="settings-panel mcp-panel hidden"' in index
    assert 'id="mcp-tools"' in index
    assert 'id="refresh-mcp"' in index
    assert 'id="mcp-count"' in index
    mcp_panel = index[index.index('id="panel-mcp"'):]
    mcp_panel = mcp_panel[: mcp_panel.index("</footer>")]
    assert "即将支持" not in mcp_panel
    # 前端拉取后端工具清单并按 tab 惰性加载
    for contract in ('"/mcp/tools"', '"/mcp/tools?refresh=1"', "function renderMcpTools(", "function loadMcpTools(",
                     "function schemaParams(", 'tab === "mcp" && !state.mcp.loaded'):
        assert contract in script
    assert ".mcp-panel{" in stylesheet
    assert ".mcp-tool{" in stylesheet


def test_webui_skills_panel_lists_registered_skills():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 技能面板从"即将支持"占位改为已注册技能列表容器
    assert 'id="panel-skills"' in index
    assert 'class="settings-panel skills-panel hidden"' in index
    assert 'id="skills-list"' in index
    assert 'id="refresh-skills"' in index
    assert 'id="skill-count"' in index
    skills_panel = index[index.index('id="panel-skills"'):]
    skills_panel = skills_panel[: skills_panel.index("</footer>")]
    assert "即将支持" not in skills_panel
    # 前端渲染 state.skills 并按 tab 惰性渲染，刷新按钮重新拉取 /skills
    for contract in ("function renderSkills(", "function loadSkills(", 'tab === "skills"',
                     'el.refreshSkills.onclick = () => loadSkills()', 'await request("/skills")'):
        assert contract in script
    assert ".skills-panel{" in stylesheet
    assert ".skill-card{" in stylesheet


def test_webui_history_render_matches_live_stream():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 历史渲染必须还原对话中持久化的思考过程
    assert "appendReasoning(item, message.reasoning_content)" in script
    body = script[script.index("function renderHistoryMessage"):]
    body = body[: body.index("function showWelcome")]
    render, close = body[: body.index("function finishHistoryTurn")], body[body.index("function finishHistoryTurn"):]
    # 工具调用必须先挂到消息节点，收尾统一交给 finishHistoryTurn。
    # 否则"无正文、仅工具调用"的消息（自动模式常见）会被当空气泡移除，
    # 后续 tool 结果找不到 call-id，退化为独立 TOOL RESULT 气泡
    assert "(message.tool_calls || []).forEach" in render
    assert "finishAssistant(" not in render
    assert "finishAssistant(turn, true)" in close
    assert "setBubbleUsage(turn" in close
    # 实时流里一轮对话只有一个气泡，而持久化会按迭代拆成多条 assistant/tool 消息，
    # 历史渲染必须按轮次合并，否则重开会话后变成"一条消息一个工具调用"
    assert "function renderHistory(messages)" in close
    assert 'message.role === "assistant" || message.role === "tool"' in close
    assert "renderHistoryMessage(message, turn)" in close
    assert "item.text += " in render
    assert "renderHistory(state.session.messages)" in script


def test_webui_surfaces_upstream_error_classification():
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 后端透传的分类字段必须真的被用上，而不是只剩一句 message
    for field in ("event.category", "event.retryable", "event.retry_after"):
        assert field in script
    assert "function streamFailure(" in script
    assert "function renderFailure(" in script
    assert 'retryable ? "RETRY" : "ERROR"' in script
    assert "throw new Error(event.message" not in script
    # 只有连不上后端才把顶栏标成离线，上游故障不该说"连接异常"
    assert "if (!error.stream) setConnection(" in script
    assert 'sendMessage(retry.message, retry.display)' in script


def test_webui_reconnects_background_stream_after_switching_sessions():
    """切走再切回（或刷新页面）时，后台仍在跑的回复必须能接回实时输出。"""
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    backend = (Path(__file__).resolve().parents[1] / "app.py").read_text(encoding="utf-8")

    # 服务端：本轮事件带序号存档，重连时整体回放并按序号去重，_seq 不外泄
    assert "async def _bg_put(bg: BackgroundSession, event: dict)" in backend
    assert 'stamped["_seq"] = bg.seq' in backend
    assert "def _sse_payload(event: dict) -> dict" in backend
    assert '@app.get("/sessions/{session_id}/background")' in backend

    # 前端：loadSession 末尾探测后台状态，活跃则重建本轮气泡并重连
    assert "function handleStreamEvent(id, type, event, assistant)" in script
    assert "async function reconnectStream(sessionId, info, turnTs)" in script
    assert "async function maybeReconnect(id)" in script
    assert "maybeReconnect(id);" in script
    assert 'request(`/sessions/${encodeURIComponent(id)}/background`)' in script
    reconnect = script[script.index("async function reconnectStream"):]
    # 重连要恢复"停止"按钮状态（按当前会话计算），并原样带回本轮消息标识，避免被当成新消息再跑一轮
    assert "message:info.last_message" in reconnect
    # 重连必须显式带 resume：后端只认这个标志区分「回放」和「新一轮」，否则停止后
    # 重发同一句话会被当成重连，上一轮内容灌进新气泡
    assert "resume:true" in reconnect
    assert "state.controllers.set(sessionId, controller)" in reconnect
    assert "syncComposer();" in reconnect
    # 用户主动停止过的会话不自动重连，否则切回时又粘回后台流
    assert 'const stopped = error.name === "AbortError";' in script
    assert 'setStatus(sessionId, stopped ? "stopped" : "error");' in script
    assert 'state.status.get(id) === "stopped"' in script
    # 停止标记实时流与重建历史共用一套逻辑，避免实时多一条「已停止」提示、刷新后又消失
    assert "function markStopped(item)" in script
    assert "if (message.interrupted) markStopped(item);" in script
    assert "markStopped(assistant)" in script


def test_webui_per_session_stream_state_and_badges():
    """每个会话有独立的流状态与列表徽标，发送按钮只跟随当前所在会话。"""
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")
    backend = (Path(__file__).resolve().parents[1] / "app.py").read_text(encoding="utf-8")
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")

    # 全局单槽 busy/controller/stream 已移除，流状态按会话隔离成 Map
    assert "state.busy" not in script
    assert "setBusy(" not in script
    assert "controllers: new Map(), streams: new Map(), status: new Map()" in script
    # 发送按钮的"停止/发送"形态只根据当前会话计算
    assert "function isBusy() { const id = currentId(); return Boolean(id && state.controllers.has(id)); }" in script
    assert "function syncComposer()" in script
    # 点停止：断开 SSE 之外还要真正取消后端任务，否则会继续消耗 token
    assert "function stopSession(id)" in script
    assert "encodeURIComponent(id)}/cancel" in script
    assert '@app.post("/sessions/{session_id}/cancel")' in backend
    assert "bg.task.cancel()" in backend
    # 会话列表五态徽标；刷新页面后靠服务端 active/waiting 字段兜底标出"进行中/待确认"
    assert 'const STATUS_TEXT = {running:"进行中", done:"已完成", stopped:"已停止", error:"异常", waiting:"待确认"}' in script
    assert '<i class="session-badge ${status}">' in script
    assert 'session.active ? "running"' in script
    assert 'item["active"]' in backend
    for cls in (".session-badge.running", ".session-badge.done", ".session-badge.stopped", ".session-badge.error", ".session-badge.waiting"):
        assert cls in stylesheet
    assert "@keyframes badgePulse" in stylesheet
    # 待确认：选项卡片/工具参数面板收尾或审批浮层挂起时进入 waiting
    assert 'session.waiting ? "waiting"' in script
    assert "message.awaitingInput" in script
    assert "stream.awaiting = true" in script
    assert 'setStatus(id, "waiting")' in script
    assert "function queueApproval(id, event)" in script
    assert 'item["waiting"]' in backend
    # 缓存版本随本次前端改动升级
    assert "app.js?v=75" in index
    assert "style.css?v=58" in index


def test_webui_storage_issue_surfacing_contract():
    """会话存储读不出来时界面必须说出来，不能和「没有会话」长得一样。"""
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    backend = (Path(__file__).resolve().parents[1] / "app.py").read_text(encoding="utf-8")

    # 后端把异常随列表一起返回
    assert '"storage_issues": issues' in backend
    assert "from session import (" in backend
    assert "storage_issues," in backend
    # 前端有独立提示函数，且两条取列表的路径都调用它
    assert "function warnStorageIssues(issues)" in script
    assert "warnStorageIssues(data.storage_issues)" in script
    assert "warnStorageIssues(sessions.value.storage_issues)" in script
    # 同一句话只提示一次，避免每次刷新都盖掉别的提示
    assert "const warnedStorageIssues = new Set()" in script
    assert "warnedStorageIssues.has(issue)" in script


def test_webui_voice_input_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 语音按钮位于发送按钮之前，处于同一输入区
    assert 'id="voice-btn"' in index
    assert 'class="voice"' in index
    assert 'aria-label="语音输入"' in index
    assert index.index('id="voice-btn"') < index.index('id="send"')
    # 缓存版本随本次前端改动升级
    assert "style.css?v=58" in index
    assert "app.js?v=75" in index

    # 录音 → 浏览器端 WAV 编码 → POST /asr → 回填，全链路契约
    for contract in (
        "getUserMedia", "createScriptProcessor", "function encodeWav(",
        "function startRecording(", "function stopRecording(",
        'fetch("/asr"', 'request("/asr/health")', "FormData",
        "function appendTranscript(", "function toggleVoice(",
    ):
        assert contract in script

    # 按钮四态样式（常态/录音脉冲/加载 spinner/禁用）
    for rule in (".voice{", ".voice.recording{", ".voice.loading{", ".voice:disabled{",
                 "@keyframes voicePulse", "@keyframes voiceSpin"):
        assert rule in stylesheet


def test_webui_attachment_drag_and_upload_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 输入区附件控件：选择按钮、隐藏 file input、芯片条、拖拽遮罩
    assert 'id="attach-btn"' in index
    assert 'id="file-input"' in index
    assert 'id="attach-bar"' in index
    assert 'id="drop-overlay"' in index
    assert 'type="file" multiple' in index
    # 附件按钮位于语音/发送按钮之前，同属输入区
    assert index.index('id="attach-btn"') < index.index('id="voice-btn"')

    # 上传 → 芯片 → 随消息发送 → 历史渲染 全链路契约
    for contract in (
        '"/upload?name="', "function uploadFile(", "function addFiles(",
        "function renderAttachments(", "function renderMessageAttachments(",
        "attachments:sentAttachments", 'type === "notice"',
        "function dragHasFiles(", 'addEventListener("dragenter"', 'addEventListener("drop"',
        "attachments: [], uploading: 0",
    ):
        assert contract in script

    # 附件相关样式（芯片条、拖拽遮罩、气泡内附件）
    for rule in (".attach-bar{", ".attach-chip{", ".drop-overlay{", ".bubble-attachments{"):
        assert rule in stylesheet


def test_webui_choice_card_contract():
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 待作答的询问浮在输入框上方、把输入框盖住；卡片不落进对话流
    assert '<div id="choice-overlay" class="choice-overlay hidden"' in index
    assert ".choice-overlay{position:absolute;" in stylesheet
    assert ".choice-overlay.open{" in stylesheet
    assert ".choice-overlay .choice-card{width:100%;margin:0}" in stylesheet
    # 面板展示期间彻底藏掉输入框（卡片圆角更大，不藏会露出输入框圆角与投影）
    assert ".composer.choice-open .input-wrap{opacity:0;visibility:hidden" in stylesheet
    assert 'if (el.composer) el.composer.classList.add("choice-open");' in script
    assert 'if (el.composer) el.composer.classList.remove("choice-open");' in script
    assert "function openChoiceOverlay(payload)" in script
    assert "function closeChoiceOverlay()" in script
    assert "requestAnimationFrame" in script
    assert 'host.classList.add("open")' in script
    assert 'host.classList.remove("open");' in script

    # 模型询问：ask_user_question 工具事件与旧 options 文本块都走同一套浮层
    assert 'const ASK_USER_TOOL = "ask_user_question"' in script
    assert "function renderChoiceCard(payload, parent)" in script
    assert 'else if (type === "options_offered") { assistant.pendingAsk = event; }' in script
    assert 'if (!deferred && message.pendingAsk && message.node.isConnected) openChoiceOverlay(message.pendingAsk);' in script
    assert "if (last && last.awaitingInput && last.pendingAsk) openChoiceOverlay(last.pendingAsk);" in script
    assert "finishAssistant(turn, true);" in script
    # 询问类调用不落成工具条目：实时流与历史重放保持一致
    assert 'if (event.name !== ASK_USER_TOOL) renderToolCall(event,assistant.node);' in script
    assert 'if (name === ASK_USER_TOOL) { item.pendingAsk = args; return; }' in script
    assert 'if (message.tool_name === ASK_USER_TOOL) return turn;' in script
    # 交互契约：普通选项点一下即确认；「其他」先放开输入框、写完再点提交答案；折叠、关闭、Esc 收起
    assert 'entries.push({item, option: null, label: "其他", other: true});' in script
    assert "const activate = () => { if (entry.other) openAnswer(index); else { pick(index); submit(); } };" in script
    assert 'placeholder="点击「其他」后在此输入答案"' in script
    assert '<button class="choice-submit" type="button" disabled>提交答案</button>' in script
    assert "submitBtn.onclick = submit;" in script
    assert 'if (!text) { input.classList.add("invalid"); input.focus(); return; }' in script
    assert 'input.onkeydown = event => { if (event.key === "Enter") { event.preventDefault(); submit(); } };' in script
    assert 'card.classList.toggle("collapsed")' in script
    assert '$(".choice-close", card).onclick = event => { event.stopPropagation(); closeChoiceOverlay(); };' in script
    assert "按 Esc 取消" in script
    # Esc 只在浮层打开时响应，且让位给设置弹窗
    assert 'if (!el.choiceOverlay || el.choiceOverlay.classList.contains("hidden")) return;' in script
    # 卡片样式（三主题共用变量）
    for rule in (".choice-card{", ".choice-head{", ".choice-close{", ".choice-item{",
                 ".choice-item.selected .choice-dot:after{", ".choice-input:focus{",
                 ".choice-input.invalid{", ".choice-submit{", ".choice-hint{"):
        assert rule in stylesheet


def test_webui_turn_rail_navigates_conversation_turns():
    """对话界面最左侧竖排白点：一轮一个点，鼠标移入展开整轮列表，点击行/点跳转，当前轮高亮。"""
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 轨道与轮次面板是对话区旁的独立节点，面板不放在轨道内（轨道可滚动，放进去会被 overflow 裁掉）
    assert 'id="turn-rail"' in index
    assert 'id="turn-rail-panel"' in index
    assert 'id="turn-rail-list"' in index
    assert index.index('id="turn-rail-panel"') > index.index('id="turn-rail"')

    # 轮次以用户消息为界：一个点对应一条 user 消息，列表一行摘要同一轮
    assert 'function renderTurnRail()' in script
    assert 'el.messages.querySelectorAll(".message.user")' in script
    # 消息区增删统一走 MutationObserver + 下一帧合并刷新，点集未变不重建
    assert "new MutationObserver(syncTurnRail).observe(el.messages, {childList: true});" in script
    assert "function syncTurnRail()" in script
    # 移入轨道展开整轮列表、点击定位到该轮、滚动时重算当前轮高亮
    assert "function showTurnRailPanel(" in script
    assert "function hideTurnRailPanel()" in script
    assert "function jumpToTurn(" in script
    assert 'el.turnRail.addEventListener("mouseenter", showTurnRailPanel);' in script
    assert 'node.scrollIntoView({block: "start", behavior: "smooth"})' in script
    assert "function updateTurnRailActive()" in script
    assert "updateTurnRailActive(); }, {passive: true});" in script
    assert "syncTurnRail();" in script[script.index("function switchViewTab"):]

    # 固定在最左边缘垂直居中，点/当前点的样式 + 右侧列表行样式
    for rule in (".turn-rail{position:fixed;left:0;top:50%;transform:translateY(-50%)",
                 ".turn-rail-dot{", ".turn-rail-dot:before{", ".turn-rail-dot.active:before{",
                 ".turn-rail-panel{position:fixed;", ".turn-rail-panel.show{",
                 ".turn-rail-row{", ".turn-rail-index{", ".turn-rail-row.active .turn-rail-index{"):
        assert rule in stylesheet


def test_webui_usage_panel_contract():
    """设置中心第四个 Tab「用量」：供应商/模型维度、按天/按小时、日历选区间、三图 + 明细表。"""
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    charts = (Path(WEBUI_DIR) / "charts.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")
    backend_dir = Path(__file__).resolve().parents[1]
    stats = (backend_dir / "usage_stats.py").read_text(encoding="utf-8")

    # Tab 排在 MCP 之后，面板与筛选控件齐备
    assert 'data-settings-tab="usage"' in index
    assert 'id="panel-usage"' in index
    assert index.index('data-settings-tab="usage"') > index.index('data-settings-tab="mcp"')
    for anchor in ('id="usage-group"', 'id="usage-provider"', 'id="usage-model"', 'id="usage-granularity"',
                   'id="usage-range"', 'id="usage-range-panel"', 'id="usage-calendar"', 'id="usage-overview"',
                   'id="usage-line"', 'id="usage-pie"', 'id="usage-bar"', 'id="usage-table"'):
        assert anchor in index
    # 筛选顺序：分组 → 供应商 → 模型 → 粒度 → 日期
    assert index.index('id="usage-group"') < index.index('id="usage-provider"')
    assert index.index('id="usage-provider"') < index.index('id="usage-model"')
    assert index.index('id="usage-model"') < index.index('id="usage-granularity"')
    # 日历面板挂在筛选行下而不是日期触发器内：它宽约 560，
    # 跟着触发器左缘向右展开会顶出弹窗右缘被裁掉
    assert index.index('id="usage-range-panel"') > index.index('id="usage-refresh"')
    assert index.index('id="usage-range-panel"') < index.index('id="usage-status"')
    assert ".usage-filters{position:relative;" in stylesheet
    assert ".usage-range-panel{position:absolute;z-index:26;top:calc(100% + 5px);right:0;left:auto;max-width:100%;" in stylesheet
    # 图表模块先于 app.js 加载，且不引任何外部资源（内网离线）
    assert 'charts.js?v=' in index
    assert index.index('charts.js?v=') < index.index('app.js?v=')
    assert '<script src="http' not in index

    # 接口与聚合口径
    assert "/usage/stats" in script
    assert "/usage/stats" in (backend_dir / "app.py").read_text(encoding="utf-8")
    assert "function loadUsageStats(" in script
    assert "function usageRollupDays(" in script
    assert "function usageDrilled(" in script
    assert 'if (tab === "usage")' in script
    # 供应商/模型筛选项只列「有调用记录」的模型：candidates 按时间窗算出、不随筛选缩水。
    # 列出从没调用过的配置模型只会让用户选到空结果。
    assert "function usageSyncCatalog(" in script
    assert "data.candidates" in script
    assert '"candidates"' in stats
    assert "function usageProviderName(" in script
    assert "function usageModelName(" in script
    assert "&provider=" in script
    assert '"provider"' in stats
    # 配置目录只用于名称映射，不再作为选项来源
    assert "(state.models || []).filter(item => item.enabled !== false)" in script
    # 选中项在当前时间窗无记录时仍保留下拉里，不静默清空
    assert "usage.modelOptions.push({value: usage.model" in script
    # 结果缓存有上限：key 精确到分钟，跨分钟后旧条目再也命中不了，不设上限会一直堆积
    assert "USAGE_CACHE_LIMIT" in script
    assert "if (Object.keys(usage.cache).length >= USAGE_CACHE_LIMIT) usage.cache = {};" in script
    # 命中缓存时也要作废在飞请求，否则它返回时会把这份缓存结果覆盖成上一个筛选的数据
    assert "usage.token += 1;" in script
    # 分组只决定饼图与明细表的拆分维度，不再置灰模型下拉、也不再清空已选筛选
    assert "el.usageModel.disabled" not in script
    for label in ("最近 12 小时", "最近 24 小时", "最近 3 天", "最近 7 天", "最近 30 天"):
        assert label in script
    for label in ("模型用量", "供应商用量", "按小时", "按天"):
        assert label in script
    # 12/24 小时档在按天下只剩一两个点，选择后自动切到按小时
    assert "if (usagePreset(id).hours) usage.granularity = \"hour\";" in script
    # 单个模型下饼图退化为该模型的 token 构成，避免只剩一个满圆
    assert "缓存读" in script and "缓存写" in script
    # 缓存命中率口径不假设 input 与 cache_read 的包含关系
    assert "totals.cache_read / base" in script

    # 图表模块自带折线/饼/柱与时间轴缩放，零依赖
    for name in ("function line(", "function pie(", "function bar(", "function bindTimeZoom(",
                 "function clampView(", "function arcPath("):
        assert name in charts
    assert "https://" not in charts and "http://" not in charts.replace("http://www.w3.org/2000/svg", "")
    assert 'svg.addEventListener("dblclick"' in charts
    assert 'svg.addEventListener("wheel"' in charts
    assert 'svg.addEventListener("mousedown"' in charts

    # 用量一律以 M 为单位：同一页混用 K 与 M 无法横向比较，故不再有 K 档
    assert "function formatMillions(" in charts
    assert "/ 102.4" not in charts
    assert 'return "0M";' in charts
    # 不足 1M 时按数量级补足小数位，否则 8.8K 会被压成 0.00M，看起来像没有消耗
    assert "Math.min(8, 1 - Math.floor(Math.log(abs) / Math.LN10))" in charts
    # 用量面板统一走 M 格式化
    for call in ("Charts.formatMillions(totals.total)", "Charts.formatMillions(totals.input)",
                 "Charts.formatMillions(totals.output)", "Charts.formatMillions(row[item.key])"):
        assert call in script
    # 模型设置的上下文窗口仍用 K/M 自适应：128K 换成 0.13M 反而难读
    assert "function formatTokens(value) { return value >= 1048576" in script
    assert "formatTokens(totals" not in script and "formatTokens(row[item" not in script
    # 轮次是计数不是用量：只给 token 列打 tokens 标记，否则 33 轮会写成 0.000033M
    assert '{key:"turns",label:"轮次",numeric:true},' in script
    assert 'escapeHtml(item.tokens ? Charts.formatMillions(row[item.key]) : row[item.key])' in script

    for rule in (".usage-panel{", ".usage-filters{", ".usage-select{", ".usage-listbox{",
                 ".usage-range-panel{", ".usage-cal-day{", ".usage-overview{", ".usage-stat{",
                 ".usage-table{", ".chart-legend{", ".chart-tip{", ".chart-empty{", ".pie-legend{"):
        assert rule in stylesheet


def test_webui_serves_charts_module():
    response = client.get("/ui/charts.js")

    assert response.status_code == 200
    assert "javascript" in response.headers["content-type"]


def test_webui_clears_stream_state_and_avoids_duplicate_streams():
    """清空会话要像删除一样丢弃前端流状态，且同会话不允许开出第二条 SSE。

    回归的 bug：clearSession 只请求后端、不清理 controllers/streams/stashed，
    被切走会话的暂存 DOM 会在切回时被 restoreView 挂回，看起来「清空没生效」。
    """
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 统一的流状态清理：中断在跑的流并清掉 controller/stream/stash 三张表
    assert "function dropStreamState(id)" in script
    assert "state.stashed.delete(id);" in script

    clear = script[script.index("async function clearSession(session)"):]
    clear = clear[:clear.index("async function deleteSession")]
    assert "dropStreamState(session.id);" in clear
    # 清空后要抹掉中断流异步写入的 stopped 徽标
    assert "state.status.delete(session.id);" in clear
    # 丢弃暂存 DOM 必须在刷新视图之前，否则切回时挂回的是清空前的旧内容
    assert clear.index("dropStreamState(session.id);") < clear.index("await loadSession(session.id);")

    delete = script[script.index("async function deleteSession(session)"):]
    assert "dropStreamState(session.id);" in delete

    # error 事件抛出前必须主动断开，避免响应体不被读完、连接挂到 GC
    consume = script[script.index("async function consumeSse(response, onEvent, controller)"):]
    consume = consume[:consume.index("function handleStreamEvent")]
    assert "controller.abort();" in consume
    assert "reader.cancel();" in consume

    # 同会话双流防护：发送与重连在锁定会话后都要再确认一次没有在跑的流
    guard = "if (state.controllers.has(sessionId) || state.streams.has(sessionId)) return;"
    send = script[script.index("async function sendMessage("):]
    send = send[:send.index("async function reconnectStream")]
    assert guard in send
    reconnect = script[script.index("async function reconnectStream"):]
    reconnect = reconnect[:reconnect.index("async function maybeReconnect")]
    assert guard in reconnect


def test_webui_phase_panel_visibility_contract():
    """计划窗口只有一条可见性规则：有未收尾阶段才显示，全部收尾即收起。

    回归守卫：切会话时若只给面板加 hidden，上一条会话的旧 DOM 与旧
    state.phasePlan 会留在页面里，之后切回对话页签时会按"看起来还有计划"
    重新亮出来，表现为切换对话后仍显示上一个流程面板。
    """
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 收起必须同时清 DOM 与状态，否则残留会被后续判断当成"还有计划"
    hide = script[script.index("function hidePhasePanel()"):]
    hide = hide[:hide.index("function renderPhase(")]
    assert 'el.phasePanel.classList.add("hidden");' in hide
    assert 'el.phasePanel.innerHTML = "";' in hide
    assert "state.phasePlan = null;" in hide

    # 完成即收起的规则收在渲染口，实时更新/切会话/回放历史都绕不过去
    render = script[script.index("function renderPhase("):]
    render = render[:render.index("// 过程区挂在卡底")]
    assert "if (planComplete(phase)) { hidePhasePanel(); return; }" in render

    # 切会话走同一个收起函数
    load = script[script.index("async function loadSession("):]
    load = load[:load.index("function renderSessions(")]
    assert 'el.messages.innerHTML = ""; hidePhasePanel();' in load
    assert 'el.phasePanel.classList.add("hidden")' not in load

    # 回对话页签按状态判断，不再按残留 DOM 判断
    tab = script[script.index("function switchViewTab("):]
    tab = tab[:tab.index("// 时间轴命中检测")]
    assert ('if (state.phasePlan && !planComplete(state.phasePlan)) '
            'el.phasePanel.classList.remove("hidden");') in tab
    assert "el.phasePanel.innerHTML.trim()" not in tab

    # 本轮结束时的收起同样走同一函数
    assert "if (visible && planComplete(state.phasePlan)) hidePhasePanel();" in script


def test_webui_mode_dropdown_contract():
    """交互模式由分段按钮改为下拉：每项一行标题 + 一行小字说明，选中项打勾。

    值仍以 state.mode 发往 /chat/stream 的 interaction_mode，契约不变。
    """
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 结构：触发器 + 列表，复用模型下拉的控件族；分段按钮与旧样式不得残留
    assert 'class="model-control mode-control"' in index
    assert 'id="mode-trigger"' in index and 'id="mode-label"' in index and 'id="mode-listbox"' in index
    assert "data-mode" not in index
    assert "mode-switch" not in index and "mode-switch" not in stylesheet

    # 选项：标题 + 一行说明 + 打勾，且仅选中项可见
    assert '{value:"manual", title:"手动", hint:' in script
    assert '{value:"auto", title:"自动", hint:' in script
    assert 'class="mode-option-text"><strong>' in script
    assert 'class="mode-check" aria-hidden="true">✓' in script
    assert ".mode-option[aria-selected=\"true\"] .mode-check{visibility:visible}" in stylesheet
    assert ".mode-option-text small{" in stylesheet and "display:block" in stylesheet
    assert ".mode-listbox{" in stylesheet

    # 行为：选择写回 state.mode，发送时作为 interaction_mode；三个下拉互斥
    assert "function selectMode(value)" in script
    assert 'option.setAttribute("aria-selected", String(state.mode === item.value));' in script
    assert "interaction_mode:state.mode" in script
    assert "el.modeTrigger.onclick = () => state.modeListOpen ? closeModeList() : openModeList();" in script
    assert "function openModeList() { closeModelList(); closeSlashMenu();" in script
    assert 'if (state.modeListOpen && !inside(".mode-control")) closeModeList();' in script
    assert "closeModelList(); closeSlashMenu(); closeModeList(); closeSessions();" in script


def test_webui_composer_dropdowns_are_hover_driven():
    """输入框的下拉统一为悬停展开、点击开合，且触发按钮不再带箭头。

    技能已改为输入框里的 "/" 面板，不再有第三个下拉（见斜杠面板用例）。
    """
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")

    # 触发按钮内不得再有箭头；其它下拉（主题、会话、用量）保持原样
    for trigger in ('id="mode-trigger"', 'id="model-trigger"'):
        start = index.index(trigger)
        assert "chevron" not in index[start:index.index("</button>", start)]
    assert 'id="theme-trigger"' in index
    theme = index[index.index('id="theme-trigger"'):]
    assert "chevron" in theme[:theme.index("</button>")]

    # 悬停绑定：进入即展开、移开延迟收起
    assert "function bindHoverDropdown(root, open, close)" in script
    assert 'root.addEventListener("mouseenter", () => { cancel(); open(); });' in script
    assert "}, HOVER_CLOSE_DELAY);" in script
    assert 'bindHoverDropdown(el.modelTrigger.closest(".model-control"), openModelList, closeModelList);' in script
    assert 'bindHoverDropdown(el.modeTrigger.closest(".mode-control"), openModeList, closeModeList);' in script
    assert "bindHoverDropdown(el.skillTrigger" not in script

    # 点击仍是开合切换
    assert ("el.modelTrigger.onclick = () => state.modelListOpen ? closeModelList() : openModelList();"
            in script)
    assert "el.modeTrigger.onclick = () => state.modeListOpen ? closeModeList() : openModeList();" in script

    # 互斥收在各自的 open 里：悬停扫过一排控件时不会同时开着两个面板
    assert "function openModelList() { closeModeList(); closeSlashMenu();" in script
    assert "function openModeList() { closeModelList(); closeSlashMenu();" in script


def test_webui_dropdown_check_sits_at_right():
    """下拉的选中勾排在文字右侧。

    模型列表原先把勾放在最前，配 18px 定宽首列；改成尾部列后，
    勾由 justify-self 贴右，行的左边距不再被占位列撑开。
    """
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 模型列表：文字在前，勾在后
    assert ('<strong>${escapeHtml(modelName(item))}</strong>'
            '<small>${escapeHtml(item.model_id || item.id || key)}</small>'
            '<span class="model-check">') in script
    assert 'option.innerHTML = `<span class="model-check">' not in script

    # 模式下拉本来就是文字在前
    assert '<span class="mode-option-text"><strong>' in script

    # 输入框下拉的定宽首列已取消，勾挪到最后一列并等宽占位（避免选中/未选中行抖动）；
    # 桌面端模型 id 是可见的第三列，网格必须保留三列，否则勾会换行
    assert "grid-template-columns:18px minmax(0,1fr) auto;grid-gap:7px" not in stylesheet
    assert "grid-template-columns:minmax(0,1fr) auto auto" in stylesheet
    assert ".model-candidates button{width:100%;display:grid;grid-template-columns:18px" in stylesheet
    assert ".model-check{min-width:12px;justify-self:end;color:var(--cyan)}" in stylesheet


def test_webui_skill_slash_menu_contract():
    """技能选择从下拉按钮改成输入框的 "/" 面板。"""
    index = (Path(WEBUI_DIR) / "index.html").read_text(encoding="utf-8")
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    stylesheet = (Path(WEBUI_DIR) / "style.css").read_text(encoding="utf-8")

    # 旧的技能下拉已移除，保留隐藏输入作为取值容器，并新增面板与已选标签
    assert "skill-trigger" not in index and "skill-listbox" not in index
    assert 'id="skill-select" type="hidden"' in index
    assert 'id="slash-menu"' in index and 'id="skill-chip"' in index
    assert 'id="slash-menu" class="slash-menu hidden"' in index
    assert "skill-control" not in stylesheet and "skill-control" not in script
    # 输入框提示语提示可用 "/" 选技能
    assert "输入 / 选择技能" in index

    # 触发与过滤：只在输入以 "/" 开头时展开，输入内容作为过滤词
    assert "function slashQuery()" in script and "if (!text.startsWith(\"/\")) return null;" in script
    assert "function syncSlashMenu()" in script
    assert "if (slashQuery() === null) { closeSlashMenu(); return; }" in script
    assert "function slashMatch(text, query)" in script

    # 指令分组：模型（切到可搜索的模型面板）与导出对话（直接调后端导出）；指令排在技能之前
    assert '{id: "model", name: "模型", description: "选择本次会话使用的模型"}' in script
    assert '{id: "export", name: "导出对话", description: "把当前会话导出为 Markdown 文件"}' in script
    assert 'return [{label: "指令", items: commands}, {label: "技能", items: skills}];' in script
    # 供应商发现来的模型不进选择范围：斜杠面板与模型下拉都只列配置里写过的，
# 但"查名字"仍用全量目录，避免既有会话选中的发现模型显示成"未配置"
    assert 'function configuredModels() { return visibleModels().filter(item => item.status !== "discovered"); }' in script
    assert "const groups = new Map(); configuredModels().forEach(item =>" in script
    assert "return [{label: \"模型\", items: configuredModels().map(item => ({" in script
    assert '没有已配置的模型' in script
    assert "state.models.filter(item => item.status === \"discovered\")" not in script
    assert "function openSlashModelPicker()" in script
    assert 'search.placeholder = "搜索模型...";' in script
    # 行只在打开/切模式时建一次，按键只切可见性：避免打字过程中丢按键
    assert "let slashRowCache = [];" in script
    assert "function applySlashFilter()" in script
    assert "if (state.slashOpen && slashRowCache.length) { applySlashFilter(); return; }" in script
    assert "function visibleSlashRows()" in script
    assert "function runSlashExport()" in script
    assert "`/sessions/${encodeURIComponent(sessionId)}/export`" in script
    assert 'state.slashMode = "model";' in script
    assert 'state.slashMode = "root";' in script
    # 模型面板选中后收起面板、清掉正文的 "/"，焦点回到输入框
    assert "function finishSlashModelPick()" in script
    assert 'if (el.input.value.startsWith("/")) { el.input.value = ""; autoGrowInput(); }' in script

    # 行为：键盘上下选择、Enter 确认、Escape 收起；选定后清掉 "/xx" 并写入隐藏输入
    assert "function handleSlashKeys(event)" in script
    assert 'if (event.key === "Escape") { event.preventDefault(); closeSlashMenu(); return true; }' in script
    assert 'if (event.key === "Enter") {' in script
    assert "function selectSkill(id)" in script
    assert 'if (el.input.value.startsWith("/")) { el.input.value = ""; autoGrowInput(); }' in script
    assert "function renderSkillChip()" in script
    assert 'el.skillChip.onclick = () => { selectSkill(""); el.input.focus(); };' in script

    # 接线：输入事件同步面板、回车先给面板处理；点击外部与 Esc 都能收起
    assert "el.input.oninput = () => { autoGrowInput(); updateSendState(); syncSlashMenu(); };" in script
    assert "el.input.onkeydown = event => { if (handleSlashKeys(event)) return;" in script
    assert 'if (state.slashOpen && !inside("#slash-menu") && event.target !== el.input) closeSlashMenu();' in script
    # 点面板内的选项可能重建面板内容（如「模型」指令），被点的行已脱离 DOM，
    # 因此"点在面板外"必须按事件派发时固定的 composedPath 判断
    assert 'const path = typeof event.composedPath === "function" ? event.composedPath() : [];' in script
    assert "const inside = selector => path.some(node => node.nodeType === 1 && node.matches && node.matches(selector));" in script
    assert "closeModelList(); closeSlashMenu(); closeModeList(); closeSessions();" in script

    # 取值链路不变：仍以 selected_skills 发给后端
    assert "const selectedSkills = el.skill.value ? [{id:el.skill.value,params:{}}] : [];" in script


def test_webui_category_round_trip_contract():
    """模型调 set_active_category 后，下拉框必须跟着变；用户自己选过的不许被顶掉。

    链路：agent_loop 推 session_meta → 前端 syncCategoryFromMeta 同步 state 与下拉框。
    同时区分「用户显式选过」和「只是个默认值」：只有前者才允许覆盖服务端 meta.json，
    否则「全部」这个空值会被 app.py 当成前端没传而回落，用户的选择又会被模型改掉。
    """
    script = (Path(WEBUI_DIR) / "app.js").read_text(encoding="utf-8")
    backend = (Path(__file__).resolve().parents[1] / "app.py").read_text(encoding="utf-8")

    # 前端：消费 session_meta，并把 state 和下拉框一起改掉
    assert 'else if (type === "session_meta") syncCategoryFromMeta(id, event.category || "");' in script
    assert "function syncCategoryFromMeta(id, category)" in script
    # 事件可能属于另一个会话（多会话并行流式输出），必须按会话 id 过滤
    assert "if (!state.session || state.session.meta.id !== id) return;" in script
    assert "if ((state.selectedCategory || \"\") !== (category || \"\")) selectCategory(category || \"\");" in script

    # 显式选择才有覆盖权：默认值不置 categoryTouched
    assert "if (explicit) state.categoryTouched = true;" in script
    assert 'selectCategory(opt.dataset.category || "", true);' in script
    assert "state.categoryTouched = false; selectCategory(state.session.meta.category || \"\");" in script

    # 请求体同时带值和「是不是用户选的」，后端据此决定要不要回落读 meta.json
    assert 'selected_category:state.selectedCategory || "",category_explicit:!!state.categoryTouched' in script
    assert 'if not selected_category and not bool(body.get("category_explicit")):' in backend

    # 切会话/刷新后下拉框要显示会话里存的分类，所以接口得把它带出来
    assert '"category": s.category,' in backend

