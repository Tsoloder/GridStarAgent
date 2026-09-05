"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
// Chromium 65 兼容：Promise.allSettled / AbortController 垫片
function settle(promise) { return promise.then(value => ({status: "fulfilled", value}), reason => ({status: "rejected", reason})); }
const HAS_ABORT = typeof AbortController === "function";
// Chromium 65 降级：fetch 不认 signal（66+ 才支持），改为挂载流式 reader，
// abort() 时 reader.cancel() 中断读取并断开连接，等效于真正停止。
function createAbortController() {
  if (HAS_ABORT) return new AbortController();
  return {signal: undefined, reader: null, aborted: false, abort: function () { this.aborted = true; if (this.reader && this.reader.cancel) { try { this.reader.cancel(); } catch (_) {} } }};
}
const state = {
  sessions: [], session: null, models: [], skills: [], mode: "manual",
  controller: null, busy: false, assistant: null, workflow: null, configLoaded: false,
  modelListOpen: false, modelOptionIndex: -1, modelSearch: "",
  settings: {open:false,activeTab:"models",activeProviderId:null,original:null,draft:null,revision:null,dirty:false,testingProviderId:null,readingProviderId:null,discoveredModels:{},validationErrors:{},controllers:{}},
  mcp: {tools:[],loaded:false,loading:false,connected:false,error:""},
  skillsLoading: false, skillsError: "",
};
const el = {
  connection: $("#connection"), newSession: $("#new-session"), sessionTrigger: $("#session-trigger"),
  sessionPanel: $("#session-panel"), sessionSearch: $("#session-search"), sessionList: $("#session-list"),
  closeSessions: $("#close-sessions"), currentTitle: $("#current-title"), messages: $("#messages"),
  welcome: $("#welcome"), phasePanel: $("#phase-panel"), model: $("#model-select"), modelTrigger: $("#model-trigger"), modelLabel: $("#model-label"), modelListbox: $("#model-listbox"), skill: $("#skill-select"), skillTrigger: $("#skill-trigger"), skillLabel: $("#skill-label"), skillListbox: $("#skill-listbox"),
  input: $("#message-input"), send: $("#send"), busyLabel: $("#busy-label"), warning: $("#config-warning"), toast: $("#toast"),
  openSettings: $("#open-settings"), settingsModal: $("#settings-modal"), closeSettings: $("#close-settings"), cancelSettings: $("#cancel-settings"), saveSettings: $("#save-settings"), settingsStatus: $("#settings-status"), providerList: $("#provider-list"), providerEditor: $("#provider-editor"), addProvider: $("#add-provider"),
  mcpTools: $("#mcp-tools"), mcpCount: $("#mcp-count"), mcpStatus: $("#mcp-status"), refreshMcp: $("#refresh-mcp"),
  skillsList: $("#skills-list"), skillCount: $("#skill-count"), skillsStatus: $("#skills-status"), refreshSkills: $("#refresh-skills"),
};
el.phasePanel.addEventListener("click", event => {
  if (!event.target.closest(".phase-head")) return;
  const expanded = el.phasePanel.classList.toggle("expanded");
  const head = el.phasePanel.querySelector(".phase-head");
  if (head) head.setAttribute("aria-expanded", String(expanded));
});

