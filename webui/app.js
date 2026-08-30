"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const state = {
  sessions: [], session: null, models: [], skills: [], mode: "manual",
  controller: null, busy: false, assistant: null, workflow: null, configLoaded: false,
};
const el = {
  connection: $("#connection"), newSession: $("#new-session"), sessionTrigger: $("#session-trigger"),
  sessionPanel: $("#session-panel"), sessionSearch: $("#session-search"), sessionList: $("#session-list"),
  closeSessions: $("#close-sessions"), currentTitle: $("#current-title"), messages: $("#messages"),
  welcome: $("#welcome"), phasePanel: $("#phase-panel"), model: $("#model-select"), skill: $("#skill-select"),
  input: $("#message-input"), send: $("#send"), busyLabel: $("#busy-label"), warning: $("#config-warning"), toast: $("#toast"),
};

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, char => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"})[char]);
}
function showToast(message) {
  el.toast.textContent = message;
  el.toast.classList.remove("hidden");
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => el.toast.classList.add("hidden"), 5000);
}
async function request(path, options = {}) {
  const response = await fetch(path, {headers: {"Content-Type": "application/json", ...(options.headers || {})}, ...options});
  if (!response.ok) {
    let message = `请求失败 (${response.status})`;
    try { const data = await response.json(); message = data.error || data.detail || message; } catch (_) {}
    throw new Error(message);
  }
  return response.status === 204 ? null : response.json();
}
function setConnection(status, label) {
  el.connection.className = `connection ${status}`;
  $("b", el.connection).textContent = label;
}
function setBusy(busy) {
  state.busy = busy;
  el.send.classList.toggle("stop", busy);
  el.send.textContent = busy ? "■" : "↑";
  el.send.title = busy ? "停止接收" : "发送";
  el.send.setAttribute("aria-label", el.send.title);
  el.input.disabled = busy;
  el.busyLabel.textContent = busy ? "Agent 正在处理…" : "Enter 发送 · Shift+Enter 换行";
  updateSendState();
}
function updateSendState() {
  el.send.disabled = !state.busy && (!state.session || !el.input.value.trim() || !state.configLoaded);
}