function escapeHtml(value) {
  return String(value == null ? "" : value).replace(/[&<>"']/g, char => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"})[char]);
}
function showToast(message) {
  el.toast.textContent = message;
  el.toast.classList.remove("hidden");
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => el.toast.classList.add("hidden"), 5000);
}
function showDialog({title, message = "", input, confirmText = "确定", cancelText = "取消", danger = false}) {
  return new Promise(resolve => {
    const backdrop = document.createElement("div"); backdrop.className = "modal-backdrop dialog-backdrop";
    const okClass = danger ? "action-button deny" : "action-button approve";
    backdrop.innerHTML = `<div class="confirm-dialog" role="dialog" aria-modal="true" aria-label="${escapeHtml(title)}"><h3>${escapeHtml(title)}</h3>${message ? `<p>${escapeHtml(message)}</p>` : ""}${input !== undefined ? `<input type="text" value="${escapeHtml(input)}" aria-label="${escapeHtml(title)}">` : ""}<div class="dialog-actions"><button type="button" class="action-button" data-act="cancel">${escapeHtml(cancelText)}</button><button type="button" class="${okClass}" data-act="ok">${escapeHtml(confirmText)}</button></div></div>`;
    let settled = false;
    const done = value => { if (settled) return; settled = true; document.removeEventListener("keydown", onKey); backdrop.remove(); resolve(value); };
    const ok = () => { if (input !== undefined) { const field = $("input", backdrop); const value = field ? field.value.trim() : ""; done(value || null); } else done(true); };
    const cancel = () => done(input !== undefined ? null : false);
    const onKey = event => { if (event.key === "Escape") cancel(); else if (event.key === "Enter") ok(); };
    $("[data-act='ok']", backdrop).onclick = ok;
    $("[data-act='cancel']", backdrop).onclick = cancel;
    backdrop.onclick = event => { if (event.target === backdrop) cancel(); };
    document.addEventListener("keydown", onKey);
    document.body.append(backdrop);
    if (input !== undefined) { const field = $("input", backdrop); field.focus(); field.select(); }
  });
}
async function request(path, options = {}) {
  const response = await fetch(path, {headers: {"Content-Type": "application/json", ...(options.headers || {})}, ...options});
  if (!response.ok) {
    let message = `请求失败 (${response.status})`;
    try { const data = await response.json(); message = data.error || data.detail || message; } catch (_) {}
    const error = new Error(message); error.status = response.status; throw error;
  }
  return response.status === 204 ? null : response.json();
}
function modelKey(item) { return item.key || (String(item.model_id || item.id || "").includes("/") ? String(item.model_id || item.id) : `${item.provider}/${item.model_id || item.id}`); }
function modelName(item) { return item.name || item.display_name || item.model_id || item.id || modelKey(item); }
function visibleModels() { return state.models.filter(item => item.enabled !== false && item.provider_enabled !== false); }
function renderModelList() {
  const selected = el.model.value; el.modelListbox.innerHTML = "";
  const groups = new Map(); visibleModels().forEach(item => { if (!groups.has(item.provider)) groups.set(item.provider, []); groups.get(item.provider).push(item); });
  groups.forEach((items, provider) => {
    // 分组标题显示供应商名称，没有配置名称时回退到供应商 ID
    const label = items[0].provider_name || provider;
    const group = document.createElement("div"); group.className = "model-group"; group.setAttribute("role","group"); group.setAttribute("aria-label",label);
    group.innerHTML = `<div class="model-group-label">${escapeHtml(label)}</div>`;
    items.forEach(item => { const key = modelKey(item), option = document.createElement("button"); option.type = "button"; option.className = `model-option${key === selected ? " selected" : ""}`; option.setAttribute("role","option"); option.setAttribute("aria-selected",String(key === selected)); option.dataset.value = key; option.innerHTML = `<span class="model-check">${key === selected ? "✓" : ""}</span><strong>${escapeHtml(modelName(item))}</strong><small>${escapeHtml(item.model_id || item.id || key)}</small>`; option.onclick = () => selectModel(key); group.append(option); });
    el.modelListbox.append(group);
  });
  if (!groups.size) el.modelListbox.innerHTML = '<div class="listbox-empty">未配置可用模型</div>';
}
function selectModel(key) { const item = visibleModels().find(model => modelKey(model) === key); el.model.value = key || ""; el.modelLabel.textContent = item ? modelName(item) : "未配置"; closeModelList(); renderModelList(); }
function openModelList() { renderModelList(); state.modelListOpen = true; state.modelOptionIndex = Math.max(0, [...el.modelListbox.querySelectorAll(".model-option")].findIndex(item => item.dataset.value === el.model.value)); el.modelListbox.classList.remove("hidden"); el.modelTrigger.setAttribute("aria-expanded","true"); focusModelOption(); }
function closeModelList() { state.modelListOpen = false; state.modelSearch = ""; el.modelListbox.classList.add("hidden"); el.modelTrigger.setAttribute("aria-expanded","false"); }
function focusModelOption() { const options = [...el.modelListbox.querySelectorAll(".model-option")]; options.forEach((item,index) => item.classList.toggle("focused",index === state.modelOptionIndex)); const active = options[state.modelOptionIndex]; if (active) active.scrollIntoView({block:"nearest"}); }
function handleModelKeys(event) {
  if (!["ArrowDown","ArrowUp","Enter","Escape"].includes(event.key) && event.key.length !== 1) return;
  if (!state.modelListOpen) { if (["ArrowDown","ArrowUp","Enter"].includes(event.key)) { event.preventDefault(); openModelList(); } return; }
  const options = [...el.modelListbox.querySelectorAll(".model-option")];
  if (event.key === "Escape") { event.preventDefault(); closeModelList(); return; }
  if (event.key === "Enter") { event.preventDefault(); const active = options[state.modelOptionIndex]; if (active) active.click(); return; }
  if (event.key === "ArrowDown" || event.key === "ArrowUp") { event.preventDefault(); state.modelOptionIndex = (state.modelOptionIndex + (event.key === "ArrowDown" ? 1 : -1) + options.length) % options.length; focusModelOption(); return; }
  state.modelSearch = (state.modelSearch + event.key).toLowerCase(); clearTimeout(handleModelKeys.timer); handleModelKeys.timer = setTimeout(() => state.modelSearch = "",700); const index = options.findIndex(item => item.textContent.toLowerCase().includes(state.modelSearch)); if (index >= 0) { state.modelOptionIndex = index; focusModelOption(); }
}
function selectedSkill() { return state.skills.find(item => item.id === el.skill.value) || null; }
function renderSkillList() {
  const selected = el.skill.value; el.skillListbox.innerHTML = "";
  [{ id: "", name: "无 Skill", description: "" }].concat(state.skills).forEach(item => {
    const option = document.createElement("button"); option.type = "button"; option.className = `model-option${item.id === selected ? " selected" : ""}`; option.setAttribute("role","option"); option.setAttribute("aria-selected",String(item.id === selected)); option.dataset.value = item.id;
    if (item.description) option.title = item.description;
    option.innerHTML = `<span class="model-check">${item.id === selected ? "✓" : ""}</span><strong>${escapeHtml(item.name || item.id)}</strong><small></small>`;
    option.onclick = () => selectSkill(item.id); el.skillListbox.append(option);
  });
}
function selectSkill(id) { el.skill.value = id || ""; const item = selectedSkill(); el.skillLabel.textContent = item ? (item.name || item.id) : "无 Skill"; closeSkillList(); }
function openSkillList() { renderSkillList(); state.skillListOpen = true; el.skillListbox.classList.remove("hidden"); el.skillTrigger.setAttribute("aria-expanded","true"); }
function closeSkillList() { state.skillListOpen = false; el.skillListbox.classList.add("hidden"); el.skillTrigger.setAttribute("aria-expanded","false"); }
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
  el.send.disabled = !state.busy && (!el.input.value.trim() || !state.configLoaded);
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
  const welcome = document.getElementById("welcome");
  if (welcome) welcome.remove();
  if (el.welcome) el.welcome = null;
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
function renderTokenUsage(message, event) {
  if (!message || message.node.querySelector(".token-usage")) return;
  const total = event.tokens || 0, input = event.tokens_input || 0, output = event.tokens_output || 0;
  if (!total && !input && !output) return;
  const usage = document.createElement("div");
  usage.className = "token-usage";
  // 供应商没回传 usage 时后端会给出本地估算值，用 ≈ 区分实测与估算。
  usage.textContent = `${event.tokens_estimated ? "≈ " : ""}tokens ${total} · input ${input} · output ${output}`;
  message.bubble.append(usage);
}
function finishAssistant(message) {
  if (!message || message.finished) return;
  message.finished = true;
  const parsed = structuredBlocks(message.text);
  message.body.innerHTML = basicMarkdown(parsed.visible);
  parsed.found.forEach(data => renderStructured(data, message.node));
  if (!parsed.visible && !parsed.found.length && !message.node.querySelector(".tool-group,.approval-card")) message.node.remove();
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
    button.onclick = () => { card.querySelectorAll("button").forEach(item => item.disabled = true); sendMessage(String(option.value != null ? option.value : (option.label != null ? option.label : "")), button.textContent); };
    $(".options", card).append(button);
  });
  parent.append(card);
}
function valueForInput(value) { return typeof value === "object" ? JSON.stringify(value) : String(value == null ? "" : value); }
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
  const steps = Array.isArray(workflow && workflow.steps) ? workflow.steps : [];
  const card = document.createElement("section"); card.className = "structured";
  card.innerHTML = '<div class="structured-title">静态工作流</div><div class="params"></div><div class="options"></div>';
  steps.forEach((step, index) => $(".params", card).insertAdjacentHTML("beforeend", `<div class="param-row"><label><b>${index + 1}. ${escapeHtml(step.tool)}</b>${escapeHtml(step.desc || "")}</label><code>${escapeHtml(JSON.stringify(step.params || {}))}</code></div>`));
  const run = document.createElement("button"); run.className = "option-button primary"; run.textContent = "执行工作流"; run.disabled = !steps.length;
  run.onclick = () => { run.disabled = true; runWorkflow(steps); };
  $(".options", card).append(run); parent.append(card);
}
function extractPhase(value) {
  if (value && typeof value === "object" && value.phases) return value;
  const text = typeof value === "string" ? value : (value && value.text);
  if (!text) return null;
  const blocks = structuredBlocks(text).found;
  const found = blocks.find(item => item.phase_plan);
  return (found && found.phase_plan) || null;
}
function renderPhase(value) {
  const phase = extractPhase(value) || value;
  if (!phase || !Array.isArray(phase.phases)) return;
  el.phasePanel.classList.remove("hidden");
  const completed = phase.phases.filter(item => ["done","succeeded","completed","skipped"].includes(item.status)).length;
  const expanded = el.phasePanel.classList.contains("expanded");
  const pct = phase.phases.length ? Math.round(completed / phase.phases.length * 100) : 0;
  el.phasePanel.innerHTML = `<div class="phase-head" role="button" tabindex="0" aria-expanded="${expanded}" title="点击展开/收起进度"><strong>${escapeHtml(phase.title || "阶段计划")}</strong><small>${completed}/${phase.phases.length}</small><span class="phase-chevron" aria-hidden="true">⌃</span><i class="phase-progress" style="width:${pct}%"></i></div><div class="phase-steps"></div>`;
  const head = $(".phase-head", el.phasePanel);
  head.addEventListener("keydown", event => {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault();
    el.phasePanel.classList.toggle("expanded");
    head.setAttribute("aria-expanded", String(el.phasePanel.classList.contains("expanded")));
  });
  phase.phases.forEach(item => $(".phase-steps", el.phasePanel).insertAdjacentHTML("beforeend", `<div class="phase-step ${escapeHtml(item.status || "pending")}" title="${escapeHtml(item.note || item.desc || "")}"><span class="phase-step-title">${escapeHtml(item.title || item.id || "阶段")}</span><span class="phase-step-note">${escapeHtml(item.note || item.desc || "")}</span></div>`));
}