function inlineMarkdown(text) {
  return escapeHtml(text)
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/\*([^*]+)\*/g, "<em>$1</em>")
    .replace(/\[([^\]]+)]\((https?:\/\/[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>');
}
function basicMarkdown(text) {
  const blocks = [];
  const source = String(text || "").replace(/```([\w-]*)\n?([\s\S]*?)```/g, (_, lang, code) => {
    const token = `\u0000${blocks.length}\u0000`;
    blocks.push(`<div class="code-block"><header>${escapeHtml(lang || "text")}</header><pre>${escapeHtml(code.trim())}</pre></div>`);
    return token;
  });
  const lines = source.split("\n");
  let html = "", list = "";
  const closeList = () => { if (list) { html += `</${list}>`; list = ""; } };
  for (const line of lines) {
    const token = line.match(/^\u0000(\d+)\u0000$/);
    if (token) { closeList(); html += blocks[Number(token[1])]; continue; }
    const heading = line.match(/^(#{1,3})\s+(.+)/);
    if (heading) { closeList(); const level = heading[1].length; html += `<h${level}>${inlineMarkdown(heading[2])}</h${level}>`; continue; }
    const item = line.match(/^\s*([-*]|\d+\.)\s+(.+)/);
    if (item) {
      const type = item[1].endsWith(".") ? "ol" : "ul";
      if (list !== type) { closeList(); list = type; html += `<${type}>`; }
      html += `<li>${inlineMarkdown(item[2])}</li>`; continue;
    }
    closeList();
    if (line.trim()) html += `<p>${inlineMarkdown(line)}</p>`;
  }
  closeList();
  return html;
}
function structuredBlocks(text) {
  const found = [];
  const visible = String(text || "").replace(/```json\s*([\s\S]*?)```/gi, (whole, raw) => {
    try {
      const data = JSON.parse(raw);
      if (["options", "tool_params", "toolparams", "workflow", "phase_plan"].some(key => key in data)) {
        found.push(data); return "";
      }
    } catch (_) {}
    return whole;
  });
  return {visible: visible.trim(), found};
}
function createMessage(role, content = "", label = "") {
  el.welcome?.remove();
  const node = document.createElement("article");
  node.className = `message ${role}`;
  const bubble = document.createElement("div");
  bubble.className = "bubble";
  if (label) bubble.innerHTML = `<div class="message-label">${escapeHtml(label)}</div>`;
  const body = document.createElement("div"); body.className = "markdown"; body.innerHTML = basicMarkdown(content);
  bubble.append(body); node.append(bubble); el.messages.append(node); scrollMessages();
  return {node, bubble, body, text: content, reasoning: "", structured: []};
}
function scrollMessages() { el.messages.scrollTop = el.messages.scrollHeight; }
function finishAssistant(message) {
  if (!message || message.finished) return;
  message.finished = true;
  const parsed = structuredBlocks(message.text);
  message.body.innerHTML = basicMarkdown(parsed.visible);
  parsed.found.forEach(data => renderStructured(data, message.node));
  if (!parsed.visible && !parsed.found.length && !message.node.querySelector(".tool-card,.approval-card")) message.node.remove();
  scrollMessages();
}
function appendReasoning(message, delta) {
  message.reasoning += delta || "";
  let details = $(".reasoning", message.node);
  if (!details) {
    details = document.createElement("details"); details.className = "reasoning";
    details.innerHTML = "<summary>思考过程</summary><div></div>";
    message.node.insertBefore(details, message.bubble);
  }
  $("div", details).textContent = message.reasoning;
}

function renderStructured(data, parent) {
  if (data.phase_plan) renderPhase(data.phase_plan);
  if (data.tool_params || data.toolparams) renderToolParams(data.tool_params || data.toolparams, data.options || [], parent);
  else if (data.options) renderOptions(data.options, parent);
  if (data.workflow) renderWorkflowProposal(data.workflow, parent);
}
function renderOptions(options, parent) {
  if (!Array.isArray(options) || !options.length) return;
  const card = document.createElement("section"); card.className = "structured";
  card.innerHTML = '<div class="structured-title">请选择下一步</div><div class="options"></div>';
  options.forEach(option => {
    const button = document.createElement("button");
    button.className = `option-button ${option.style || ""}`; button.type = "button";
    button.textContent = option.label || option.value || "选择";
    button.onclick = () => { card.querySelectorAll("button").forEach(item => item.disabled = true); sendMessage(String(option.value ?? option.label ?? ""), button.textContent); };
    $(".options", card).append(button);
  });
  parent.append(card);
}
function valueForInput(value) { return typeof value === "object" ? JSON.stringify(value) : String(value ?? ""); }
function coerceValue(value, original) {
  if (typeof original === "number") { const number = Number(value); return Number.isNaN(number) ? value : number; }
  if (typeof original === "boolean") return value === "true";
  if (typeof original === "object") { try { return JSON.parse(value); } catch (_) { return value; } }
  return value;
}
function renderToolParams(toolParams, options, parent) {
  if (!toolParams || typeof toolParams !== "object") return;
  const card = document.createElement("section"); card.className = "structured params-card";
  card.innerHTML = `<div class="structured-title">确认工具参数 · ${escapeHtml(toolParams.tool || "")}</div><div class="params"></div><div class="options"></div>`;
  const params = Array.isArray(toolParams.params) ? toolParams.params : [];
  params.forEach((param, index) => {
    const row = document.createElement("div"); row.className = "param-row";
    row.innerHTML = `<label for="param-${index}"><b>${escapeHtml(param.name)}</b>${escapeHtml(param.description || "")}</label>`;
    const input = document.createElement("input"); input.id = `param-${index}`; input.value = valueForInput(param.value); input.dataset.index = index;
    row.append(input); $(".params", card).append(row);
  });
  const actions = options.length ? options : [{label:"确认执行",value:"confirm",style:"primary"},{label:"取消",value:"cancel",style:"danger"}];
  actions.forEach(option => {
    const button = document.createElement("button"); button.className = `option-button ${option.style || ""}`; button.textContent = option.label || option.value; button.type = "button";
    button.onclick = () => {
      card.querySelectorAll("button,input").forEach(item => item.disabled = true);
      const values = {};
      params.forEach((param, index) => { values[param.name] = coerceValue(card.querySelector(`[data-index="${index}"]`).value, param.value); });
      const payload = `<structured_interaction>${JSON.stringify({type:"tool_params_confirmed",tool:toolParams.tool,confirmed:String(option.value)==="confirm",params:values})}</structured_interaction>`;
      sendMessage(payload, option.label || option.value);
    };
    $(".options", card).append(button);
  });
  parent.append(card);
}
function renderWorkflowProposal(workflow, parent) {
  const steps = Array.isArray(workflow?.steps) ? workflow.steps : [];
  const card = document.createElement("section"); card.className = "structured";
  card.innerHTML = '<div class="structured-title">静态工作流</div><div class="params"></div><div class="options"></div>';
  steps.forEach((step, index) => $(".params", card).insertAdjacentHTML("beforeend", `<div class="param-row"><label><b>${index + 1}. ${escapeHtml(step.tool)}</b>${escapeHtml(step.desc || "")}</label><code>${escapeHtml(JSON.stringify(step.params || {}))}</code></div>`));
  const run = document.createElement("button"); run.className = "option-button primary"; run.textContent = "执行工作流"; run.disabled = !steps.length;
  run.onclick = () => { run.disabled = true; runWorkflow(steps); };
  $(".options", card).append(run); parent.append(card);
}
function extractPhase(value) {
  if (value && typeof value === "object" && value.phases) return value;
  const text = typeof value === "string" ? value : value?.text;
  if (!text) return null;
  const blocks = structuredBlocks(text).found;
  return blocks.find(item => item.phase_plan)?.phase_plan || null;
}
function renderPhase(value) {
  const phase = extractPhase(value) || value;
  if (!phase || !Array.isArray(phase.phases)) return;
  el.phasePanel.classList.remove("hidden");
  const completed = phase.phases.filter(item => ["done","succeeded","completed"].includes(item.status)).length;
  el.phasePanel.innerHTML = `<div class="phase-head"><strong>${escapeHtml(phase.title || "阶段计划")}</strong><small>${completed}/${phase.phases.length}</small></div><div class="phase-steps"></div>`;
  phase.phases.forEach(item => $(".phase-steps", el.phasePanel).insertAdjacentHTML("beforeend", `<div class="phase-step ${escapeHtml(item.status || "pending")}" title="${escapeHtml(item.desc || "")}">${escapeHtml(item.title || item.id || "阶段")}</div>`));
}

function renderToolCall(event, parent) {
  const card = document.createElement("section"); card.className = "tool-card"; card.dataset.callId = event.id || event.call_id || "";
  card.innerHTML = `<div class="card-head"><strong>${escapeHtml(event.name || "工具调用")}</strong><span class="status">执行中</span></div><div class="tool-body">${escapeHtml(JSON.stringify(event.args || {}, null, 2))}</div>`;
  parent.append(card); scrollMessages();
}
function renderToolResult(event, parent) {
  let card = parent.querySelector(`[data-call-id="${CSS.escape(event.call_id || "")}"]`);
  if (!card) { renderToolCall({id:event.call_id,name:event.name,args:{}}, parent); card = parent.lastElementChild; }
  const failed = String(event.result || "").toLowerCase().includes("error") || String(event.result || "").includes("denied");
  const status = $(".status", card); status.textContent = failed ? "失败" : "完成"; status.className = `status ${failed ? "failed" : "succeeded"}`;
  $(".tool-body", card).textContent = String(event.result ?? ""); scrollMessages();
}
function renderApproval(event, parent) {
  const card = document.createElement("section"); card.className = "approval-card";
  card.innerHTML = `<div class="card-head"><strong>审批工具 · ${escapeHtml(event.name)}</strong><span class="status">等待操作</span></div><div class="approval-args"><textarea aria-label="工具参数">${escapeHtml(JSON.stringify(event.args || {}, null, 2))}</textarea></div><div class="approval-actions"><button class="action-button approve" type="button">批准</button><button class="action-button deny" type="button">拒绝</button></div>`;
  const resolve = async approved => {
    card.querySelectorAll("button,textarea").forEach(item => item.disabled = true);
    let args = event.args || {};
    if (approved) { try { args = JSON.parse($("textarea", card).value); } catch (_) { showToast("工具参数不是有效 JSON"); card.querySelectorAll("button,textarea").forEach(item => item.disabled = false); return; } }
    try {
      await request(`/sessions/${encodeURIComponent(state.session.meta.id)}/tool-approvals/${encodeURIComponent(event.call_id)}`, {method:"POST",body:JSON.stringify({approved,args})});
      const status = $(".status", card); status.textContent = approved ? "已批准" : "已拒绝"; status.className = `status ${approved ? "succeeded" : "cancelled"}`;
    } catch (error) { showToast(error.message); card.querySelectorAll("button,textarea").forEach(item => item.disabled = false); }
  };
  $(".approve", card).onclick = () => resolve(true); $(".deny", card).onclick = () => resolve(false); parent.append(card); scrollMessages();
}
function renderWorkflowEvent(event) {
  if (!state.workflow || event.type === "workflow_started") {
    const message = createMessage("workflow", "", "WORKFLOW"); message.body.remove();
    const card = document.createElement("section"); card.className = "workflow-card"; card.innerHTML = '<div class="card-head"><strong>工作流执行</strong><span class="status">运行中</span></div><div class="params"></div>';
    message.node.append(card); state.workflow = {message, card, steps: []};
  }
  if (event.type === "workflow_step") state.workflow.steps[event.index] = event;
  const body = $(".params", state.workflow.card); body.innerHTML = "";
  state.workflow.steps.forEach((step, index) => { if (step) body.insertAdjacentHTML("beforeend", `<div class="param-row"><label><b>${index + 1}. ${escapeHtml(step.tool)}</b>${escapeHtml(step.desc || "")}</label><span class="status ${escapeHtml(step.status)}">${escapeHtml(step.status)}</span></div>`); });
  if (event.type === "workflow_done") { const status = $(".card-head .status", state.workflow.card); status.textContent = event.status; status.className = `status ${event.status}`; }
  scrollMessages();
}

function renderHistoryMessage(message) {
  if (message.role === "user") return createMessage("user", message.display_content || message.content || "");
  if (message.role === "assistant") {
    const item = createMessage("assistant", "", (message.active_skills || []).join(" · "));
    item.text = message.content || ""; finishAssistant(item);
    (message.tool_calls || []).forEach(call => { let args = {}; try { args = JSON.parse(call.function?.arguments || "{}"); } catch (_) {} renderToolCall({id:call.id,name:call.function?.name,args}, item.node); });
    return item;
  }
  if (message.role === "tool") {
    const item = createMessage("tool", "", "TOOL RESULT"); renderToolResult({call_id:message.tool_call_id,name:message.tool_name,result:message.content}, item.node); return item;
  }
  if (message.role === "workflow") {
    renderWorkflowEvent({type:"workflow_started"});
    (message.steps || []).forEach((step,index) => renderWorkflowEvent({type:"workflow_step",index,...step}));
    renderWorkflowEvent({type:"workflow_done",status:message.status,message:message.message});
  }
}
async function loadSession(id) {
  if (state.busy) state.controller?.abort();
  try {
    state.session = await request(`/sessions/${encodeURIComponent(id)}`);
    el.currentTitle.textContent = state.session.meta.title;
    if (state.session.meta.model_id && state.models.some(item => item.model_id === state.session.meta.model_id)) el.model.value = state.session.meta.model_id;
    el.messages.innerHTML = ""; el.phasePanel.classList.add("hidden"); state.workflow = null;
    if (!state.session.messages.length) el.messages.innerHTML = '<div id="welcome" class="empty-state"><div class="empty-symbol">⌁</div><strong>对话已就绪</strong><p>描述你的工程目标，Agent 将按当前模式执行。</p></div>';
    else state.session.messages.forEach(renderHistoryMessage);
    closeSessions(); updateSendState(); scrollMessages();
  } catch (error) { showToast(error.message); }
}
function renderSessions() {
  const query = el.sessionSearch.value.trim().toLowerCase(); el.sessionList.innerHTML = "";
  const sessions = state.sessions.filter(item => !query || String(item.title).toLowerCase().includes(query));
  if (!sessions.length) { el.sessionList.innerHTML = '<div class="empty-state"><p>没有会话</p></div>'; return; }
  sessions.forEach(session => {
    const row = document.createElement("div"); row.className = "session-row";
    row.innerHTML = `<button class="session-select" type="button"><strong>${escapeHtml(session.title || "未命名会话")}</strong><small>${escapeHtml(String(session.updated_at || session.created_at || "").slice(0,16).replace("T"," "))}</small></button><div class="session-actions"><button class="rename" type="button" title="重命名">✎</button><button class="clear" type="button" title="清空">⌫</button><button class="danger delete" type="button" title="删除">×</button></div>`;
    $(".session-select", row).onclick = () => loadSession(session.id);
    $(".rename", row).onclick = () => renameSession(session);
    $(".clear", row).onclick = () => clearSession(session);
    $(".delete", row).onclick = () => deleteSession(session);
    el.sessionList.append(row);
  });
}
async function refreshSessions(selectId = null) {
  const data = await request("/sessions"); state.sessions = data.sessions || []; renderSessions();
  if (selectId) await loadSession(selectId);
}
async function createSession() {
  try { const data = await request("/sessions", {method:"POST",body:JSON.stringify({title:"New Session",model_id:el.model.value || ""})}); await refreshSessions(data.id); } catch (error) { showToast(error.message); }
}
async function renameSession(session) {
  const title = prompt("输入新的会话名称", session.title || ""); if (!title?.trim()) return;
  try { await request(`/sessions/${encodeURIComponent(session.id)}/rename`, {method:"PUT",body:JSON.stringify({title:title.trim()})}); await refreshSessions(); if (state.session?.meta.id === session.id) { state.session.meta.title = title.trim(); el.currentTitle.textContent = title.trim(); } } catch (error) { showToast(error.message); }
}
async function clearSession(session) {
  if (!confirm(`清空“${session.title}”的全部消息？`)) return;
  try { await request(`/sessions/${encodeURIComponent(session.id)}/clear`, {method:"POST",body:"{}"}); if (state.session?.meta.id === session.id) await loadSession(session.id); await refreshSessions(); } catch (error) { showToast(error.message); }
}
async function deleteSession(session) {
  if (!confirm(`永久删除“${session.title}”？`)) return;
  try { await request(`/sessions/${encodeURIComponent(session.id)}`, {method:"DELETE"}); if (state.session?.meta.id === session.id) { state.session = null; el.currentTitle.textContent = "选择会话"; el.messages.innerHTML = '<div id="welcome" class="empty-state"><div class="empty-symbol">⌁</div><strong>开始一项工程任务</strong><p>创建对话后，可进行网格生成、质量检查和工具编排。</p></div>'; } await refreshSessions(); updateSendState(); } catch (error) { showToast(error.message); }
}
function openSessions() { el.sessionPanel.classList.remove("hidden"); el.sessionTrigger.setAttribute("aria-expanded","true"); el.sessionSearch.focus(); }
function closeSessions() { el.sessionPanel.classList.add("hidden"); el.sessionTrigger.setAttribute("aria-expanded","false"); }

async function consumeSse(response, onEvent) {
  if (!response.ok) { let message = `请求失败 (${response.status})`; try { const data = await response.json(); message = data.error || message; } catch (_) {} throw new Error(message); }
  if (!response.body) throw new Error("浏览器不支持流式响应");
  const reader = response.body.getReader(), decoder = new TextDecoder(); let buffer = "";
  while (true) {
    const {value, done} = await reader.read(); buffer += decoder.decode(value || new Uint8Array(), {stream:!done}).replace(/\r\n/g,"\n");
    let boundary;
    while ((boundary = buffer.indexOf("\n\n")) >= 0) {
      const frame = buffer.slice(0,boundary); buffer = buffer.slice(boundary+2); let type = "message"; const dataLines = [];
      frame.split("\n").forEach(line => { if (line.startsWith("event:")) type = line.slice(6).trim(); else if (line.startsWith("data:")) dataLines.push(line.slice(5).trimStart()); });
      if (dataLines.length) { try { await onEvent(type, JSON.parse(dataLines.join("\n"))); } catch (error) { if (error instanceof SyntaxError) showToast(`忽略无效 SSE 数据：${type}`); else throw error; } }
    }
    if (done) break;
  }
}
async function sendMessage(rawMessage = null, displayContent = null) {
  if (state.busy || !state.session) return;
  const message = rawMessage ?? el.input.value.trim(); if (!message) return;
  const shown = displayContent ?? message; createMessage("user", shown); el.input.value = ""; updateSendState();
  const assistant = createMessage("assistant", "", el.skill.selectedOptions[0]?.text !== "无 Skill" ? el.skill.selectedOptions[0]?.text : ""); state.assistant = assistant;
  const controller = new AbortController(); state.controller = controller; setBusy(true);
  try {
    const selectedSkills = el.skill.value ? [{id:el.skill.value,params:{}}] : [];
    const response = await fetch("/chat/stream", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:state.session.meta.id,message,display_content:shown === message ? "" : shown,interaction_mode:state.mode,model_id:el.model.value || "",selected_skills:selectedSkills})});
    await consumeSse(response, async (type, event) => {
      if (type === "text_chunk") { assistant.text += event.delta || ""; assistant.body.innerHTML = basicMarkdown(assistant.text); scrollMessages(); }
      else if (type === "reasoning_chunk") appendReasoning(assistant,event.delta);
      else if (type === "phase_plan") renderPhase(event.text || event);
      else if (type === "tool_call") renderToolCall(event,assistant.node);
      else if (type === "tool_result") renderToolResult(event,assistant.node);
      else if (type === "tool_approval_required") renderApproval(event,assistant.node);
      else if (type === "skill_loaded") { assistant.bubble.querySelector(".message-label")?.remove(); assistant.bubble.insertAdjacentHTML("afterbegin",`<div class="message-label">SKILL LOADED · ${escapeHtml(event.skill_id)}</div>`); }
      else if (type === "error") throw new Error(event.message || "Agent 处理失败");
      else if (type === "done") finishAssistant(assistant);
    });
    finishAssistant(assistant); await refreshSessions();
  } catch (error) {
    finishAssistant(assistant);
    if (error.name === "AbortError") createMessage("assistant", "已停止接收当前响应。", "STOPPED");
    else { createMessage("assistant", error.message, "ERROR").bubble.style.borderColor = "var(--red)"; setConnection("offline","连接异常"); }
  } finally { if (state.controller === controller) state.controller = null; setBusy(false); }
}
async function runWorkflow(steps) {
  if (state.busy || !state.session) return;
  const controller = new AbortController(); state.controller = controller; state.workflow = null; setBusy(true);
  try {
    const response = await fetch("/workflows/run", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:state.session.meta.id,steps,selected_skills:el.skill.value?[{id:el.skill.value,params:{}}]:[]})});
    await consumeSse(response, async (type,event) => { event.type = type; if (["workflow_started","workflow_step","workflow_done"].includes(type)) renderWorkflowEvent(event); else if (type === "tool_approval_required") renderApproval(event,state.workflow.message.node); else if (type === "error") throw new Error(event.message || "工作流失败"); });
    await refreshSessions();
  } catch (error) { if (error.name !== "AbortError") showToast(error.message); }
  finally { if (state.controller === controller) state.controller = null; setBusy(false); }
}