function toolGroup(parent) {
  let group = parent.querySelector(":scope > .tool-group");
  if (group) return group;
  group = document.createElement("details"); group.className = "tool-group";
  group.innerHTML = `<summary class="tool-group-head"><span class="tool-chevron" aria-hidden="true"></span><strong>工具调用</strong><span class="tool-count">0 个</span><span class="status">执行中</span></summary><div class="tool-list"></div>`;
  parent.append(group); return group;
}
function updateToolGroup(group) {
  const items = [...group.querySelectorAll(".tool-item")];
  $(".tool-count", group).textContent = `${items.length} 个`;
  const running = items.some(item => item.dataset.state === "running");
  const failed = items.some(item => item.dataset.state === "failed");
  const status = $(".tool-group-head .status", group);
  status.textContent = running ? "执行中" : failed ? "失败" : "完成";
  status.className = `status ${running ? "" : failed ? "failed" : "succeeded"}`;
}
function renderToolCall(event, parent) {
  const group = toolGroup(parent); const item = document.createElement("details");
  item.className = "tool-item"; item.dataset.callId = event.id || event.call_id || ""; item.dataset.state = "running";
  item.innerHTML = `<summary><span class="tool-dot" aria-hidden="true"></span><strong>${escapeHtml(event.name || "工具调用")}</strong><span class="status">执行中</span></summary><div class="tool-detail"><span class="tool-detail-label">调用参数</span><pre class="tool-args">${escapeHtml(JSON.stringify(event.args || {}, null, 2))}</pre><div class="tool-result hidden"><span class="tool-detail-label">调用结果</span><pre></pre></div></div>`;
  $(".tool-list", group).append(item); updateToolGroup(group); scrollMessages();
}
function renderToolResult(event, parent) {
  const selector = `[data-call-id="${CSS.escape(event.call_id || "")}"]`;
  let item = parent.querySelector(selector) || document.querySelector(selector);
  if (!item) { renderToolCall({id:event.call_id,name:event.name,args:{}}, parent); item = parent.querySelector(selector); }
  const failed = String(event.result || "").toLowerCase().includes("error") || String(event.result || "").includes("denied");
  item.dataset.state = failed ? "failed" : "succeeded";
  const status = $(":scope > summary .status", item); status.textContent = failed ? "失败" : "完成"; status.className = `status ${failed ? "failed" : "succeeded"}`;
  const result = $(".tool-result", item); result.classList.remove("hidden"); $("pre", result).textContent = String(event.result == null ? "" : event.result);
  updateToolGroup(item.closest(".tool-group")); scrollMessages();
}
function coerceSchemaValue(raw, type) {
  const text = String(raw == null ? "" : raw).trim();
  if (type === "number" || type === "integer") { const number = Number(text); return Number.isNaN(number) ? text : number; }
  if (type === "boolean") return ["true","1","yes"].includes(text.toLowerCase());
  if (type === "object" || type === "array") { try { return JSON.parse(text); } catch (_) { return text; } }
  return raw;
}
function renderApproval(event, parent) {
  const card = document.createElement("section"); card.className = "approval-card";
  card.innerHTML = `<div class="card-head"><strong>审批工具 · ${escapeHtml(event.name)}</strong><span class="status">等待操作</span></div>`;
  const args = event.args && typeof event.args === "object" && !Array.isArray(event.args) ? event.args : {};
  const schemaProps = event.schema && event.schema.properties && typeof event.schema.properties === "object" ? event.schema.properties : null;
  const required = event.schema && Array.isArray(event.schema.required) ? event.schema.required : [];
  const inferType = value => typeof value === "number" ? "number" : typeof value === "boolean" ? "boolean" : (value !== null && typeof value === "object" ? "object" : "string");
  const entries = schemaProps
    ? Object.entries(schemaProps).map(([name, def]) => ({name, type: (def && def.type) || inferType(args[name]), desc: (def && def.description) || "", value: args[name]}))
    : Object.keys(args).map(name => ({name, type: inferType(args[name]), desc: "", value: args[name]}));
  if (schemaProps) Object.keys(args).filter(name => !(name in schemaProps)).forEach(name => entries.push({name, type: inferType(args[name]), desc: "", value: args[name]}));
  if (entries.length) {
    const wrap = document.createElement("div"); wrap.className = "params";
    entries.forEach((entry, index) => {
      const row = document.createElement("div"); row.className = "param-row";
      const mark = required.includes(entry.name) ? " *" : "";
      row.innerHTML = `<label for="approval-param-${index}"><b>${escapeHtml(entry.name + mark)}</b>${escapeHtml(entry.desc || "")}</label>`;
      const structured = entry.type === "object" || entry.type === "array";
      const input = document.createElement(structured ? "textarea" : "input");
      input.id = `approval-param-${index}`; input.dataset.index = index; input.value = valueForInput(entry.value);
      if (structured) { input.rows = 3; input.style.font = "11px/1.4 Consolas,monospace"; }
      row.append(input); wrap.append(row);
    });
    card.append(wrap);
  } else {
    card.insertAdjacentHTML("beforeend", `<div class="approval-args"><textarea aria-label="工具参数">${escapeHtml(JSON.stringify(event.args || {}, null, 2))}</textarea></div>`);
  }
  card.insertAdjacentHTML("beforeend", `<div class="approval-actions"><button class="action-button approve" type="button">批准</button><button class="action-button deny" type="button">拒绝</button></div>`);
  const setDisabled = disabled => card.querySelectorAll("button,input,textarea").forEach(item => item.disabled = disabled);
  const resolve = async approved => {
    setDisabled(true);
    let args = event.args || {};
    if (approved) {
      try {
        if (entries.length) {
          const values = {};
          entries.forEach((entry, index) => { values[entry.name] = coerceSchemaValue(card.querySelector(`[data-index="${index}"]`).value, entry.type); });
          args = values;
        } else args = JSON.parse($(".approval-args textarea", card).value);
      } catch (_) { showToast("工具参数不是有效 JSON"); setDisabled(false); return; }
    }
    try {
      await request(`/sessions/${encodeURIComponent(state.session.meta.id)}/tool-approvals/${encodeURIComponent(event.call_id)}`, {method:"POST",body:JSON.stringify({approved,args})});
      const status = $(".status", card); status.textContent = approved ? "已批准" : "已拒绝"; status.className = `status ${approved ? "succeeded" : "cancelled"}`;
    } catch (error) { showToast(error.message); setDisabled(false); }
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

function skillLabel(item, skills) {
  const label = (skills || []).join(" · "); if (!label) return;
  let node = $(".message-label", item.bubble);
  if (!node) { node = document.createElement("div"); node.className = "message-label"; item.bubble.insertBefore(node, item.bubble.firstChild); }
  node.textContent = label;
}
// turn 为本轮已合并的气泡：实时流里一轮对话只有一个 assistant 气泡（文本累加、
// 所有工具调用进同一个折叠组），而持久化会拆成多条消息，渲染时必须合并回去。
function renderHistoryMessage(message, turn) {
  if (message.role === "user") return createMessage("user", message.display_content || message.content || "");
  if (message.role === "assistant") {
    const item = turn || createMessage("assistant", "", "");
    // 带工具调用的消息不存 active_skills，Skill 标签要等本轮后续消息补上
    skillLabel(item, message.active_skills);
    if (message.content) item.text += (item.text ? "\n\n" : "") + message.content;
    item.body.innerHTML = basicMarkdown(item.text);
    if (message.reasoning_content) appendReasoning(item, message.reasoning_content);
    // 与实时流顺序一致：先把持久化的工具调用挂到消息节点，收尾留给 finishHistoryTurn。
    // 否则 finishAssistant 会把"无正文、仅工具调用"的消息（自动模式常见）当空气泡移除，
    // 后续 tool 结果找不到对应 call-id，退化为独立 TOOL RESULT 气泡。
    (message.tool_calls || []).forEach(call => { let args = {}; try { args = JSON.parse((call.function && call.function.arguments) || "{}"); } catch (_) {} renderToolCall({id:call.id,name:call.function && call.function.name,args}, item.node); });
    if (message.usage) item.usage = message.usage;
    return item;
  }
  if (message.role === "tool") {
    const selector = `[data-call-id="${CSS.escape(message.tool_call_id || "")}"]`;
    const existing = (turn && turn.node.querySelector(selector)) || document.querySelector(selector);
    // 结果回填到本轮气泡里的工具项；调用没落盘时才退化为独立 TOOL RESULT 气泡
    const parent = existing ? existing.closest(".message") : createMessage("tool", "", "TOOL RESULT").node;
    renderToolResult({call_id:message.tool_call_id,name:message.tool_name,result:message.content}, parent); return turn;
  }
  if (message.role === "workflow") {
    renderWorkflowEvent({type:"workflow_started"});
    (message.steps || []).forEach((step,index) => renderWorkflowEvent({type:"workflow_step",index,...step}));
    renderWorkflowEvent({type:"workflow_done",status:message.status,message:message.message});
  }
}
function finishHistoryTurn(turn) {
  if (!turn) return null;
  finishAssistant(turn);
  const usage = turn.usage;
  if (usage) renderTokenUsage(turn, {tokens: usage.total, tokens_input: usage.input, tokens_output: usage.output, tokens_estimated: !!usage.estimated});
  return null;
}
// 一条 user 消息之后、下一条 user/workflow 消息之前的 assistant/tool 消息属于同一轮
function renderHistory(messages) {
  let turn = null;
  (messages || []).forEach(message => {
    if (message.role === "assistant" || message.role === "tool") { turn = renderHistoryMessage(message, turn) || turn; return; }
    turn = finishHistoryTurn(turn); renderHistoryMessage(message, null);
  });
  finishHistoryTurn(turn);
}
function showWelcome() { el.messages.innerHTML = '<div id="welcome" class="empty-state"><div class="empty-symbol">⌁</div><strong>对话已就绪</strong><p>描述你的工程目标，Agent 将按当前模式执行。</p></div>'; el.welcome = $("#welcome"); }
async function loadSession(id) {
  if (state.busy && state.controller) state.controller.abort();
  try {
    state.session = await request(`/sessions/${encodeURIComponent(id)}`);
    el.currentTitle.textContent = state.session.meta.title;
    const sessionModel = state.models.find(item => modelKey(item) === state.session.meta.model_id || item.model_id === state.session.meta.model_id); if (sessionModel) selectModel(modelKey(sessionModel));
    el.messages.innerHTML = ""; el.phasePanel.classList.add("hidden"); state.workflow = null;
    if (!state.session.messages.length) showWelcome();
    else renderHistory(state.session.messages);
    if (state.session.plan) renderPhase(state.session.plan);
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
  const title = await showDialog({title:"重命名会话", input:session.title || "", confirmText:"重命名"}); if (!title || !title.trim()) return;
  try { await request(`/sessions/${encodeURIComponent(session.id)}/rename`, {method:"PUT",body:JSON.stringify({title:title.trim()})}); await refreshSessions(); if (state.session && state.session.meta.id === session.id) { state.session.meta.title = title.trim(); el.currentTitle.textContent = title.trim(); } } catch (error) { showToast(error.message); }
}
async function clearSession(session) {
  if (!(await showDialog({title:"清空会话", message:`将清空“${session.title}”的全部消息，此操作不可撤销。`, confirmText:"清空", danger:true}))) return;
  try { await request(`/sessions/${encodeURIComponent(session.id)}/clear`, {method:"POST",body:"{}"}); if (state.session && state.session.meta.id === session.id) await loadSession(session.id); await refreshSessions(); } catch (error) { showToast(error.message); }
}
async function deleteSession(session) {
  if (!(await showDialog({title:"删除会话", message:`将永久删除“${session.title}”，此操作不可撤销。`, confirmText:"删除", danger:true}))) return;
  try { await request(`/sessions/${encodeURIComponent(session.id)}`, {method:"DELETE"}); if (state.session && state.session.meta.id === session.id) { state.session = null; el.currentTitle.textContent = "选择会话"; showWelcome(); el.phasePanel.classList.add("hidden"); state.workflow = null; } await refreshSessions(); closeSessions(); updateSendState(); } catch (error) { showToast(error.message); }
}
function openSessions() { el.sessionPanel.classList.remove("hidden"); el.sessionTrigger.setAttribute("aria-expanded","true"); el.sessionSearch.focus(); }
function closeSessions() { el.sessionPanel.classList.add("hidden"); el.sessionTrigger.setAttribute("aria-expanded","false"); }

const FAILURE_HINTS = {
  rate_limited: "模型服务商限流",
  network: "后端连不上模型服务",
  stream_error: "模型流式响应中断",
  upstream_http: "模型服务返回错误",
  provider_error: "模型服务异常",
};
function streamFailure(event, fallback = "Agent 处理失败") {
  const failure = new Error(event.message || fallback);
  failure.stream = true;  // 后端回了结构化 error 事件，说明浏览器到后端这一段是通的
  failure.category = event.category || "";
  failure.retryable = event.retryable === true;
  const wait = Number(event.retry_after);
  failure.retryAfter = Number.isFinite(wait) && wait > 0 ? Math.ceil(wait) : 0;
  return failure;
}
function failureText(error) {
  if (!error.stream) return `${error.message || "请求失败"}\n请求没能走到模型这一步，确认后端服务在运行后重发。`;
  const lines = [`${FAILURE_HINTS[error.category] || "模型服务异常"}：${error.message}`];
  if (error.retryable) lines.push(error.retryAfter ? `这是瞬时故障，等 ${error.retryAfter} 秒后重发即可。` : "这是瞬时故障，可以直接重发。");
  else lines.push("这类错误不会自己恢复，先按上面的提示改参数或模型配置。");
  return lines.join("\n");
}
function renderFailure(error, retry) {
  const notice = createMessage("assistant", failureText(error), error.retryable ? "RETRY" : "ERROR");
  notice.bubble.style.borderColor = "var(--red)";
  if (retry) {
    const button = document.createElement("button");
    button.type = "button"; button.className = "action-button"; button.style.marginTop = "10px";
    button.textContent = "重发这条消息";
    button.addEventListener("click", () => {
      if (state.busy) { showToast("当前还有请求在处理中"); return; }
      button.disabled = true;
      sendMessage(retry.message, retry.display).finally(() => { button.disabled = false; });
    });
    notice.bubble.append(button);
  }
  if (!error.stream) setConnection("offline", "连接异常");
}
async function consumeSse(response, onEvent, controller) {
  if (!response.ok) { let message = `请求失败 (${response.status})`; try { const data = await response.json(); message = data.error || message; } catch (_) {} throw new Error(message); }
  if (!response.body) throw new Error("浏览器不支持流式响应");
  const reader = response.body.getReader(), decoder = new TextDecoder(); let buffer = "";
  if (controller) { controller.reader = reader; if (controller.aborted) { try { reader.cancel(); } catch (_) {} } }
  while (true) {
    const {value, done} = await reader.read(); buffer += decoder.decode(value || new Uint8Array(), {stream:!done}).replace(/\r\n/g,"\n");
    let boundary;
    while ((boundary = buffer.indexOf("\n\n")) >= 0) {
      const frame = buffer.slice(0,boundary); buffer = buffer.slice(boundary+2); let type = "message"; const dataLines = [];
      frame.split("\n").forEach(line => { if (line.startsWith("event:")) type = line.slice(6).trim(); else if (line.startsWith("data:")) dataLines.push(line.slice(5).replace(/^\s+/, "")); });
      if (dataLines.length) { try { await onEvent(type, JSON.parse(dataLines.join("\n"))); } catch (error) { if (error instanceof SyntaxError) showToast(`忽略无效 SSE 数据：${type}`); else throw error; } }
    }
    if (done) break;
  }
}
async function sendMessage(rawMessage = null, displayContent = null) {
  if (state.busy) return;
  const message = rawMessage != null ? rawMessage : el.input.value.trim(); if (!message) return;
  if (!state.session) { await createSession(); if (!state.session) return; }
  const shown = displayContent != null ? displayContent : message; createMessage("user", shown); el.input.value = ""; updateSendState();
  if ((state.session.meta.title || "").trim() === "New Session" && !state.session.messages.length) {
    const title = message.replace(/\s+/g, " ").trim().slice(0, 10);
    if (title) { try { await request(`/sessions/${encodeURIComponent(state.session.meta.id)}/rename`, {method:"PUT",body:JSON.stringify({title})}); state.session.meta.title = title; el.currentTitle.textContent = title; const entry = state.sessions.find(item => item.id === state.session.meta.id); if (entry) entry.title = title; renderSessions(); } catch (_) {} }
  }
  const skill = selectedSkill();
  const assistant = createMessage("assistant", "", skill ? (skill.name || skill.id) : ""); state.assistant = assistant;
  const controller = createAbortController(); state.controller = controller; setBusy(true);
  try {
    const selectedSkills = el.skill.value ? [{id:el.skill.value,params:{}}] : [];
    const response = await fetch("/chat/stream", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:state.session.meta.id,message,display_content:shown === message ? "" : shown,interaction_mode:state.mode,model_id:el.model.value || "",selected_skills:selectedSkills})});
    await consumeSse(response, async (type, event) => {
      if (type === "text_chunk") { assistant.text += event.delta || ""; assistant.body.innerHTML = basicMarkdown(assistant.text); scrollMessages(); }
      else if (type === "reasoning_chunk") appendReasoning(assistant,event.delta);
      else if (type === "plan_updated") renderPhase(event.plan);
      else if (type === "tool_call") renderToolCall(event,assistant.node);
      else if (type === "tool_result") renderToolResult(event,assistant.node);
      else if (type === "tool_approval_required") renderApproval(event,assistant.node);
      else if (type === "skill_loaded") { const label = assistant.bubble.querySelector(".message-label"); if (label) label.remove(); assistant.bubble.insertAdjacentHTML("afterbegin",`<div class="message-label">SKILL LOADED · ${escapeHtml(event.skill_id)}</div>`); }
      else if (type === "error") throw streamFailure(event);
      else if (type === "done") { finishAssistant(assistant); renderTokenUsage(assistant, event); }
    });
    finishAssistant(assistant); await refreshSessions();
  } catch (error) {
    finishAssistant(assistant);
    if (error.name === "AbortError") createMessage("assistant", "已停止接收当前响应。", "STOPPED");
    else renderFailure(error, {message, display: shown});
  } finally { if (state.controller === controller) state.controller = null; setBusy(false); }
}
async function runWorkflow(steps) {
  if (state.busy || !state.session) return;
  const controller = createAbortController(); state.controller = controller; state.workflow = null; setBusy(true);
  try {
    const response = await fetch("/workflows/run", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:state.session.meta.id,steps,selected_skills:el.skill.value?[{id:el.skill.value,params:{}}]:[]})});
    await consumeSse(response, async (type,event) => { event.type = type; if (["workflow_started","workflow_step","workflow_done"].includes(type)) renderWorkflowEvent(event); else if (type === "tool_approval_required") renderApproval(event,state.workflow.message.node); else if (type === "error") throw streamFailure(event, "工作流失败"); }, controller);
    await refreshSessions();
  } catch (error) { if (error.name !== "AbortError") showToast(failureText(error).replace(/\n/g, " · ")); }
  finally { if (state.controller === controller) state.controller = null; setBusy(false); }
}