async function bootstrap() {
  setConnection("checking","连接中");
  const results = await Promise.allSettled([request("/health"),request("/config/models"),request("/skills"),request("/sessions")]);
  const [health,models,skills,sessions] = results;
  if (health.status === "fulfilled") { state.configLoaded = Boolean(health.value.config_loaded); setConnection("online","Agent 已连接"); }
  else setConnection("offline","服务不可用");
  if (models.status === "fulfilled") {
    state.models = models.value.models || []; el.model.innerHTML = "";
    if (!state.models.length) el.model.innerHTML = '<option value="">未配置</option>';
    else state.models.forEach(item => el.model.add(new Option(`${item.provider} / ${item.model_id}`,item.model_id)));
    if (state.models[models.value.default_index]) el.model.value = state.models[models.value.default_index].model_id;
  }
  if (skills.status === "fulfilled") {
    state.skills = skills.value.skills || []; el.skill.innerHTML = '<option value="">无 Skill</option>';
    state.skills.forEach(item => { const option = new Option(item.name || item.id,item.id); option.title = item.description || ""; el.skill.add(option); });
  }
  if (sessions.status === "fulfilled") { state.sessions = sessions.value.sessions || []; renderSessions(); if (state.sessions[0]) await loadSession(state.sessions[0].id); }
  const failures = results.filter(item => item.status === "rejected"); if (failures.length) showToast(failures[0].reason.message);
  el.warning.classList.toggle("hidden",state.configLoaded); updateSendState();
}

el.newSession.onclick = createSession;
el.sessionTrigger.onclick = () => el.sessionPanel.classList.contains("hidden") ? openSessions() : closeSessions();
el.closeSessions.onclick = closeSessions; el.sessionSearch.oninput = renderSessions;
el.connection.onclick = bootstrap;
el.send.onclick = () => state.busy ? state.controller?.abort() : sendMessage();
el.input.oninput = updateSendState;
el.input.onkeydown = event => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); if (!state.busy && !el.send.disabled) sendMessage(); } };
document.querySelectorAll("[data-mode]").forEach(button => button.onclick = () => { state.mode = button.dataset.mode; document.querySelectorAll("[data-mode]").forEach(item => item.classList.toggle("active",item === button)); });
document.addEventListener("keydown", event => { if (event.key === "Escape") closeSessions(); });
bootstrap();