const PROVIDER_PRESETS = {
  openai:{name:"OpenAI",base_url:"https://api.openai.com/v1",default_api:"openai-responses",discovery_api:"openai"},
  anthropic:{name:"Anthropic",base_url:"https://api.anthropic.com",default_api:"anthropic-messages",discovery_api:"anthropic"},
  compatible:{name:"OpenAI-compatible",base_url:"",default_api:"openai-chat",discovery_api:"openai"},
  ollama:{name:"Ollama",base_url:"http://localhost:11434/v1",default_api:"openai-chat",discovery_api:"openai"},
};
const CAPABILITIES = [["tools","Tools"],["parallel_tools","Parallel tools"],["reasoning","Reasoning"],["vision","Vision"],["stream_usage","Stream usage"]];
function clone(value) { return JSON.parse(JSON.stringify(value)); }
function draftProvider() { const draft = state.settings.draft; return draft && draft.providers ? draft.providers.find(item => item.id === state.settings.activeProviderId) : undefined; }
function providerModels(id = state.settings.activeProviderId) { const draft = state.settings.draft; return ((draft && draft.models) || []).filter(item => item.provider === id); }
function markSettingsDirty() { state.settings.dirty = true; el.settingsStatus.textContent = "有未保存的修改"; }
function field(label, name, value, type = "text", attrs = "") { return `<label class="settings-field"><span>${label}</span><input type="${type}" data-provider-field="${name}" value="${escapeHtml(value)}" ${attrs}></label>`; }
function modelField(label, name, value, type = "text", attrs = "") { return `<label class="settings-field"><span>${label}</span><input type="${type}" data-model-field="${name}" value="${escapeHtml(value)}" ${attrs}></label>`; }
function renderProviderList() {
  el.providerList.innerHTML = ""; const draft = state.settings.draft; ((draft && draft.providers) || []).forEach(provider => { const button = document.createElement("button"); button.type = "button"; button.className = provider.id === state.settings.activeProviderId ? "active" : ""; button.innerHTML = `<span><strong>${escapeHtml(provider.name)}</strong><small>${escapeHtml(provider.id)}</small></span><b>${providerModels(provider.id).length}</b>`; button.onclick = () => { state.settings.activeProviderId = provider.id; renderSettings(); }; el.providerList.append(button); });
}
function renderProviderEditor() {
  const provider = draftProvider(); if (!provider) { el.providerEditor.innerHTML = '<div class="placeholder-panel"><strong>添加供应商以开始配置</strong></div>'; return; }
  const models = providerModels(); const discovered = state.settings.discoveredModels[provider.id] || [];
  el.providerEditor.innerHTML = `<section class="provider-section"><div class="editor-title"><div><span class="eyebrow">PROVIDER</span><h3>${escapeHtml(provider.name)}</h3></div><label class="toggle"><input type="checkbox" data-provider-field="enabled" ${provider.enabled !== false ? "checked" : ""}><span>启用</span></label></div><div class="form-grid">${field("供应商名称","name",provider.name)}${field("供应商 ID","id",provider.id,"text","disabled")}<label class="settings-field"><span>供应商类型</span><select data-provider-field="discovery_api" ${models.length ? "disabled" : ""}><option value="openai">OpenAI / Compatible / Ollama</option><option value="anthropic">Anthropic</option><option value="none">不支持模型发现</option></select></label>${field("API 地址","base_url",provider.base_url || "","url")}${field("API Key 环境变量","api_key_env",provider.api_key_env || "")}${field("API Key","api_key",provider.api_key === "********" ? "" : provider.api_key || "","password",`placeholder="${provider.api_key === "********" ? "已安全保存" : "输入 API Key"}" ${provider.api_key_env ? "disabled" : ""}`)}<label class="settings-field"><span>默认 API 协议</span><select data-provider-field="default_api"><option value="openai-chat">OpenAI Chat</option><option value="openai-responses">OpenAI Responses</option><option value="anthropic-messages">Anthropic Messages</option></select></label></div><div class="provider-actions"><button type="button" class="secondary" data-action="clear-key">清除 Key</button><button type="button" class="secondary" data-action="test-provider">${state.settings.testingProviderId === provider.id ? "测试中…" : "测试连接"}</button><span class="inline-result" data-result="test"></span><button type="button" class="danger-button" data-action="delete-provider">删除供应商</button></div></section><section class="provider-section models-section"><div class="section-head"><div><span class="eyebrow">MODELS</span><h3>已添加模型 <small>${models.length}</small></h3></div><button type="button" class="secondary" data-action="read-models">${state.settings.readingProviderId === provider.id ? "读取中…" : "读取模型"}</button></div><div class="model-settings-list"></div><div class="add-model"><label><span>添加模型 · 搜索候选</span><input type="search" id="model-candidate-search" placeholder="搜索模型 ID"></label><div id="model-candidates" class="model-candidates"></div><div class="manual-model"><input id="manual-model-id" placeholder="手动输入模型 ID"><button type="button" class="primary" data-action="add-manual-model">添加</button></div></div></section>`;
  el.providerEditor.querySelector('[data-provider-field="default_api"]').value = provider.default_api;
  el.providerEditor.querySelector('[data-provider-field="discovery_api"]').value = provider.discovery_api || "openai";
  el.providerEditor.querySelectorAll("[data-provider-field]").forEach(input => input.onchange = () => { const name = input.dataset.providerField; if (name !== "api_key" || input.value || provider.api_key !== "********") provider[name] = input.type === "checkbox" ? input.checked : input.value; markSettingsDirty(); renderProviderList(); });
  const list = $(".model-settings-list",el.providerEditor); models.forEach(model => renderModelCard(model,list));
  renderCandidates(discovered); $("#model-candidate-search",el.providerEditor).oninput = event => renderCandidates(discovered,event.target.value);
  $("[data-action='clear-key']",el.providerEditor).onclick = () => { provider.api_key = ""; markSettingsDirty(); renderProviderEditor(); };
  $("[data-action='test-provider']",el.providerEditor).onclick = testProvider; $("[data-action='read-models']",el.providerEditor).onclick = readProviderModels; $("[data-action='delete-provider']",el.providerEditor).onclick = deleteProvider; $("[data-action='add-manual-model']",el.providerEditor).onclick = () => addModel($("#manual-model-id",el.providerEditor).value);
}
function renderModelCard(model, parent) {
  const card = document.createElement("details"); card.className = "settings-model"; card.innerHTML = `<summary><span><strong>${escapeHtml(model.name || model.id)}</strong><small>${escapeHtml(model.id)}</small></span><code>${escapeHtml(model.api || "继承供应商")}</code><button type="button" class="model-delete" aria-label="删除模型">×</button></summary><div class="model-advanced"><div class="form-grid">${modelField("显示名称","name",model.name || "")}${modelField("Context window","context_window",model.context_window || 32768,"number",'min="1"')}${modelField("Max output tokens","max_output_tokens",model.max_output_tokens || 4096,"number",'min="1"')}<label class="settings-field"><span>API 协议覆盖</span><select data-model-field="api"><option value="">继承供应商</option><option value="openai-chat">OpenAI Chat</option><option value="openai-responses">OpenAI Responses</option><option value="anthropic-messages">Anthropic Messages</option></select></label></div><div class="capability-grid">${CAPABILITIES.map(([key,label]) => `<label><input type="checkbox" data-capability="${key}" ${model.capabilities && model.capabilities[key] ? "checked" : ""}> ${label}</label>`).join("")}</div></div>`;
  card.querySelector('[data-model-field="api"]').value = model.api || ""; card.querySelectorAll("[data-model-field]").forEach(input => input.onchange = () => { model[input.dataset.modelField] = input.type === "number" ? Number(input.value) : (input.value || null); markSettingsDirty(); }); card.querySelectorAll("[data-capability]").forEach(input => input.onchange = () => { if (!model.capabilities) model.capabilities = {}; model.capabilities[input.dataset.capability] = input.checked; markSettingsDirty(); }); $(".model-delete",card).onclick = event => { event.preventDefault(); const deletedKey = `${model.provider}/${model.id}`; state.settings.draft.models = state.settings.draft.models.filter(item => item !== model); if (state.settings.draft.default_model === deletedKey) { const fallback = state.settings.draft.models.find(item => item.enabled !== false); state.settings.draft.default_model = fallback ? `${fallback.provider}/${fallback.id}` : ""; } markSettingsDirty(); renderSettings(); }; parent.append(card);
}
function renderCandidates(candidates, query = "") { const root = $("#model-candidates",el.providerEditor); if (!root) return; const added = new Set(providerModels().map(item => item.id)); root.innerHTML = ""; candidates.filter(item => !query || String(item.id || item.model_id).toLowerCase().includes(query.toLowerCase())).forEach(item => { const id = item.id || item.model_id, button = document.createElement("button"); button.type = "button"; button.disabled = added.has(id); button.innerHTML = `<span>${added.has(id) ? "✓" : "+"}</span><strong>${escapeHtml(item.name || id)}</strong><small>${escapeHtml(id)}</small>`; button.onclick = () => addModel(id,item.name); root.append(button); }); if (!root.children.length) root.innerHTML = '<div class="listbox-empty">暂无候选，可手动添加</div>'; }
function addModel(rawId, name = "") { const id = String(rawId || "").trim(); if (!id) { el.settingsStatus.textContent = "模型 ID 不得为空"; return; } if (providerModels().some(item => item.id === id)) { el.settingsStatus.textContent = "该模型已添加"; return; } const model = {id,provider:state.settings.activeProviderId,api:null,name:name || id,enabled:true,context_window:32768,max_output_tokens:4096,capabilities:{tools:false,parallel_tools:false,reasoning:false,vision:false,stream_usage:false},compat:{}}; state.settings.draft.models.push(model); if (!state.settings.draft.default_model) state.settings.draft.default_model = `${model.provider}/${model.id}`; markSettingsDirty(); renderSettings(); }
function renderSettings() { renderProviderList(); renderProviderEditor(); }
function switchSettingsTab(tab) { state.settings.activeTab = tab; document.querySelectorAll("[data-settings-tab]").forEach(button => { const active = button.dataset.settingsTab === tab; button.setAttribute("aria-selected",String(active)); $(`#panel-${button.dataset.settingsTab}`).classList.toggle("hidden",!active); }); el.saveSettings.classList.toggle("hidden",tab !== "models"); if (tab === "mcp" && !state.mcp.loaded) loadMcpTools(); if (tab === "skills") renderSkills(); }
function schemaParams(schema) {
  // 从 JSON Schema 提取入参摘要，用于工具列表展示每个工具的参数
  const props = (schema && schema.properties) || {}, required = new Set((schema && schema.required) || []);
  return Object.keys(props).map(name => { const p = props[name] || {}; return {name,type:p.type || "any",required:required.has(name),description:p.description || ""}; });
}
function renderMcpTools() {
  const {tools,connected,error,loading} = state.mcp;
  if (el.mcpCount) el.mcpCount.textContent = String(tools.length);
  el.mcpStatus.textContent = loading && !tools.length ? "正在读取 MCP 工具…" : (error ? `MCP 读取失败：${error}` : (connected ? `已连接 · 共 ${tools.length} 个工具` : "MCP 服务未连接"));
  el.mcpTools.innerHTML = "";
  if (loading && !tools.length) return;
  if (!tools.length) { el.mcpTools.innerHTML = '<div class="listbox-empty">暂无可用工具</div>'; return; }
  tools.forEach(tool => {
    const params = schemaParams(tool.input_schema), card = document.createElement("details"); card.className = "mcp-tool";
    card.innerHTML = `<summary><strong>${escapeHtml(tool.name)}</strong><span class="mcp-param-count">${params.length} 参数</span></summary>${tool.description ? `<p class="mcp-tool-desc">${escapeHtml(tool.description)}</p>` : ""}${params.length ? `<ul class="mcp-param-list">${params.map(p => `<li><code>${escapeHtml(p.name)}</code><span class="mcp-param-type">${escapeHtml(p.type)}</span>${p.required ? '<span class="mcp-param-required">必填</span>' : ""}${p.description ? `<span class="mcp-param-desc">${escapeHtml(p.description)}</span>` : ""}</li>`).join("")}</ul>` : '<p class="mcp-tool-desc">无参数</p>'}`;
    el.mcpTools.append(card);
  });
}
async function loadMcpTools(refresh = false) {
  if (state.mcp.loading) return;
  state.mcp.loading = true; if (el.refreshMcp) el.refreshMcp.disabled = true; renderMcpTools();
  try {
    const data = await request(refresh ? "/mcp/tools?refresh=1" : "/mcp/tools");
    state.mcp.tools = data.tools || []; state.mcp.connected = Boolean(data.connected); state.mcp.error = data.error || ""; state.mcp.loaded = true;
  } catch (error) { state.mcp.tools = []; state.mcp.connected = false; state.mcp.error = error.message; }
  finally { state.mcp.loading = false; if (el.refreshMcp) el.refreshMcp.disabled = false; renderMcpTools(); }
}
function renderSkills() {
  // 设置页「技能」列表：展示后端已注册技能的名称、来源、版本、描述与可用工具
  const skills = state.skills || [];
  if (el.skillCount) el.skillCount.textContent = String(skills.length);
  if (el.skillsStatus) el.skillsStatus.textContent = state.skillsLoading ? "正在读取技能…" : (state.skillsError ? `技能读取失败：${state.skillsError}` : `共 ${skills.length} 个技能`);
  el.skillsList.innerHTML = "";
  if (state.skillsLoading && !skills.length) return;
  if (!skills.length) { el.skillsList.innerHTML = '<div class="listbox-empty">暂无可用技能</div>'; return; }
  skills.forEach(skill => {
    const tools = skill.allowed_tools || [], shadowed = skill.shadowed || [], card = document.createElement("details"); card.className = "skill-card";
    card.innerHTML = `<summary><strong>${escapeHtml(skill.name || skill.id)}</strong>${skill.version ? `<span class="skill-version">v${escapeHtml(skill.version)}</span>` : ""}<span class="skill-source">${escapeHtml(skill.source || "")}</span><span class="skill-tool-count">${tools.length} 工具</span></summary>`
      + (skill.description ? `<p class="skill-desc">${escapeHtml(skill.description)}</p>` : "")
      + `<div class="skill-meta">ID <code>${escapeHtml(skill.id)}</code></div>`
      + (tools.length ? `<ul class="skill-tool-list">${tools.map(name => `<li><code>${escapeHtml(name)}</code></li>`).join("")}</ul>` : '<p class="skill-desc">未限定可用工具</p>')
      + (shadowed.length ? `<p class="skill-shadowed">覆盖了 ${shadowed.length} 个同名来源：${shadowed.map(item => escapeHtml(`${item.source || ""}${item.version ? " v" + item.version : ""}`)).join("、")}</p>` : "");
    el.skillsList.append(card);
  });
}
async function loadSkills() {
  if (state.skillsLoading) return;
  state.skillsLoading = true; state.skillsError = ""; if (el.refreshSkills) el.refreshSkills.disabled = true; renderSkills();
  try {
    const data = await request("/skills");
    state.skills = data.skills || [];
    if (el.skill.value && !selectedSkill()) selectSkill("");
  } catch (error) { state.skillsError = error.message; }
  finally { state.skillsLoading = false; if (el.refreshSkills) el.refreshSkills.disabled = false; renderSkills(); }
}
async function openSettings() { try { const data = await request("/config"); const config = data.config || {version:1,default_model:"",providers:[],models:[]}; state.settings.original = clone(config); state.settings.draft = clone(config); if (!state.settings.draft.providers) state.settings.draft.providers = []; if (!state.settings.draft.models) state.settings.draft.models = []; if (!state.settings.draft.default_model) state.settings.draft.default_model = ""; state.settings.revision = data.revision || config.revision || null; delete state.settings.draft.revision; const firstProvider = state.settings.draft.providers[0]; state.settings.activeProviderId = firstProvider ? firstProvider.id : null; state.settings.open = true; state.settings.dirty = false; state.settings.discoveredModels = {}; el.settingsStatus.textContent = ""; el.settingsModal.classList.remove("hidden"); switchSettingsTab("models"); renderSettings(); el.closeSettings.focus(); } catch (error) { showToast(error.message); } }
function closeSettings(force = false) { if (!state.settings.open) return; if (!force && state.settings.dirty) { showDialog({title:"放弃未保存的设置", message:"模型设置尚未保存，确定要关闭吗？", confirmText:"放弃更改", danger:true}).then(ok => { if (ok) closeSettings(true); }); return; } Object.values(state.settings.controllers).forEach(controller => controller.abort()); state.settings.open = false; state.settings.original = state.settings.draft = null; state.settings.discoveredModels = {}; el.settingsModal.classList.add("hidden"); el.openSettings.focus(); }
function providerPayload(provider) { return {provider:{...provider,headers:provider.headers || {},discover_models:provider.discover_models !== false}}; }
async function testProvider() { const provider = draftProvider(), id = provider.id, controller = createAbortController(); if (state.settings.controllers.test) state.settings.controllers.test.abort(); state.settings.controllers.test = controller; state.settings.testingProviderId = id; renderProviderEditor(); try { const data = await request("/config/providers/test",{method:"POST",signal:controller.signal,body:JSON.stringify(providerPayload(provider))}); if (state.settings.activeProviderId === id) el.settingsStatus.textContent = data.message || `连接成功 · ${data.latency_ms || 0}ms`; } catch(error) { if (error.name !== "AbortError" && state.settings.activeProviderId === id) el.settingsStatus.textContent = error.message; } finally { if (state.settings.testingProviderId === id) { state.settings.testingProviderId = null; renderProviderEditor(); } } }
async function readProviderModels() { const provider = draftProvider(), id = provider.id, controller = createAbortController(); if (state.settings.controllers.models) state.settings.controllers.models.abort(); state.settings.controllers.models = controller; state.settings.readingProviderId = id; renderProviderEditor(); try { const data = await request("/config/providers/models",{method:"POST",signal:controller.signal,body:JSON.stringify(providerPayload(provider))}); state.settings.discoveredModels[id] = [...new Map((data.models || []).map(item => [item.id || item.model_id,item])).values()].sort((a,b) => String(a.id || a.model_id).localeCompare(String(b.id || b.model_id))); if (state.settings.activeProviderId === id) renderProviderEditor(); } catch(error) { if (error.name !== "AbortError" && state.settings.activeProviderId === id) el.settingsStatus.textContent = error.message; } finally { if (state.settings.readingProviderId === id) { state.settings.readingProviderId = null; renderProviderEditor(); } } }
function addProvider() { if (!state.settings.draft.providers) state.settings.draft.providers = []; const providers = state.settings.draft.providers; let id = "custom", suffix = 2; while (providers.some(item => item.id === id)) id = `custom-${suffix++}`; providers.push({id,...PROVIDER_PRESETS.compatible,name:"新供应商",api_key:"",api_key_env:"",headers:{},discover_models:true,enabled:true}); state.settings.activeProviderId = id; markSettingsDirty(); renderSettings(); const nameInput = el.providerEditor.querySelector('[data-provider-field="name"]'); if (nameInput) { nameInput.focus(); nameInput.select(); } }
function deleteProvider() { const provider = draftProvider(), models = providerModels(); if (models.length) { el.settingsStatus.textContent = "请先删除该供应商下的模型"; return; } const defaultModel = state.settings.draft.default_model; if (defaultModel && defaultModel.startsWith(`${provider.id}/`)) { el.settingsStatus.textContent = "默认模型属于该供应商，无法删除"; return; } state.settings.draft.providers = state.settings.draft.providers.filter(item => item !== provider); const nextProvider = state.settings.draft.providers[0]; state.settings.activeProviderId = nextProvider ? nextProvider.id : null; markSettingsDirty(); renderSettings(); }
function validateSettings() { const errors = []; for (const provider of state.settings.draft.providers) { try { const url = new URL(provider.base_url); if (!["http:","https:"].includes(url.protocol)) errors.push(`${provider.name} 的 API 地址无效`); } catch (_) { errors.push(`${provider.name} 的 API 地址无效`); } } for (const model of state.settings.draft.models) { if (model.context_window < model.max_output_tokens) errors.push(`${model.id} 的 context window 不能小于最大输出`); } const enabledModels = state.settings.draft.models.filter(item => item.enabled !== false); if (!enabledModels.some(item => `${item.provider}/${item.id}` === state.settings.draft.default_model)) { const fallback = enabledModels[0]; if (fallback) state.settings.draft.default_model = `${fallback.provider}/${fallback.id}`; else errors.push("请至少添加一个已启用模型"); } el.settingsStatus.textContent = errors[0] || ""; return !errors.length; }
async function saveSettings() { if (!validateSettings()) return; el.saveSettings.disabled = true; try { const payload = {revision:state.settings.revision,config:state.settings.draft}; const data = await request("/config",{method:"POST",body:JSON.stringify(payload)}); state.settings.dirty = false; closeSettings(true); state.configLoaded = true; el.warning.classList.add("hidden"); await refreshModels(null, data.config && data.config.default_model); updateSendState(); showToast("模型设置已保存"); } catch(error) { el.settingsStatus.textContent = error.status === 409 ? "配置已被其他窗口修改，请关闭后重新加载" : error.message; } finally { el.saveSettings.disabled = false; } }
async function refreshModels(models = null, defaultModel = null) { if (!models) { const data = await request("/config/models"); models = data.models || []; defaultModel = data.default_model || defaultModel; } const previous = el.model.value; state.models = models; const keys = visibleModels().map(modelKey); selectModel(keys.includes(previous) ? previous : (defaultModel && keys.includes(defaultModel) ? defaultModel : keys[0] || "")); }

async function bootstrap() {
  setConnection("checking","连接中");
  const results = await Promise.all([settle(request("/health")),settle(request("/config/models")),settle(request("/skills")),settle(request("/sessions"))]);
  const [health,models,skills,sessions] = results;
  if (health.status === "fulfilled") { state.configLoaded = Boolean(health.value.config_loaded); setConnection("online","Agent 已连接"); }
  else setConnection("offline","服务不可用");
  if (models.status === "fulfilled") {
    state.models = models.value.models || [];
    selectModel(models.value.default_model || "");
  }
  if (skills.status === "fulfilled") {
    state.skills = skills.value.skills || [];
    if (el.skill.value && !selectedSkill()) selectSkill("");
  }
  if (sessions.status === "fulfilled") { state.sessions = sessions.value.sessions || []; renderSessions(); if (state.sessions[0]) await loadSession(state.sessions[0].id); else showWelcome(); }
  const failures = results.filter(item => item.status === "rejected"); if (failures.length) showToast(failures[0].reason.message);
  el.warning.classList.toggle("hidden",state.configLoaded); updateSendState();
}
el.newSession.onclick = createSession;
el.modelTrigger.onclick = () => state.modelListOpen ? closeModelList() : openModelList(); el.modelTrigger.onkeydown = handleModelKeys;
el.skillTrigger.onclick = () => state.skillListOpen ? closeSkillList() : openSkillList();
el.openSettings.onclick = openSettings; el.closeSettings.onclick = () => closeSettings(); el.cancelSettings.onclick = () => closeSettings(); el.saveSettings.onclick = saveSettings; el.addProvider.onclick = addProvider;
document.querySelectorAll("[data-settings-tab]").forEach(button => button.onclick = () => switchSettingsTab(button.dataset.settingsTab));
if (el.refreshMcp) el.refreshMcp.onclick = () => loadMcpTools(true);
if (el.refreshSkills) el.refreshSkills.onclick = () => loadSkills();
el.sessionTrigger.onclick = () => el.sessionPanel.classList.contains("hidden") ? openSessions() : closeSessions();
el.closeSessions.onclick = closeSessions; el.sessionSearch.oninput = renderSessions;
el.connection.onclick = bootstrap;
el.send.onclick = () => { if (state.busy && state.controller) state.controller.abort(); else sendMessage(); };
el.input.oninput = updateSendState;
el.input.onkeydown = event => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); if (!state.busy && !el.send.disabled) sendMessage(); } };
document.querySelectorAll("[data-mode]").forEach(button => button.onclick = () => { state.mode = button.dataset.mode; document.querySelectorAll("[data-mode]").forEach(item => item.classList.toggle("active",item === button)); });
document.addEventListener("click", event => { if (state.modelListOpen && !event.target.closest(".model-control:not(.skill-control)")) closeModelList(); if (state.skillListOpen && !event.target.closest(".skill-control")) closeSkillList(); if (!el.sessionPanel.classList.contains("hidden") && !event.target.closest("#session-panel,#session-trigger,.dialog-backdrop")) closeSessions(); });
document.addEventListener("keydown", event => { if (event.key === "Escape") { if (state.settings.open) closeSettings(); else { closeModelList(); closeSkillList(); closeSessions(); } } if (event.key === "Tab" && state.settings.open) { const focusable = [...el.settingsModal.querySelectorAll('button:not([disabled]),input:not([disabled]),select:not([disabled]),details summary')].filter(item => item.offsetParent !== null); if (focusable.length && ((event.shiftKey && document.activeElement === focusable[0]) || (!event.shiftKey && document.activeElement === focusable[focusable.length - 1]))) { event.preventDefault(); focusable[event.shiftKey ? focusable.length - 1 : 0].focus(); } } });
bootstrap();