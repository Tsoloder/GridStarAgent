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
  workflow: null, phasePlan: null, configLoaded: false,
  // 流式状态按会话隔离（支持多会话真并发）：
  // controllers: id → AbortController；streams: id → {hidden:切走后 DOM 已摘下暂存, workflow:工作流流}；
  // status: id → 徽标状态 running|done|stopped|error（done 常驻，该会话下次发消息时刷新）；
  // stashed: 切走会话时摘下暂存的消息 DOM，切回后原样挂回继续实时更新
  controllers: new Map(), streams: new Map(), status: new Map(), stashed: new Map(),
  modelListOpen: false, modelOptionIndex: -1, modelSearch: "",
  attachments: [], uploading: 0,
  settings: {open:false,activeTab:"models",activeProviderId:null,original:null,draft:null,revision:null,dirty:false,testingProviderId:null,readingProviderId:null,discoveredModels:{},validationErrors:{},controllers:{}},
  mcp: {tools:[],loaded:false,loading:false,connected:false,error:""},
  skillsLoading: false, skillsError: "",
  viewTab: "chat",
  traj: {events:[], keys:{}, count:{}, records:[], view:"", query:"", selected:null, range:null, scale:null, spans:[], inspectorTab:"overview", loading:false, renderPending:false, collapsed:{}, actualDuration:false},
};
const el = {
  connection: $("#connection"), newSession: $("#new-session"), sessionTrigger: $("#session-trigger"),
  sessionPanel: $("#session-panel"), sessionSearch: $("#session-search"), sessionList: $("#session-list"),
  closeSessions: $("#close-sessions"), currentTitle: $("#current-title"), messages: $("#messages"),
  welcome: $("#welcome"), phasePanel: $("#phase-panel"), model: $("#model-select"), modelTrigger: $("#model-trigger"), modelLabel: $("#model-label"), modelListbox: $("#model-listbox"), skill: $("#skill-select"), skillTrigger: $("#skill-trigger"), skillLabel: $("#skill-label"), skillListbox: $("#skill-listbox"),
  input: $("#message-input"), send: $("#send"), voiceBtn: $("#voice-btn"), busyLabel: $("#busy-label"), warning: $("#config-warning"), toast: $("#toast"), choiceOverlay: $("#choice-overlay"), composer: $(".composer"),
  attachBar: $("#attach-bar"), attachBtn: $("#attach-btn"), fileInput: $("#file-input"), dropOverlay: $("#drop-overlay"),
  openSettings: $("#open-settings"), settingsModal: $("#settings-modal"), closeSettings: $("#close-settings"), cancelSettings: $("#cancel-settings"), saveSettings: $("#save-settings"), settingsStatus: $("#settings-status"), providerList: $("#provider-list"), providerEditor: $("#provider-editor"), addProvider: $("#add-provider"),
  mcpTools: $("#mcp-tools"), mcpCount: $("#mcp-count"), mcpStatus: $("#mcp-status"), refreshMcp: $("#refresh-mcp"),
  skillsList: $("#skills-list"), skillCount: $("#skill-count"), skillsStatus: $("#skills-status"), refreshSkills: $("#refresh-skills"),
  tabChat: $("#tab-chat"), tabTraj: $("#tab-trajectory"), trajView: $("#trajectory-view"),
  trajLedger: $("#traj-ledger"), trajInspector: $("#traj-inspector"), trajSearch: $("#traj-search"),
  trajTimeline: $("#traj-timeline"), composer: $(".composer"),
  turnRail: $("#turn-rail"), turnRailPanel: $("#turn-rail-panel"), turnRailList: $("#turn-rail-list"), turnRailCount: $("#turn-rail-count"),
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
// 气泡时间：HH:MM:SS；无效/空值返回空串（老会话没有 ts 时不显示）
function formatClock(value) {
  if (!value) return "";
  const date = value instanceof Date ? value : new Date(value);
  if (isNaN(date.getTime())) return "";
  const pad = n => (n < 10 ? "0" : "") + n;
  return pad(date.getHours()) + ":" + pad(date.getMinutes()) + ":" + pad(date.getSeconds());
}
// 气泡用时：紧凑格式（dsh 风格）
function formatDuration(ms) {
  if (ms == null || isNaN(ms) || ms < 0) return "";
  if (ms < 1000) return Math.round(ms) + "ms";
  if (ms < 60000) return (ms / 1000).toFixed(1) + "s";
  const m = Math.floor(ms / 60000), s = Math.round((ms % 60000) / 1000);
  return m + "m" + (s < 10 ? "0" : "") + s + "s";
}
// 剪贴板：优先 async API，失败回退 execCommand（兼容旧内核/非安全上下文）
function copyText(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    return navigator.clipboard.writeText(text);
  }
  const area = document.createElement("textarea");
  area.value = text;
  area.style.cssText = "position:fixed;top:0;left:0;opacity:0";
  document.body.append(area);
  area.select();
  try { document.execCommand("copy"); } catch (_) {}
  area.remove();
  return Promise.resolve();
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
// 用量明细的「提供方 / 模型」：后端回传的是 provider ID（如 custom），这里换成用户配置的供应商名称
function usageModelLabel(key) {
  const raw = String(key || "").trim();
  if (!raw) return "";
  const slash = raw.indexOf("/");
  const providerId = slash > 0 ? raw.slice(0, slash) : "";
  const modelId = slash > 0 ? raw.slice(slash + 1) : raw;
  if (!providerId) return modelId;
  const hit = state.models.find(item => item.provider === providerId);
  const providerName = (hit && hit.provider_name) || providerId;
  return `${providerName} / ${modelId}`;
}
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
function currentId() { return state.session ? state.session.meta.id : null; }
// 当前会话是否有进行中的流：发送按钮的"停止/发送"形态只跟随当前会话
function isBusy() { const id = currentId(); return Boolean(id && state.controllers.has(id)); }
// 发送/停止两种形态用同一套描边图标，避免字体字形与 SVG 混在一起（笔画粗细对齐卡片拷贝按钮）
const SEND_ICON = '<svg class="send-icon" viewBox="0 0 24 24" width="16" height="16" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M12 19V5"/><path d="M5 12l7-7 7 7"/></svg>';
const STOP_ICON = '<svg class="send-icon" viewBox="0 0 24 24" width="16" height="16" aria-hidden="true" fill="currentColor" stroke="none"><rect x="7" y="7" width="10" height="10" rx="1.5"/></svg>';
function syncComposer() {
  const busy = isBusy();
  el.send.classList.toggle("stop", busy);
  el.send.innerHTML = busy ? STOP_ICON : SEND_ICON;
  el.send.title = busy ? "停止接收" : "发送";
  el.send.setAttribute("aria-label", el.send.title);
  el.input.disabled = busy;
  el.busyLabel.textContent = busy ? "Agent 正在处理…" : "";
  updateSendState();
}
// 点「停止」：断开 SSE 之外还要取消后端任务，否则 agent 会继续跑、继续消耗 token
function stopSession(id) {
  const controller = state.controllers.get(id);
  if (controller) controller.abort();
  request(`/sessions/${encodeURIComponent(id)}/cancel`, {method:"POST",body:"{}"}).catch(() => {});
}
// 会话列表徽标：前端实时状态优先；刷新页面后靠服务端 active 字段兜底
const STATUS_TEXT = {running:"进行中", done:"已完成", stopped:"已停止", error:"异常", waiting:"待确认"};
function setStatus(id, status) {
  if (!id) return;
  state.status.set(id, status);
  renderSessions();
}
function sessionStatus(session) {
  return state.status.get(session.id) || (session.waiting ? "waiting" : session.active ? "running" : "");
}

function updateSendState() {
  const hasPayload = Boolean(el.input.value.trim()) || state.attachments.length > 0;
  el.send.disabled = !isBusy() && (!hasPayload || !state.configLoaded || state.uploading > 0);
}

// 输入框随内容行数增高；到上限后锁定高度并改为可滚动
const INPUT_MAX_HEIGHT = 200;
function autoGrowInput() {
  const node = el.input;
  node.style.height = "auto";
  const full = node.scrollHeight;
  node.style.height = `${Math.min(full, INPUT_MAX_HEIGHT)}px`;
  node.style.overflowY = full > INPUT_MAX_HEIGHT ? "auto" : "hidden";
}

// --- 附件：拖拽/选择文件 → POST /upload → 芯片列表 → 随消息一起发给大模型 ---
const ATTACH_MAX_BYTES = 10 * 1024 * 1024;
const ATTACH_MAX_COUNT = 6;
const ATTACH_MAX_IMAGES = 4;
const ATTACH_DOC_EXT = ["txt","md","markdown","csv","json","xml","log","html","htm","yaml","yml","py","js","ts","css","sql","sh","bat","ini","conf","pdf","doc","docx","xls","xlsx"];
const ATTACH_IMG_EXT = ["png","jpg","jpeg","gif","webp","bmp"];
let attachSeq = 0;
function attachExt(name) {
  const text = String(name || ""); const dot = text.lastIndexOf(".");
  return dot > 0 ? text.slice(dot + 1).toLowerCase() : "";
}
function attachKind(name) {
  const ext = attachExt(name);
  if (ATTACH_IMG_EXT.includes(ext)) return "image";
  if (ATTACH_DOC_EXT.includes(ext)) return "text";
  return "";
}
function attachSizeLabel(size) {
  const bytes = Number(size) || 0;
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MB";
  if (bytes >= 1024) return Math.round(bytes / 1024) + " KB";
  return bytes + " B";
}
// Chromium 65 下 fetch 上传大文件不便，用 XHR 包一层 Promise
function uploadFile(file) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/upload?name=" + encodeURIComponent(file.name), true);
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        try { resolve(JSON.parse(xhr.responseText)); } catch (_) { reject(new Error("上传响应解析失败")); }
        return;
      }
      let message = "上传失败 (" + xhr.status + ")";
      try { const data = JSON.parse(xhr.responseText); if (data && data.error) message = data.error; } catch (_) {}
      reject(new Error(message));
    };
    xhr.onerror = () => reject(new Error("上传失败：网络错误"));
    xhr.send(file);
  });
}
function findAttachment(id) { return state.attachments.filter(item => item.id === id)[0] || null; }
function removeAttachment(id) {
  const index = state.attachments.findIndex(item => item.id === id);
  if (index < 0) return;
  const item = state.attachments[index];
  if (item.previewUrl && window.URL && URL.revokeObjectURL) { try { URL.revokeObjectURL(item.previewUrl); } catch (_) {} }
  state.attachments.splice(index, 1);
  renderAttachments(); updateSendState();
}
function clearAttachments() {
  state.attachments.slice().forEach(item => { if (item.previewUrl && window.URL && URL.revokeObjectURL) { try { URL.revokeObjectURL(item.previewUrl); } catch (_) {} } });
  state.attachments = []; renderAttachments(); updateSendState();
}
// 待发送附件芯片条（含上传进度态与移除按钮）
function renderAttachments() {
  const bar = el.attachBar; if (!bar) return;
  if (!state.attachments.length) { bar.innerHTML = ""; bar.classList.add("hidden"); return; }
  bar.classList.remove("hidden");
  bar.innerHTML = state.attachments.map(item => {
    const thumb = item.kind === "image" && (item.previewUrl || item.url)
      ? `<img src="${escapeHtml(item.previewUrl || item.url)}" alt="">`
      : `<span class="attach-ext">${escapeHtml(attachExt(item.name) || "file")}</span>`;
    const status = item.uploading ? " uploading" : "";
    return `<span class="attach-chip${status}" data-id="${item.id}" title="${escapeHtml(item.name)}">${thumb}<span class="attach-name">${escapeHtml(item.name)}</span><span class="attach-size">${item.uploading ? "上传中…" : escapeHtml(attachSizeLabel(item.size))}</span><button type="button" class="attach-remove" data-remove="${item.id}" aria-label="移除附件">×</button></span>`;
  }).join("");
}
async function addFiles(fileList) {
  const files = Array.prototype.slice.call(fileList || []);
  for (const file of files) {
    if (state.attachments.length >= ATTACH_MAX_COUNT) { showToast("最多添加 " + ATTACH_MAX_COUNT + " 个附件"); break; }
    const kind = attachKind(file.name);
    if (!kind) { showToast("不支持的文件类型：" + file.name); continue; }
    if (file.size > ATTACH_MAX_BYTES) { showToast("文件超过 10MB：" + file.name); continue; }
    if (kind === "image" && state.attachments.filter(item => item.kind === "image").length >= ATTACH_MAX_IMAGES) { showToast("最多添加 " + ATTACH_MAX_IMAGES + " 张图片"); continue; }
    const item = {
      id: ++attachSeq, kind, name: file.name, size: file.size, path: "", url: "", media_type: "", uploading: true,
      previewUrl: kind === "image" && window.URL && URL.createObjectURL ? URL.createObjectURL(file) : "",
    };
    state.attachments.push(item); renderAttachments();
    state.uploading += 1; updateSendState();
    try {
      const data = await uploadFile(file);
      item.path = data.path || ""; item.url = data.url || ""; item.media_type = data.media_type || "";
      if (data.kind) item.kind = data.kind;
    } catch (error) {
      removeAttachment(item.id);
      showToast((error && error.message) || ("上传失败：" + file.name));
      continue;
    } finally { state.uploading = Math.max(0, state.uploading - 1); }
    item.uploading = false; renderAttachments(); updateSendState();
  }
}
// 气泡内已发送/历史附件展示（图片给缩略图，文档给文件名芯片）
function renderMessageAttachments(bubble, attachments) {
  const list = (attachments || []).filter(item => item && typeof item === "object");
  if (!bubble || !list.length) return;
  const wrap = document.createElement("div");
  wrap.className = "bubble-attachments";
  wrap.innerHTML = list.map(item => {
    const name = item.name || "附件";
    if (item.kind === "image" && item.url) return `<a class="attach-thumb" href="${escapeHtml(item.url)}" target="_blank" rel="noopener noreferrer" title="${escapeHtml(name)}"><img src="${escapeHtml(item.url)}" alt="${escapeHtml(name)}"></a>`;
    return `<span class="attach-file" title="${escapeHtml(name)}"><span class="attach-ext">${escapeHtml(attachExt(name) || "file")}</span>${escapeHtml(name)}</span>`;
  }).join("");
  bubble.insertBefore(wrap, bubble.firstChild);
}
function dragHasFiles(event) {
  const types = event.dataTransfer && event.dataTransfer.types;
  if (!types) return false;
  for (let i = 0; i < types.length; i++) { if (types[i] === "Files" || types[i] === "files") return true; }
  return false;
}

// 行内：先整行转义，再把代码段换成占位符（否则代码里的 ** / _ 会被二次解析），
// 之后依次解析图片、链接、粗体、删除线、斜体，最后把代码段还原
const MD_CODE_TOKEN = "\u0001";
function mdHref(raw) {
  const href = String(raw || "").trim();
  if (/^(?:https?:|mailto:)/i.test(href)) return href;
  // 相对路径与锚点放行；javascript:、data: 等无法命中白名单，降级成纯文本
  if (/^[#/?]/.test(href) || /^[\w.\-/]+$/.test(href)) return href;
  return "";
}
function inlineMarkdown(text) {
  const codes = [];
  return escapeHtml(text)
    .replace(/`([^`]+)`/g, (_, code) => {
      const token = MD_CODE_TOKEN + codes.length + MD_CODE_TOKEN;
      codes.push(`<code>${code}</code>`);
      return token;
    })
    .replace(/!\[([^\]]*)]\(([^\s)]+)\)/g, (whole, alt, src) => {
      const url = mdHref(src);
      return url ? `<img class="md-image" src="${url}" alt="${alt}">` : whole;
    })
    .replace(/\[([^\]]+)]\(([^\s)]+)\)/g, (whole, label, href) => {
      const url = mdHref(href);
      return url ? `<a href="${url}" target="_blank" rel="noopener noreferrer">${label}</a>` : whole;
    })
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/__([^_]+)__/g, "<strong>$1</strong>")
    .replace(/~~([^~]+)~~/g, "<del>$1</del>")
    .replace(/\*([^*]+)\*/g, "<em>$1</em>")
    .replace(/(^|[^\w])_([^_]+)_(?=$|[^\w])/g, "$1<em>$2</em>")
    .replace(new RegExp(`${MD_CODE_TOKEN}(\\d+)${MD_CODE_TOKEN}`, "g"), (_, index) => codes[Number(index)] || "");
}
// 表格：GFM 管道表格，列数不匹配的行按表头列数截断/补空。
// `\|` 是单元格内写竖线的转义写法，先用占位符护住再切列，切完还原
const MD_PIPE_TOKEN = "\u0002";
function tableCells(line) {
  const raw = String(line || "").trim();
  if (!raw.includes("|")) return null;
  return raw
    .replace(/\\\|/g, MD_PIPE_TOKEN)
    .replace(/^\|/, "").replace(/\|$/, "")
    .split("|")
    .map(cell => cell.trim().replace(new RegExp(MD_PIPE_TOKEN, "g"), "|"));
}
const MD_TABLE_DIVIDER = /^\s*\|?\s*:?-{2,}:?\s*(?:\|\s*:?-{2,}:?\s*)*\|?\s*$/;
function tableAlign(cell) {
  const left = cell.startsWith(":"), right = cell.endsWith(":");
  return left && right ? "center" : (right ? "right" : "left");
}
function renderTable(head, aligns, rows) {
  const cell = (tag, text, index) => `<${tag} style="text-align:${aligns[index] || "left"}">${inlineMarkdown(text || "")}</${tag}>`;
  const headHtml = head.map((text, index) => cell("th", text, index)).join("");
  const bodyHtml = rows.map(row => `<tr>${head.map((_, index) => cell("td", row[index], index)).join("")}</tr>`).join("");
  return `<div class="md-table-wrap"><table><thead><tr>${headHtml}</tr></thead><tbody>${bodyHtml}</tbody></table></div>`;
}
// 块级：代码块 → 标题 → 分隔线 → 引用 → 表格 → 列表（缩进嵌套 / 任务框 / 续行）→ 段落。
// 段落与引用内的连续行合并成同一个块，换行按软换行（空格）处理，行尾两空格或反斜杠为硬换行。
// 引用内容用同一套规则递归渲染，所以引用里的列表、表格、代码块都能正常解析
function basicMarkdown(text) {
  const blocks = [];
  const source = String(text || "").replace(/```([\w-]*)\n?([\s\S]*?)```/g, (_, lang, code) => {
    const token = `\u0000${blocks.length}\u0000`;
    blocks.push(`<div class="code-block"><header>${escapeHtml(lang || "text")}</header><pre>${escapeHtml(code.trim())}</pre></div>`);
    return token;
  });
  const hardBreak = /(?: {2,}|\\)$/;
  const parseLines = lines => {
    const out = [];
    const para = [];
    const quote = [];
    const stack = []; // 列表层级：{type, indent}
    let itemIndex = -1; // 最近一个未闭合 <li> 在 out 里的下标：列表项续行要并回这里
    let blank = false; // 空行后列表先不急着关，等下一行确认不是列表项再关
    const joins = (buffer, soft) => {
      const parts = [];
      buffer.forEach((line, index) => {
        parts.push(inlineMarkdown(line.replace(/(?: {2,}|\\)+$/, "")));
        if (index < buffer.length - 1) parts.push(hardBreak.test(line) ? "<br>" : soft);
      });
      return parts.join("");
    };
    const closeItem = () => { if (itemIndex >= 0) { out.push("</li>"); itemIndex = -1; } };
    const flushPara = () => { if (para.length) { out.push(`<p>${joins(para, "\n")}</p>`); para.length = 0; } };
    const flushQuote = () => {
      if (!quote.length) return;
      out.push(`<blockquote>${parseLines(quote.slice())}</blockquote>`);
      quote.length = 0;
    };
    const closeList = () => { while (stack.length) { closeItem(); out.push(`</${stack.pop().type}>`); } };
    const flushAll = () => { flushPara(); flushQuote(); closeList(); };
    for (let i = 0; i < lines.length; i += 1) {
      const line = lines[i];
      const token = line.match(/^\u0000(\d+)\u0000$/);
      if (token) { flushAll(); out.push(blocks[Number(token[1])]); continue; }
      if (!line.trim()) { flushPara(); flushQuote(); blank = true; continue; }
      const item = line.match(/^(\s*)([-*+]|\d+[.)])\s+(.*)$/);
      // 空行只打断段落与引用；下一行仍是列表项（含缩进子项）时列表继续，层级不丢
      if (blank) { blank = false; if (!item) closeList(); }
      const heading = line.match(/^\s*(#{1,6})\s+(.+?)\s*#*\s*$/);
      if (heading) {
        flushAll(); const level = heading[1].length;
        out.push(`<h${level}>${inlineMarkdown(heading[2])}</h${level}>`); continue;
      }
      if (/^\s*(?:[-*_]\s*){3,}$/.test(line)) { flushAll(); out.push("<hr>"); continue; }
      const quoteMark = line.match(/^\s*(?:>\s?)+/);
      if (quoteMark) { flushPara(); closeList(); quote.push(line.slice(quoteMark[0].length)); continue; }
      const head = tableCells(line);
      const divider = head && i + 1 < lines.length ? tableCells(lines[i + 1]) : null;
      if (divider && divider.length === head.length && MD_TABLE_DIVIDER.test(lines[i + 1])) {
        flushAll();
        const aligns = divider.map(tableAlign);
        const rows = [];
        let j = i + 2;
        for (; j < lines.length; j += 1) {
          const cells = tableCells(lines[j]);
          if (!cells || !lines[j].trim()) break;
          rows.push(cells);
        }
        out.push(renderTable(head, aligns, rows));
        i = j - 1; continue;
      }
      if (item) {
        flushPara(); flushQuote();
        const indent = item[1].replace(/\t/g, "  ").length;
        const type = /^\d/.test(item[2]) ? "ol" : "ul";
        const number = type === "ol" ? parseInt(item[2], 10) : 0;
        while (stack.length && stack[stack.length - 1].indent > indent) { closeItem(); out.push(`</${stack.pop().type}>`); }
        let top = stack[stack.length - 1];
        if (top && top.indent === indent && top.type !== type) { closeItem(); out.push(`</${top.type}>`); stack.pop(); top = stack[stack.length - 1]; }
        if (!top || top.indent < indent) {
          closeItem();
          out.push(type === "ol" && number > 1 ? `<ol start="${number}">` : `<${type}>`);
          stack.push({ type, indent });
        }
        closeItem();
        const task = item[3].match(/^\[([ xX])\]\s*(.*)$/);
        if (task) out.push(`<li class="md-task"><span class="md-check" aria-hidden="true">${task[1].toLowerCase() === "x" ? "☑" : "☐"}</span>${inlineMarkdown(task[2])}`);
        else out.push(`<li>${inlineMarkdown(item[3])}`);
        itemIndex = out.length - 1;
        continue;
      }
      // 列表项的续行（GFM lazy continuation）：并进上一个 <li>，不再另起段落
      if (itemIndex >= 0) { out[itemIndex] += " " + inlineMarkdown(line.trim()); continue; }
      flushQuote();
      para.push(line);
    }
    flushAll();
    return out.join("");
  };
  return parseLines(source.split("\n"));
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
// 一轮消息渲染成一张卡：正文在上，过程（思考与工具调用）居中，底部一条信息行。
// 卡片不带身份行——助手与用户靠左右对齐 + 一侧色条区分
const CARD_ROLES = ["user", "assistant", "tool"];
function createMessage(role, content = "", label = "", attachments = null) {
  const welcome = document.getElementById("welcome");
  if (welcome) welcome.remove();
  if (el.welcome) el.welcome = null;
  const node = document.createElement("article");
  node.className = `message ${role}`;
  const bubble = document.createElement("div");
  bubble.className = "bubble";
  if (label) bubble.innerHTML = `<div class="message-label">${escapeHtml(label)}</div>`;
  const body = document.createElement("div"); body.className = "markdown"; body.innerHTML = basicMarkdown(content);
  bubble.append(body);
  const msg = {node, card: null, bubble, body, text: content, reasoning: "", structured: []};
  // 底部信息行：最左复制按钮，右侧依次是用量、用时、时间；两个明细弹层跟着各自的按钮
  if (CARD_ROLES.includes(role)) {
    const card = document.createElement("section");
    card.className = `turn-card ${role}`;
    const foot = document.createElement("div");
    foot.className = "turn-foot";
    foot.innerHTML = '<button class="bubble-copy" type="button" title="复制内容" aria-label="复制内容">'
      + '<svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'
      + '<rect x="5.5" y="5.5" width="8.5" height="8.5" rx="1.2"/>'
      + '<path d="M10.5 5.5V3.2A1.2 1.2 0 0 0 9.3 2H3.2A1.2 1.2 0 0 0 2 3.2v6.1A1.2 1.2 0 0 0 3.2 10.5h2.3"/></svg></button>'
      + '<span class="bubble-meta"><span class="bubble-usage-wrap">'
      + '<button class="bubble-usage" type="button" title="本轮用量明细" hidden>'
      + '<svg width="10" height="10" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'
      + '<ellipse cx="8" cy="3.6" rx="5.4" ry="2"/><path d="M2.6 3.6v8.8c0 1.1 2.4 2 5.4 2s5.4-.9 5.4-2V3.6"/><path d="M2.6 8c0 1.1 2.4 2 5.4 2s5.4-.9 5.4-2"/></svg>'
      + '<span class="bubble-usage-label"></span></button>'
      + '<div class="bubble-usage-pop" hidden>'
      + '<div class="pop-title"><svg width="11" height="11" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'
      + '<ellipse cx="8" cy="3.6" rx="5.4" ry="2"/><path d="M2.6 3.6v8.8c0 1.1 2.4 2 5.4 2s5.4-.9 5.4-2V3.6"/><path d="M2.6 8c0 1.1 2.4 2 5.4 2s5.4-.9 5.4-2"/></svg>本轮用量<b class="pop-total"></b></div>'
      + '<div class="pop-row" data-row="model"><span>提供方 / 模型</span><b></b></div>'
      + '<div class="pop-row" data-row="cache_hit"><span>缓存命中</span><b></b></div>'
      + '<div class="pop-row" data-row="uncached_input"><span>未缓存输入</span><b></b></div>'
      + '<div class="pop-row" data-row="cache_read"><span>缓存读取</span><b></b></div>'
      + '<div class="pop-row" data-row="output"><span>输出</span><b></b></div>'
      + '</div></span><span class="bubble-timing-wrap">'
      + '<button class="bubble-timing" type="button" title="本轮用时明细" hidden>'
      + '<svg width="10" height="10" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'
      + '<circle cx="8" cy="8" r="6.2"/><path d="M8 4.6V8l2.4 1.6"/></svg>'
      + '<span class="bubble-timing-label"></span></button>'
      + '<div class="bubble-timing-pop" hidden>'
      + '<div class="pop-title"><svg width="11" height="11" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.4">'
      + '<circle cx="8" cy="8" r="6.2"/><path d="M8 4.6V8l2.4 1.6"/></svg>本轮用时和速度</div>'
      + '<div class="pop-row" data-row="total"><span>本轮总用时</span><b></b></div>'
      + '<div class="pop-row" data-row="think"><span>思考用时</span><b></b></div>'
      + '<div class="pop-row" data-row="tps"><span>输出速度 (TPS)</span><b></b></div>'
      + '<div class="pop-row" data-row="ttft"><span>首 token 用时 (TTFT，累计)</span><b></b></div>'
      + '</div></span><span class="bubble-time"></span></span>';
    card.append(bubble, foot); node.append(card);
    msg.card = card;
    const footer = foot;
    msg.timingBtn = footer.querySelector(".bubble-timing");
    msg.timingPop = footer.querySelector(".bubble-timing-pop");
    msg.usageBtn = footer.querySelector(".bubble-usage");
    msg.usagePop = footer.querySelector(".bubble-usage-pop");
    msg.timeEl = footer.querySelector(".bubble-time");
    msg.timingBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      // 运行中实时计时阶段还没有定稿明细，不弹层（但已展开的其他弹层照样收起）
      if (msg.liveTimer) { closeBubblePops(); return; }
      toggleBubblePop(msg.timingPop);
    });
    msg.timingPop.addEventListener("click", (e) => e.stopPropagation());
    msg.usageBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      toggleBubblePop(msg.usagePop);
    });
    msg.usagePop.addEventListener("click", (e) => e.stopPropagation());
    // 实时消息默认填当前时间；历史渲染会用消息自带 ts 覆盖（老会话无 ts 则清空）
    msg.timeEl.textContent = formatClock(new Date());
    footer.querySelector(".bubble-copy").addEventListener("click", () => {
      const text = msg.text || (msg.body ? msg.body.textContent : "") || "";
      if (!text.trim()) { showToast("没有可复制的内容"); return; }
      copyText(text).then(() => showToast("已复制到剪贴板"), () => showToast("复制失败"));
    });
  }
  if (!msg.card) node.append(bubble);
  el.messages.append(node);
  renderMessageAttachments(bubble, attachments);
  scrollMessages();
  return msg;
}
function setBubbleTime(msg, value) {
  if (msg && msg.timeEl) msg.timeEl.textContent = formatClock(value);
}
// 明细弹层全局唯一：卡片一多，各卡自己管自己就会出现好几个同时挂着，
// 所以统一由一个变量记录当前打开的那个，开新的先收起旧的
let openPop = null;
// 收起全部明细弹层。点击弹层外任意位置都会走到这里（弹层与触发按钮内部 stopPropagation 除外）
function closeBubblePops() {
  const open = document.querySelectorAll(".bubble-timing-pop:not([hidden]),.bubble-usage-pop:not([hidden])");
  for (let i = 0; i < open.length; i++) open[i].hidden = true;
  openPop = null;
}
document.addEventListener("click", closeBubblePops);
// 弹层左边缘对齐按钮、显示在按钮下方；已经开着同一个则收起。
// 消息区 overflow-x:hidden，靠右放不下时整体左移，避免面板被裁掉。
function toggleBubblePop(pop) {
  const wasOpen = openPop === pop && !pop.hidden;
  closeBubblePops();
  if (wasOpen) return;
  pop.hidden = false;
  openPop = pop;
  pop.style.marginLeft = "";
  const box = el.messages.getBoundingClientRect(), rect = pop.getBoundingClientRect();
  const overflow = rect.right - (box.right - 6);
  if (overflow <= 0) return;
  const room = Math.max(0, rect.left - box.left - 6);
  pop.style.marginLeft = -Math.ceil(Math.min(overflow, room)) + "px";
}
// 中文时长格式，用于用时按钮与明细弹层（如 820毫秒 / 1.4秒 / 2分5秒）
function formatDurationCn(ms) {
  if (ms == null || isNaN(ms) || ms < 0) return "";
  if (ms < 1000) return Math.round(ms) + "毫秒";
  if (ms < 60000) return (ms / 1000).toFixed(1) + "秒";
  const m = Math.floor(ms / 60000), s = Math.round((ms % 60000) / 1000);
  return m + "分" + s + "秒";
}
function setBubbleTiming(msg, info) {
  if (!msg || !msg.timingBtn) return;
  const t = info || {};
  if (t.elapsed == null) { msg.timingBtn.hidden = true; return; }
  msg.timingBtn.hidden = false;
  msg.timingBtn.querySelector(".bubble-timing-label").textContent = "用时 " + formatDurationCn(t.elapsed);
  // 思考用时只在用时明细弹层里出现，折叠行的摘要留给正文片段
  const rows = {
    total: formatDurationCn(t.elapsed),
    think: formatDurationCn(t.think),
    tps: t.tps == null ? "" : t.tps + " tok/s",
    ttft: formatDurationCn(t.ttft),
  };
  Object.keys(rows).forEach(key => {
    const row = msg.timingPop.querySelector('[data-row="' + key + '"]');
    if (!row) return;
    row.hidden = !rows[key];
    row.querySelector("b").textContent = rows[key];
  });
}
// 运行中实时「已用时」：done 到达前用时按钮每秒跳动，从本轮开始的墙钟时间起算；
// done 后由 setBubbleTiming 定格为最终用时+明细，计时器在 finishAssistant 里统一清除
function startLiveTiming(msg, startTs) {
  if (!msg || !msg.timingBtn) return;
  stopLiveTiming(msg);
  let t0 = startTs ? new Date(startTs).getTime() : NaN;
  if (isNaN(t0)) t0 = Date.now();
  const label = msg.timingBtn.querySelector(".bubble-timing-label");
  const tick = () => { label.textContent = "已用时 " + formatDurationCn(Math.max(0, Date.now() - t0)); };
  msg.timingBtn.hidden = false;
  tick();
  msg.liveTimer = setInterval(tick, 1000);
}
function stopLiveTiming(msg) {
  if (msg && msg.liveTimer) { clearInterval(msg.liveTimer); msg.liveTimer = null; }
}
let scrollLocked = false;
// 贴底才跟随：流式回复时用户上滚查看工具参数/历史，不能被下一个分片拽回底部（表现为滚轮失灵）。
// 重新滚回底部附近即恢复自动跟随；force 用于发送新消息、切换会话等必须落底的场景。
const BOTTOM_SLACK = 24;
let followBottom = true;
function atBottom() { const box = el.messages; return box.scrollHeight - box.scrollTop - box.clientHeight <= BOTTOM_SLACK; }
function scrollMessages(force) {
  if (scrollLocked) return;
  if (force) followBottom = true; else if (!followBottom) return;
  el.messages.scrollTop = el.messages.scrollHeight;
}
// 滚动条拖动、滚轮、触摸、键盘翻页都会触发 scroll：离开底部即交出滚动控制权
el.messages.addEventListener("scroll", () => { followBottom = atBottom(); updateTurnRailActive(); }, {passive: true});
// 点击展开过程行/工具条目会把内容顶高，此时用户已不在底部，下一帧重判后停止跟随，避免刚展开就被拽走
el.messages.addEventListener("click", event => {
  if (!event.target.closest("summary,.proc-row")) return;
  requestAnimationFrame(() => { followBottom = atBottom(); });
});

/* --- 对话轮次导航轨：页面最左边缘竖排白点，一轮一个点 ---
   轮次以用户消息为界：一条 user 消息与其后的 assistant/tool 消息同属一轮。
   鼠标移入轨道区域即向右展开完整轮次列表（序号 + 该轮提问摘要），点击列表行或白点定位到该轮；
   当前所处的轮次（滚动位置落在中间线及以前的最末一轮）在轨道白点与列表行上同步高亮。 */
let railNodes = [], railDots = [], railRows = [], railPending = false, railPanelTimer = 0;
function railTurnText(node) {
  const body = node.querySelector(".markdown");
  const text = (body ? body.textContent : "").replace(/\s+/g, " ").trim();
  return text || "（本轮无文本内容）";
}
function hideTurnRailPanel() {
  clearTimeout(railPanelTimer);
  if (el.turnRailPanel) el.turnRailPanel.classList.remove("show");
}
// 离开轨道/列表留一点延迟再收起：鼠标从白点移到右侧列表要跨过间隙，不能让面板闪退
function scheduleHideTurnRailPanel() {
  clearTimeout(railPanelTimer);
  railPanelTimer = setTimeout(hideTurnRailPanel, 180);
}
function jumpToTurn(index) { const node = railNodes[index]; if (node) node.scrollIntoView({block: "start", behavior: "smooth"}); }
// 面板贴在轨道右侧展开，纵向与轨道居中；视口上下越界时整体回推，避免被裁掉
function showTurnRailPanel() {
  const panel = el.turnRailPanel;
  if (!panel || !railNodes.length) return;
  clearTimeout(railPanelTimer);
  panel.classList.add("show");
  const railRect = el.turnRail.getBoundingClientRect();
  panel.style.left = Math.round(railRect.right + 10) + "px";
  const top = railRect.top + railRect.height / 2 - panel.offsetHeight / 2;
  panel.style.top = Math.round(Math.min(Math.max(8, top), Math.max(8, window.innerHeight - panel.offsetHeight - 8))) + "px";
  updateTurnRailActive();
}
function updateTurnRailActive() {
  if (!railDots.length) return;
  const box = el.messages.getBoundingClientRect();
  const mid = box.top + box.height / 2;
  let active = 0;
  // 依次找「顶边还在中间线及以上」的最后一轮；一轮都够不着时停在第一轮
  railNodes.forEach((node, index) => { if (index < railDots.length && node.getBoundingClientRect().top <= mid) active = index; });
  railDots.forEach((dot, index) => dot.classList.toggle("active", index === active));
  railRows.forEach((row, index) => {
    const on = index === active;
    row.classList.toggle("active", on);
    if (on) row.setAttribute("aria-current", "true"); else row.removeAttribute("aria-current");
  });
}
// 轮次节点集合未变就不重建，避免流式输出时反复刷 DOM 打断悬浮
function renderTurnRail() {
  if (!el.turnRail) return;
  const turns = state.viewTab === "chat" && !el.messages.classList.contains("hidden")
    ? Array.prototype.slice.call(el.messages.querySelectorAll(".message.user")) : [];
  const same = turns.length === railNodes.length && turns.every((node, index) => node === railNodes[index]);
  if (!same) {
    hideTurnRailPanel();
    railNodes = turns;
    railDots = [];
    railRows = [];
    el.turnRail.innerHTML = "";
    el.turnRailList.innerHTML = "";
    el.turnRailCount.textContent = turns.length ? turns.length + " 轮" : "";
    turns.forEach((node, index) => {
      const dot = document.createElement("button");
      dot.type = "button";
      dot.className = "turn-rail-dot";
      dot.setAttribute("aria-label", "第 " + (index + 1) + " 轮");
      dot.onclick = () => jumpToTurn(index);
      el.turnRail.append(dot);
      railDots.push(dot);

      const text = railTurnText(node);
      const row = document.createElement("button");
      row.type = "button";
      row.className = "turn-rail-row";
      row.title = text;
      row.setAttribute("aria-label", "第 " + (index + 1) + " 轮：" + text);
      row.innerHTML = '<i class="turn-rail-index">' + (index + 1) + '</i><span class="turn-rail-text"></span>';
      row.querySelector(".turn-rail-text").textContent = text;
      row.onclick = () => { jumpToTurn(index); hideTurnRailPanel(); };
      el.turnRailList.append(row);
      railRows.push(row);
    });
  }
  const empty = !turns.length;
  el.turnRail.classList.toggle("hidden", empty);
  if (empty) hideTurnRailPanel();
  updateTurnRailActive();
}
// 鼠标进入轨道区域展开列表；面板自身也算悬停区，移到列表上继续保留
el.turnRail.addEventListener("mouseenter", showTurnRailPanel);
el.turnRail.addEventListener("mouseleave", scheduleHideTurnRailPanel);
el.turnRailPanel.addEventListener("mouseenter", () => clearTimeout(railPanelTimer));
el.turnRailPanel.addEventListener("mouseleave", scheduleHideTurnRailPanel);
// 消息区增删（发消息、切会话、历史重放、暂存/挂回）都走这里，统一按下一帧合并刷新
function syncTurnRail() {
  if (railPending) return;
  railPending = true;
  requestAnimationFrame(() => { railPending = false; renderTurnRail(); });
}
new MutationObserver(syncTurnRail).observe(el.messages, {childList: true});
// 会话的流式回复已被切走（DOM 摘下暂存）：内容照常写入暂存节点，但滚动等全局副作用要锁住
function hiddenFor(id) { const stream = state.streams.get(id); return Boolean(stream && stream.hidden); }
function backgroundSafe(id, render) { if (!hiddenFor(id)) return render(); scrollLocked = true; try { return render(); } finally { scrollLocked = false; } }
// 千分位整数，用于用量明细
function formatInt(n) {
  if (n == null || isNaN(n)) return "";
  return String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ",");
}
// 用量收进底部栏的「用量」按钮：按钮显示总 token，点击弹层看输入/缓存等明细
function setBubbleUsage(message, usage) {
  if (!message || !message.usageBtn) return;
  const u = usage || {};
  const total = u.total || 0, input = u.input || 0, output = u.output || 0;
  if (!total && !input && !output) { message.usageBtn.hidden = true; return; }
  message.usageBtn.hidden = false;
  // 供应商没回传 usage 时后端给本地估算值，用 ≈ 区分实测与估算
  const label = (u.estimated ? "≈ " : "") + formatInt(total) + " tokens";
  message.usageBtn.querySelector(".bubble-usage-label").textContent = label;
  message.usagePop.querySelector(".pop-total").textContent = label;
  const cacheRead = u.cache_read || 0;
  const rows = {
    model: usageModelLabel(u.model),
    cache_hit: cacheRead && input ? formatInt(cacheRead / input * 100) + "%" : "",
    uncached_input: formatInt(Math.max(input - cacheRead, 0)),
    cache_read: cacheRead ? formatInt(cacheRead) : "",
    // 输出行对齐 DeepSeek 面板：有推理 token 时追加「（其中推理 N tok）」
    output: formatInt(output) + (u.reasoning ? " tok（其中推理 " + formatInt(u.reasoning) + " tok）" : ""),
  };
  Object.keys(rows).forEach(key => {
    const row = message.usagePop.querySelector('[data-row="' + key + '"]');
    if (!row) return;
    row.hidden = !rows[key];
    row.querySelector("b").textContent = rows[key];
  });
}
function finishAssistant(message, deferred = false) {
  if (!message || message.finished) return;
  message.finished = true;
  // 本轮是否属于当前正在看的会话（后台会话的气泡已摘进暂存片段，供收尾时判断能否动共享的计划窗口）
  const visible = message.node.isConnected;
  // 流结束（done/停止/出错）统一停掉实时「已用时」计时器，并让卡底的过程行停止呼吸
  stopLiveTiming(message);
  settleProcess(message);
  const parsed = structuredBlocks(message.text);
  message.body.innerHTML = basicMarkdown(parsed.visible);
  parsed.found.forEach(data => renderStructured(data, message.node));
  // 文本兜底的 options / tool_params 块也算一次待作答询问，与 ask_user_question 工具共用浮层。
  // 取最后一个候选块：参数编辑与选项合并进同一张浮层卡片，对话流里不再单独成卡
  const ask = parsed.found.slice().reverse()
    .find(data => Boolean(data.tool_params || data.toolparams) || (Array.isArray(data.options) && data.options.length));
  if (ask && !message.pendingAsk) message.pendingAsk = ask;
  // 本轮以询问浮层收尾 → 会话进入「待确认」，等用户操作
  message.awaitingInput = Boolean(message.pendingAsk);
  // 只有思考过程没有正文时也要留住这一轮：停止在思考阶段是常见操作，丢掉节点等于
  // 把这段内容从实时视图和重渲染后的历史里一起抹掉
  if (!parsed.visible && !parsed.found.length && !message.node.querySelector(".proc-row")) message.node.remove();
  // 历史重放要等整轮重放完再弹，否则中途那些旧询问会闪一下；
  // 后台会话结束时它的 DOM 已摘进暂存片段，isConnected 为假，不能弹到当前会话头上
  if (!deferred && message.pendingAsk && message.node.isConnected) openChoiceOverlay(message.pendingAsk);
  // 计划窗口只服务执行过程：本轮结束时计划已全部完成就收起，下一条 plan_updated 会自动再出现
  if (visible && planComplete(state.phasePlan)) el.phasePanel.classList.add("hidden");
  scrollMessages();
}
function appendReasoning(message, delta) {
  message.reasoning += delta || "";
  const row = processRow(message.node, "think", PROC_LABEL.think, true);
  if (!row) return;
  row.classList.add("running");
  // 折叠态右侧一直刷新的就是思考正文的当前最后一行，展开后看全文
  setProcSummary(row, latestLine(message.reasoning));
  const rail = processRail(message.node, "think");
  if (rail) rail.textContent = message.reasoning;
  // 展开时思考正文会把内容顶高，贴底才跟随（用户上滚查看时不会被拽回）
  if (rail && !rail.parentNode.hidden) scrollMessages();
}

function renderStructured(data, parent) {
  if (data.phase_plan) renderPhase(data.phase_plan);
  // tool_params 与 options 合并到输入框上方的浮层（见 renderChoiceCard），对话流里不再单独成卡
  if (data.workflow) renderWorkflowProposal(data.workflow, parent);
}
const ASK_USER_TOOL = "ask_user_question";
let choiceTimer = null;
// 待作答的询问浮在输入框上方、把输入框盖住；关闭时向下压扁淡出，像缩回输入框一样
function openChoiceOverlay(payload) {
  const host = el.choiceOverlay;
  if (!host || !payload) return;
  clearTimeout(choiceTimer);
  host.innerHTML = "";
  host.classList.remove("hidden", "open");
  renderChoiceCard(payload, host);
  if (!host.firstElementChild) return;
  if (el.composer) el.composer.classList.add("choice-open");
  requestAnimationFrame(() => { host.classList.add("open"); syncPhasePanelLift(); });
}
function closeChoiceOverlay() {
  const host = el.choiceOverlay;
  if (!host || host.classList.contains("hidden")) return;
  // 立刻恢复输入框：卡片向下收缩的同时输入框淡入，就像卡片缩回成了输入框
  if (el.composer) el.composer.classList.remove("choice-open");
  host.classList.remove("open");
  syncPhasePanelLift();
  clearTimeout(choiceTimer);
  choiceTimer = setTimeout(() => { host.classList.add("hidden"); host.innerHTML = ""; }, 240);
}
// 选择窗口浮在输入框上方，会盖住紧贴其上的计划窗口：浮层出现/变高时把计划窗口顶开，
// 浮层收起/变矮后再贴着浮层的上沿落回来
function syncPhasePanelLift() {
  const panel = el.phasePanel;
  const overlay = el.choiceOverlay;
  if (!panel) return;
  const open = overlay && !overlay.classList.contains("hidden") && overlay.classList.contains("open");
  if (!open || panel.classList.contains("hidden")) {
    panel.style.marginBottom = "";
    return;
  }
  // 浮层绝对定位在 .composer 内且底距 12px，故其上沿 = 输入区高度 - 12px - 浮层高度；
  // 计划窗口在普通流里，把这个差值当作 margin-bottom 就能让它始终贴着浮层上沿：
  // 浮层高于计划窗口原位时向上移（挤走中间消息区），浮层很矮时向下沉（此时输入框本就藏起）
  // 用 offset* 而不是 getBoundingClientRect，避开浮层入场动画 transform 的影响
  const gap = 8;  // 计划窗口与浮层之间留出的空隙
  const lift = overlay.offsetHeight + gap + 12 - (el.composer ? el.composer.offsetHeight : 0);
  panel.style.marginBottom = `${lift}px`;
}
// 模型询问用户：可单选、也能用序号提交的选项卡片（挂在浮层里）。
// 工具参数询问与后端审批共用这一张卡：参数表 + 选项，一次提交同时回执参数与选择
function renderChoiceCard(payload, parent) {
  const approval = payload && payload.approval;
  const toolParams = payload && !approval && (payload.tool_params || payload.toolparams);
  const params = approval ? approvalEntries(approval.event) : toolParamEntries(toolParams);
  const raw = payload && Array.isArray(payload.options) ? payload.options : [];
  let options = raw.filter(item => item && (item.label != null || item.value != null));
  // 审批只有批准/拒绝；参数块没有选项时补上默认的确认/取消，参数表始终有落脚的按钮
  if (approval) options = [{label: "批准", value: "approve", style: "primary"}, {label: "拒绝", value: "deny", style: "danger"}];
  else if (!options.length && params.length) options = [{label: "确认执行", value: "confirm", style: "primary"}, {label: "取消", value: "cancel", style: "danger"}];
  if (!options.length) return;
  const title = (payload && payload.title)
    || (approval ? `审批工具 · ${approval.event.name || ""}` : toolParams ? `确认工具参数 · ${toolParams.tool || ""}` : "请选择下一步");
  const card = document.createElement("section");
  card.className = "choice-card";
  if (approval) card.classList.add("approval");
  card.innerHTML = '<div class="choice-head" role="button" tabindex="0" aria-expanded="true">'
    + `<span class="choice-title">${escapeHtml(title)}</span>`
    + '<span class="choice-chevron" aria-hidden="true">⌄</span>'
    + '<button class="choice-close" type="button" title="收起" aria-label="收起">×</button></div>'
    + '<div class="choice-question"></div>'
    + '<div class="choice-params params"></div>'
    + '<div class="choice-list" role="radiogroup" aria-label="候选选项"></div>'
    + '<div class="choice-foot"><div class="choice-answer">'
    + '<input class="choice-input" type="text" autocomplete="off" placeholder="点击「其他」后在此输入答案" aria-label="输入自定义答案" disabled>'
    + '<button class="choice-submit" type="button" disabled>提交答案</button>'
    + '</div><span class="choice-hint">按 Esc 取消</span></div>';
  const question = $(".choice-question", card);
  question.textContent = payload.question || "";
  if (!payload.question) question.classList.add("hidden");
  const paramHost = $(".choice-params", card);
  const paramInputs = [];
  if (!params.length) paramHost.classList.add("hidden");
  params.forEach((entry, index) => {
    const row = document.createElement("div"); row.className = "param-row";
    row.innerHTML = `<label for="choice-param-${index}"><b>${escapeHtml(entry.name)}${entry.required ? " *" : ""}</b>${escapeHtml(entry.desc || "")}</label>`;
    // 审批参数常带 object/array（如分布参数），这类改用多行输入
    const structured = entry.type === "object" || entry.type === "array";
    const field = document.createElement(structured ? "textarea" : "input");
    field.id = `choice-param-${index}`;
    if (structured) { field.rows = 3; field.style.font = "11px/1.4 Consolas,monospace"; } else field.type = "text";
    field.value = valueForInput(entry.value);
    row.append(field); paramHost.append(row); paramInputs.push(field);
  });
  // 参数值随提交一起回填：按条目类型还原数字/布尔/对象的原始形态
  const paramValues = () => {
    const values = {};
    params.forEach((entry, index) => { values[entry.name] = coerceSchemaValue(paramInputs[index].value, entry.type); });
    return values;
  };
  const list = $(".choice-list", card);
  const entries = options.map(option => {
    const label = option.label != null ? String(option.label) : String(option.value);
    const item = document.createElement("div");
    item.className = "choice-item";
    item.setAttribute("role", "radio");
    item.setAttribute("aria-checked", "false");
    item.tabIndex = 0;
    item.innerHTML = '<span class="choice-dot" aria-hidden="true"></span><span class="choice-text">'
      + `<strong>${escapeHtml(label)}</strong>`
      + (option.description ? `<small>${escapeHtml(String(option.description))}</small>` : "")
      + '</span>';
    list.append(item);
    return {item, option, label, other: false};
  });
  // 末尾固定补一个「其他」：模型没给自由项时也能自己写答案；审批是二选一，不补
  if (!approval && !entries.some(entry => /^(其他|其它|other)$/i.test(entry.label.trim()))) {
    const item = document.createElement("div");
    item.className = "choice-item";
    item.setAttribute("role", "radio");
    item.setAttribute("aria-checked", "false");
    item.tabIndex = 0;
    item.innerHTML = '<span class="choice-dot" aria-hidden="true"></span><span class="choice-text"><strong>其他</strong></span>';
    list.append(item);
    entries.push({item, option: null, label: "其他", other: true});
  }
  const input = $(".choice-input", card);
  const submitBtn = $(".choice-submit", card);
  let selected = -1;
  let submitted = false;
  const paint = index => {
    selected = index;
    entries.forEach((entry, position) => {
      entry.item.classList.toggle("selected", position === index);
      entry.item.setAttribute("aria-checked", String(position === index));
    });
  };
  const pick = index => { if (!submitted) paint(index); };
  // 选中「其他」才放开输入框：先写清楚要什么，再点提交答案
  const openAnswer = index => {
    pick(index);
    input.disabled = false;
    submitBtn.disabled = false;
    input.classList.remove("invalid");
    input.focus();
  };
  // 审批走接口回执：批准时连同编辑后的参数一起提交，拒绝只置否；后端在等结果，成功即收卡
  const resolve = async (info, approved) => {
    if (submitted) return;
    submitted = true;
    card.classList.add("submitted");
    paramInputs.forEach(field => field.disabled = true);
    entries.forEach(entry => entry.item.classList.add("locked"));
    const sid = info.sessionId || (state.session && state.session.meta.id);
    try {
      await request(`/sessions/${encodeURIComponent(sid)}/tool-approvals/${encodeURIComponent(info.event.call_id)}`, {method:"POST",body:JSON.stringify({approved,args: approved ? paramValues() : (info.event.args || {})})});
      const stream = state.streams.get(sid); if (stream) delete stream.approval;
      // 审批已解决，流还在跑就把徽标从「待确认」恢复「进行中」
      if (sid && state.controllers.has(sid)) setStatus(sid, "running");
      closeChoiceOverlay();
    } catch (error) {
      showToast(error.message);
      submitted = false;
      card.classList.remove("submitted");
      paramInputs.forEach(field => field.disabled = false);
      entries.forEach(entry => entry.item.classList.remove("locked"));
    }
  };
  const submit = () => {
    if (submitted) return;
    // 未选中不发消息，只提示
    if (selected < 0) return;
    const chosen = entries[selected];
    let value = chosen.label;
    if (chosen.other) {
      const text = input.value.trim();
      if (!text) { input.classList.add("invalid"); input.focus(); return; }
      value = text;
    } else if (chosen.option && chosen.option.value != null) {
      value = String(chosen.option.value);
    }
    if (approval) { resolve(approval, value === "approve"); return; }
    submitted = true;
    card.classList.add("submitted");
    input.disabled = true;
    submitBtn.disabled = true;
    paramInputs.forEach(field => field.disabled = true);
    entries.forEach(entry => entry.item.classList.add("locked"));
    // 参数块把编辑后的参数与所选选项打包成一次确认回填；走「其他」时自由文本一并带上
    const payloadText = toolParams
      ? `<structured_interaction>${JSON.stringify({type:"tool_params_confirmed",tool:toolParams.tool,confirmed:value==="confirm",params:paramValues()})}</structured_interaction>`
      : value;
    sendMessage(toolParams && chosen.other ? `${payloadText}\n\n${value}` : payloadText, chosen.other ? value : chosen.label);
    closeChoiceOverlay();
  };
  entries.forEach((entry, index) => {
    // 普通选项点一下就直接确认；「其他」只亮起输入框，写完再点提交答案
    const activate = () => { if (entry.other) openAnswer(index); else { pick(index); submit(); } };
    entry.item.onclick = activate;
    entry.item.onkeydown = event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); activate(); } };
  });
  input.oninput = () => { if (!submitted) input.classList.remove("invalid"); };
  input.onkeydown = event => { if (event.key === "Enter") { event.preventDefault(); submit(); } };
  submitBtn.onclick = submit;
  const head = $(".choice-head", card);
  const toggleCollapse = () => {
    const collapsed = card.classList.toggle("collapsed");
    head.setAttribute("aria-expanded", String(!collapsed));
  };
  head.onclick = event => { if (!event.target.closest(".choice-close")) toggleCollapse(); };
  head.onkeydown = event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); toggleCollapse(); } };
  $(".choice-close", card).onclick = event => { event.stopPropagation(); closeChoiceOverlay(); };
  parent.append(card);
}
function valueForInput(value) { return typeof value === "object" ? JSON.stringify(value) : String(value == null ? "" : value); }
function inferValueType(value) { return typeof value === "number" ? "number" : typeof value === "boolean" ? "boolean" : (value !== null && typeof value === "object" ? "object" : "string"); }
// 浮层卡的参数条目统一成 {name, desc, value, type, required}，工具参数与审批共用同一套渲染
function toolParamEntries(toolParams) {
  const list = toolParams && Array.isArray(toolParams.params) ? toolParams.params : [];
  return list.map(param => ({name: param.name, desc: param.description || "", value: param.value, type: inferValueType(param.value), required: false}));
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
// 计划是否已跑完：所有阶段都进入终态。计划窗口只服务执行过程，跑完即收起
const PHASE_DONE_STATUS = ["done", "succeeded", "completed", "skipped"];
function planComplete(plan) {
  const phases = plan && plan.phases;
  return Boolean(phases && phases.length) && phases.every(item => PHASE_DONE_STATUS.includes(item && item.status));
}
function renderPhase(value) {
  const phase = extractPhase(value) || value;
  if (!phase || !Array.isArray(phase.phases)) return;
  state.phasePlan = phase;
  el.phasePanel.classList.remove("hidden");
  const completed = phase.phases.filter(item => PHASE_DONE_STATUS.includes(item.status)).length;
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
  // 计划窗口此刻才出现、而选择窗口已经开着时，同样要把它顶到浮层上方
  syncPhasePanelLift();
}

// 过程区挂在卡底：思考与工具调用各占一行 chip（默认收起，可就地展开），
// 位置在整轮生命周期里固定不变——运行态只是换成呼吸点，结束后原地变回箭头，没有节点搬家
function processHost(parent) {
  if (!parent) return null;
  const card = parent.classList && parent.classList.contains("turn-card")
    ? parent : parent.querySelector(":scope > .turn-card");
  if (!card) return null;
  let host = card.querySelector(":scope > .turn-process");
  if (!host) { host = document.createElement("div"); host.className = "turn-process"; card.append(host); }
  return host;
}
function processToggle(row) {
  const body = row.parentNode.querySelector(`:scope > .proc-body[data-proc-body="${row.dataset.proc}"]`);
  const open = row.getAttribute("aria-expanded") === "true";
  row.setAttribute("aria-expanded", String(!open));
  if (body) body.hidden = open;
}
// first 为真时插到「工具调用」前面，保证思考始终在上
function processRow(parent, key, label, first) {
  const host = processHost(parent);
  if (!host) return null;
  let row = host.querySelector(`:scope > .proc-row[data-proc="${key}"]`);
  if (row) return row;
  row = document.createElement("div");
  row.className = "proc-row"; row.dataset.proc = key;
  row.setAttribute("role", "button"); row.setAttribute("tabindex", "0"); row.setAttribute("aria-expanded", "false");
  row.innerHTML = '<span class="proc-chevron" aria-hidden="true"></span>'
    + '<span class="run-dot" aria-hidden="true"></span>'
    + '<span class="proc-label"></span><span class="proc-sum"><span class="proc-sum-text"></span></span>';
  row.querySelector(".proc-label").textContent = label;
  const body = document.createElement("div");
  body.className = "proc-body"; body.dataset.procBody = key; body.hidden = true;
  const anchor = first && host.querySelector(':scope > .proc-row[data-proc="tools"]');
  if (anchor) { host.insertBefore(row, anchor); host.insertBefore(body, anchor); }
  else { host.append(row); host.append(body); }
  row.addEventListener("click", () => processToggle(row));
  row.addEventListener("keydown", event => {
    if (event.key !== "Enter" && event.key !== " ") return;
    event.preventDefault(); processToggle(row);
  });
  return row;
}
// 过程行的展开体里统一用一根竖线做缩进轨道
const PROC_LABEL = {think: "思考过程", tools: "工具调用"};
function processRail(parent, key) {
  const row = processRow(parent, key, PROC_LABEL[key], key === "think");
  if (!row) return null;
  const body = row.parentNode.querySelector(`:scope > .proc-body[data-proc-body="${key}"]`);
  let rail = body.querySelector(".proc-rail");
  if (!rail) { rail = document.createElement("div"); rail.className = "proc-rail"; body.append(rail); }
  return rail;
}
// 摘要文本写在里层 span 上：流式时靠它撑到 max-content，才能把超出的部分挤到左边裁掉
function setProcSummary(row, text) {
  if (!row) return;
  const target = row.querySelector(".proc-sum-text");
  if (target) target.textContent = text;
}
function updateToolGroup(parent) {
  const row = parent && parent.querySelector('.proc-row[data-proc="tools"]');
  if (!row) return;
  const items = [...parent.querySelectorAll(".tool-item")];
  const running = items.some(item => item.dataset.state === "running");
  const failed = items.some(item => item.dataset.state === "failed");
  setProcSummary(row, `${items.length} 个 · ${running ? "执行中" : failed ? "有失败" : "全部完成"}`);
  row.classList.toggle("running", running);
  row.classList.toggle("failed", !running && failed);
}
// 思考正文定位到某一行：流式中取最后一行（跟着 token 一直刷新），结束后取第一行当预览
function firstLine(text) {
  const source = String(text || "").trim();
  const newline = source.indexOf("\n");
  return newline === -1 ? source : source.slice(0, newline);
}
function latestLine(text) {
  const source = String(text || "").trimEnd();
  const newline = source.lastIndexOf("\n");
  return newline === -1 ? source : source.slice(newline + 1);
}
// 思考 chip 收起时的摘要：不再放耗时/字数，直接给正文片段，内容本身比指标有用
function updateThinkSummary(message) {
  if (!message || !message.card) return;
  const row = message.card.querySelector('.proc-row[data-proc="think"]');
  if (!row) return;
  setProcSummary(row, firstLine(message.reasoning));
}
// 思考段落结束（正文开始产出或开始调工具）：思考 chip 立刻停止「思考中」，
// 否则整轮都停在呼吸态，看起来像一直在想
function settleThink(message) {
  if (!message || !message.card) return;
  const row = message.card.querySelector('.proc-row[data-proc="think"]');
  if (!row || !row.classList.contains("running")) return;
  row.classList.remove("running");
  updateThinkSummary(message);
}
// 流结束（正常收尾 / 停止 / 出错）统一落定：停掉呼吸点，别让动画一直转下去
function settleProcess(message) {
  if (!message || !message.card) return;
  const todos = message.card.querySelectorAll(".proc-row.running");
  for (let i = 0; i < todos.length; i++) {
    const row = todos[i];
    row.classList.remove("running");
    if (row.dataset.proc === "think") updateThinkSummary(message);
    else setProcSummary(row, row.querySelector(".proc-sum").textContent.replace("执行中", "已中断"));
  }
}
function toolArgsTableHtml(args) {
  const entries = args && typeof args === "object" && !Array.isArray(args) ? Object.entries(args) : [];
  if (!entries.length) return '<div class="tool-args-empty">（无参数）</div>';
  const rows = entries.map(([key, value]) => {
    const structured = value !== null && typeof value === "object";
    const text = structured ? JSON.stringify(value) : String(value == null ? "" : value);
    const cls = structured ? ' class="tool-args-json"' : "";
    return `<tr><th scope="row">${escapeHtml(key)}</th><td${cls}>${escapeHtml(text)}</td></tr>`;
  }).join("");
  return `<div class="tool-args-scroll"><table class="tool-args-table"><thead><tr><th scope="col">参数</th><th scope="col">值</th></tr></thead><tbody>${rows}</tbody></table></div>`;
}
function renderToolCall(event, parent) {
  const rail = processRail(parent, "tools");
  if (!rail) return;
  let list = rail.querySelector(".tool-list");
  if (!list) { list = document.createElement("div"); list.className = "tool-list"; rail.append(list); }
  const item = document.createElement("details");
  item.className = "tool-item"; item.dataset.callId = event.id || event.call_id || ""; item.dataset.state = "running";
  item.innerHTML = `<summary><span class="tool-dot" aria-hidden="true"></span><strong>${escapeHtml(event.name || "工具调用")}</strong><span class="status">执行中</span></summary><div class="tool-detail"><span class="tool-detail-label">调用参数</span>${toolArgsTableHtml(event.args)}<div class="tool-result hidden"><span class="tool-detail-label">调用结果</span><pre></pre></div></div>`;
  list.append(item); updateToolGroup(parent); scrollMessages();
}
// 工具返回值多是 JSON 字符串：能解析就缩进美化，嵌套的 JSON 字符串一并展开，方便直接阅读
function expandJsonStrings(value, depth) {
  if (depth <= 0) return value;
  if (typeof value === "string") {
    const text = value.trim();
    if (text[0] !== "{" && text[0] !== "[") return value;
    try { return expandJsonStrings(JSON.parse(text), depth - 1); } catch (_) { return value; }
  }
  if (Array.isArray(value)) return value.map(item => expandJsonStrings(item, depth - 1));
  if (value && typeof value === "object") {
    const out = {};
    Object.keys(value).forEach(key => { out[key] = expandJsonStrings(value[key], depth - 1); });
    return out;
  }
  return value;
}
function formatToolResult(raw) {
  const text = String(raw == null ? "" : raw);
  const trimmed = text.trim();
  if (trimmed[0] !== "{" && trimmed[0] !== "[") return text;
  try { return JSON.stringify(expandJsonStrings(JSON.parse(trimmed), 3), null, 2); } catch (_) { return text; }
}
function renderToolResult(event, parent) {
  const selector = `[data-call-id="${CSS.escape(event.call_id || "")}"]`;
  let item = parent.querySelector(selector) || document.querySelector(selector);
  if (!item) { renderToolCall({id:event.call_id,name:event.name,args:{}}, parent); item = parent.querySelector(selector); }
  const failed = String(event.result || "").toLowerCase().includes("error") || String(event.result || "").includes("denied");
  item.dataset.state = failed ? "failed" : "succeeded";
  const status = $(":scope > summary .status", item); status.textContent = failed ? "失败" : "完成"; status.className = `status ${failed ? "failed" : "succeeded"}`;
  const result = $(".tool-result", item); result.classList.remove("hidden"); $("pre", result).textContent = formatToolResult(event.result);
  updateToolGroup(item.closest(".message")); scrollMessages();
}
function coerceSchemaValue(raw, type) {
  const text = String(raw == null ? "" : raw).trim();
  if (type === "number" || type === "integer") { const number = Number(text); return Number.isNaN(number) ? text : number; }
  if (type === "boolean") return ["true","1","yes"].includes(text.toLowerCase());
  if (type === "object" || type === "array") { try { return JSON.parse(text); } catch (_) { return text; } }
  return raw;
}
// 审批参数按工具 schema 展开：必填打星、object/array 用多行输入
function approvalEntries(event) {
  const args = event.args && typeof event.args === "object" && !Array.isArray(event.args) ? event.args : {};
  const schemaProps = event.schema && event.schema.properties && typeof event.schema.properties === "object" ? event.schema.properties : null;
  const required = event.schema && Array.isArray(event.schema.required) ? event.schema.required : [];
  const entries = schemaProps
    ? Object.entries(schemaProps).map(([name, def]) => ({name, desc: (def && def.description) || "", value: args[name], type: (def && def.type) || inferValueType(args[name]), required: required.includes(name)}))
    : Object.keys(args).map(name => ({name, desc: "", value: args[name], type: inferValueType(args[name]), required: required.includes(name)}));
  if (schemaProps) Object.keys(args).filter(name => !(name in schemaProps)).forEach(name => entries.push({name, desc: "", value: args[name], type: inferValueType(args[name]), required: false}));
  return entries;
}
// 审批复用输入框上方那张参数卡：后端阻塞等回执，所以卡上只给批准/拒绝，不留关闭入口
function openApprovalOverlay(event, sessionId) {
  openChoiceOverlay({approval: {event, sessionId}});
}
// 审批到达：与参数询问一样弹浮层；后台会话先挂在流上，切回时再补弹，避免盖到别的会话头上
function queueApproval(id, event) {
  const stream = state.streams.get(id);
  if (stream) stream.approval = {event, sessionId: id};
  if (!hiddenFor(id)) openApprovalOverlay(event, id);
  setStatus(id, "waiting");
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

// 用户点「停止」中断的回复：给气泡补标记，避免半截内容看起来像正常答完。
// 实时流（停止那一刻）和刷新后重建历史都走这里，两种呈现保持一致。
function markStopped(item) {
  let label = $(".message-label", item.bubble);
  if (!label) { label = document.createElement("div"); label.className = "message-label"; item.bubble.insertBefore(label, item.bubble.firstChild); }
  if (label.textContent.indexOf("已停止") < 0) label.textContent = label.textContent ? label.textContent + " · 已停止" : "已停止";
}
// turn 为本轮已合并的气泡：实时流里一轮对话只有一个 assistant 气泡（文本累加、
// 所有工具调用进同一个折叠组），而持久化会拆成多条消息，渲染时必须合并回去。
function renderHistoryMessage(message, turn) {
  if (message.role === "user") {
    const item = createMessage("user", message.display_content || message.content || "", "", message.attachments);
    setBubbleTime(item, message.ts || "");
    return item;
  }
  if (message.role === "assistant") {
    const item = turn || createMessage("assistant", "", "");
    if (message.interrupted) markStopped(item);
    if (message.content) item.text += (item.text ? "\n\n" : "") + message.content;
    item.body.innerHTML = basicMarkdown(item.text);
    if (message.reasoning_content) appendReasoning(item, message.reasoning_content);
    // 与实时流顺序一致：先把持久化的工具调用挂到消息节点，收尾留给 finishHistoryTurn。
    // 否则 finishAssistant 会把"无正文、仅工具调用"的消息（自动模式常见）当空气泡移除，
    // 后续 tool 结果找不到对应 call-id，退化为独立 TOOL RESULT 气泡。
    (message.tool_calls || []).forEach(call => {
      let args = {}; try { args = JSON.parse((call.function && call.function.arguments) || "{}"); } catch (_) {}
      const name = call.function && call.function.name;
      // 询问类调用不落成工具条目：末尾待作答时由输入框上方的浮层弹出
      if (name === ASK_USER_TOOL) { item.pendingAsk = args; return; }
      renderToolCall({id:call.id,name,args}, item.node);
    });
    if (message.usage) item.usage = message.usage;
    // 本轮可能由多条 assistant 消息合并，取最后一条的 ts/elapsed_ms（即完成时刻与总耗时）
    if (message.ts) item.ts = message.ts;
    if (message.elapsed_ms != null) item.elapsed_ms = message.elapsed_ms;
    if (message.think_ms != null) item.think_ms = message.think_ms;
    if (message.ttft_ms != null) item.ttft_ms = message.ttft_ms;
    if (message.tps != null) item.tps = message.tps;
    return item;
  }
  if (message.role === "tool") {
    if (message.tool_name === ASK_USER_TOOL) return turn;
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
  // 历史重放：询问浮层留到整段重放完再弹，避免中途的旧询问闪动
  finishAssistant(turn, true);
  const usage = turn.usage;
  if (usage) setBubbleUsage(turn, {total: usage.total, input: usage.input, output: usage.output, estimated: !!usage.estimated, cache_read: usage.cache_read, reasoning: usage.reasoning, model: usage.model});
  // 历史气泡：时间取本轮最后一条消息的 ts（老会话无 ts 则清空默认值），用时取落盘 elapsed_ms
  setBubbleTime(turn, turn.ts || "");
  setBubbleTiming(turn, {elapsed: turn.elapsed_ms, think: turn.think_ms, ttft: turn.ttft_ms, tps: turn.tps});
  return null;
}
// 一条 user 消息之后、下一条 user/workflow 消息之前的 assistant/tool 消息属于同一轮
// 返回最后一轮是否停在未应答的选项/参数卡片上（用于恢复「待确认」徽标）
function renderHistory(messages) {
  let turn = null;
  (messages || []).forEach(message => {
    if (message.role === "assistant" || message.role === "tool") { turn = renderHistoryMessage(message, turn) || turn; return; }
    turn = finishHistoryTurn(turn); renderHistoryMessage(message, null);
  });
  const last = turn;
  finishHistoryTurn(turn);
  // 只有最后一轮仍停在未作答的询问上才弹浮层；已作答/更早的卡片不再出现在历史里
  if (last && last.awaitingInput && last.pendingAsk) openChoiceOverlay(last.pendingAsk);
  return Boolean(last && last.awaitingInput);
}
function showWelcome() { el.messages.innerHTML = '<div id="welcome" class="empty-state"><div class="empty-symbol">⌁</div><strong>对话已就绪</strong><p>描述你的工程目标，Agent 将按当前模式执行。</p></div>'; el.welcome = $("#welcome"); }
// 切离正在流式输出的会话：摘下当前消息 DOM 暂存，回复继续在后台跑，切回时原样挂回
function stashView(id) {
  const fragment = document.createDocumentFragment();
  while (el.messages.firstChild) fragment.append(el.messages.firstChild);
  state.stashed.set(id, fragment);
}
function restoreView(id) {
  const fragment = state.stashed.get(id);
  if (!fragment) return false;
  state.stashed.delete(id); el.messages.append(fragment);
  const stream = state.streams.get(id);
  if (stream) {
    stream.hidden = false;
    // 后台挂起的审批切回来要补弹浮层，否则用户看不到也批不了
    if (stream.approval) openApprovalOverlay(stream.approval.event, id);
  }
  return true;
}
// 流在后台结束时产生的收尾气泡（已停止/失败）要挂回暂存 DOM，别落在用户正在看的会话里
function adoptIntoStash(id, message) {
  if (!message || !hiddenFor(id)) return;
  const fragment = state.stashed.get(id);
  if (fragment) fragment.append(message.node);
}
async function loadSession(id) {
  // 切离正在流式输出的会话：聊天流摘下 DOM 暂存继续在后台跑；工作流直接写可见 DOM，只能中止
  const prev = currentId();
  if (prev && prev !== id) {
    const stream = state.streams.get(prev);
    if (stream && !stream.hidden && !stream.workflow) { stashView(prev); stream.hidden = true; }
    else if (stream && stream.workflow) { const controller = state.controllers.get(prev); if (controller) controller.abort(); }
  }
  try {
    state.session = await request(`/sessions/${encodeURIComponent(id)}`);
    el.currentTitle.textContent = state.session.meta.title;
    const sessionModel = state.models.find(item => modelKey(item) === state.session.meta.model_id || item.model_id === state.session.meta.model_id); if (sessionModel) selectModel(modelKey(sessionModel));
    el.messages.innerHTML = ""; el.phasePanel.classList.add("hidden"); state.workflow = null; closeChoiceOverlay();
    // 有暂存视图（本会话的回复还在流式输出，或刚在后台结束）就直接挂回，不用服务端历史重渲染
    if (!restoreView(id)) {
      if (!state.session.messages.length) showWelcome();
      // 历史最后一轮停在选项/参数卡片且没有活跃流 → 恢复「待确认」徽标
      else if (renderHistory(state.session.messages) && !state.controllers.has(id)) setStatus(id, "waiting");
    }
    if (state.session.plan) renderPhase(state.session.plan);
    state.traj.events = []; state.traj.keys = {}; state.traj.count = {};
    state.traj.records = []; state.traj.selected = null; state.traj.range = null; state.traj.collapsed = {};
    if (state.viewTab === "traj") loadTrajectory();
    closeSessions(); clearAttachments(); syncComposer(); scrollMessages(true);
    // 后台任务可能还在跑（刷新页面/切走时流已断开）：异步探测并重连，恢复实时输出与停止按钮
    maybeReconnect(id);
  } catch (error) { showToast(error.message); }
}
function renderSessions() {
  const query = el.sessionSearch.value.trim().toLowerCase(); el.sessionList.innerHTML = "";
  const sessions = state.sessions.filter(item => !query || String(item.title).toLowerCase().includes(query));
  if (!sessions.length) { el.sessionList.innerHTML = '<div class="empty-state"><p>没有会话</p></div>'; return; }
  sessions.forEach(session => {
    const row = document.createElement("div"); row.className = "session-row";
    // 状态徽标：进行中/已完成/已停止/异常（已完成常驻，直到该会话再次发消息）
    const status = sessionStatus(session);
    const badge = status ? `<i class="session-badge ${status}">${STATUS_TEXT[status]}</i>` : "";
    row.innerHTML = `<button class="session-select" type="button"><span class="session-text"><strong>${escapeHtml(session.title || "未命名会话")}</strong><small>${escapeHtml(String(session.updated_at || session.created_at || "").slice(0,16).replace("T"," "))}</small></span>${badge}</button><div class="session-actions"><button class="rename" type="button" title="重命名">✎</button><button class="clear" type="button" title="清空">⌫</button><button class="danger delete" type="button" title="删除">×</button></div>`;
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
  try { await request(`/sessions/${encodeURIComponent(session.id)}`, {method:"DELETE"}); const controller = state.controllers.get(session.id); if (controller) controller.abort(); state.controllers.delete(session.id); state.streams.delete(session.id); state.stashed.delete(session.id); state.status.delete(session.id); if (state.session && state.session.meta.id === session.id) { state.session = null; el.currentTitle.textContent = "选择会话"; showWelcome(); el.phasePanel.classList.add("hidden"); state.workflow = null; closeChoiceOverlay(); } await refreshSessions(); syncComposer(); } catch (error) { showToast(error.message); }
}
function openSessions() { el.sessionPanel.classList.remove("hidden"); el.sessionTrigger.setAttribute("aria-expanded","true"); el.sessionSearch.focus(); }
function closeSessions() { el.sessionPanel.classList.add("hidden"); el.sessionTrigger.setAttribute("aria-expanded","false"); }

const FAILURE_HINTS = {
  rate_limited: "模型服务商限流",
  network: "后端连不上模型服务",
  stream_error: "模型流式响应中断",
  upstream_http: "模型服务返回错误",
  provider_error: "模型服务异常",
  busy: "上一轮还没停下来",
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
  // 卡片已没有一侧高亮条，错误只标整圈边框
  if (notice.card) notice.card.style.borderColor = "var(--red)";
  if (retry) {
    const button = document.createElement("button");
    button.type = "button"; button.className = "action-button"; button.style.marginTop = "10px";
    button.textContent = "重发这条消息";
    button.addEventListener("click", () => {
      if (isBusy()) { showToast("当前会话还有请求在处理中"); return; }
      button.disabled = true;
      sendMessage(retry.message, retry.display).finally(() => { button.disabled = false; });
    });
    notice.bubble.append(button);
  }
  if (!error.stream) setConnection("offline", "连接异常");
  return notice;
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
// SSE 事件分发：sendMessage 与 reconnectStream 共用，id 为所属会话（可能已被切走），assistant 为本轮回复气泡
function handleStreamEvent(id, type, event, assistant) {
  const hidden = hiddenFor(id);
  if (!hidden && TRAJ_LIVE_TYPES.indexOf(type) >= 0) { trajIngest(event); scheduleTrajRender(); }
  if (type === "text_chunk") { settleThink(assistant); assistant.text += event.delta || ""; assistant.body.innerHTML = basicMarkdown(assistant.text); scrollMessages(); }
  else if (type === "reasoning_chunk") appendReasoning(assistant,event.delta);
  else if (type === "plan_updated") { if (!hidden) renderPhase(event.plan); }
  else if (type === "tool_call") { settleThink(assistant); if (event.name !== ASK_USER_TOOL) renderToolCall(event,assistant.node); }
  else if (type === "tool_result") { if (event.name !== ASK_USER_TOOL) renderToolResult(event,assistant.node); }
  else if (type === "options_offered") { assistant.pendingAsk = event; }
  else if (type === "tool_approval_required") { queueApproval(id, event); }
  else if (type === "skill_loaded") { /* 识别但不在气泡上展示 Skill 标签 */ }
  else if (type === "notice") showToast(event.message || "附件处理提示");
  else if (type === "token_usage") setBubbleUsage(assistant, {total: event.tokens, input: event.tokens_input, output: event.tokens_output, estimated: event.tokens_estimated});
  else if (type === "error") throw streamFailure(event);
  else if (type === "done") { finishAssistant(assistant); const stream = state.streams.get(id); if (stream && assistant.awaitingInput) stream.awaiting = true; setBubbleUsage(assistant, {total: event.tokens, input: event.tokens_input, output: event.tokens_output, estimated: event.tokens_estimated, cache_read: event.cache_read_tokens, reasoning: event.reasoning_tokens, model: event.model}); setBubbleTiming(assistant, {elapsed: event.elapsed_ms, think: event.think_ms, ttft: event.ttft_ms, tps: event.tps}); setBubbleTime(assistant, new Date()); }
}
async function sendMessage(rawMessage = null, displayContent = null, retryAttachments = null) {
  const busyId = currentId();
  if (busyId && state.controllers.has(busyId)) return;
  const message = rawMessage != null ? rawMessage : el.input.value.trim();
  // 附件随消息一起发给大模型：重发沿用原附件，新消息取芯片条里已上传完成的项
  const sentAttachments = retryAttachments != null ? retryAttachments
    : state.attachments.filter(item => !item.uploading).map(item => ({kind:item.kind,name:item.name,path:item.path,url:item.url,media_type:item.media_type}));
  if (!message && !sentAttachments.length) return;
  if (retryAttachments == null && state.uploading > 0) { showToast("附件还在上传中，请稍候再发送"); return; }
  if (!state.session) { await createSession(); if (!state.session) return; }
  // 会话 id 立即锁定：后续任何 await 期间用户切走会话，消息也必须发进原会话
  const sessionId = state.session.meta.id;
  const shown = displayContent != null ? displayContent : message;
  if (retryAttachments == null) clearAttachments();
  // 新一轮提问必须落底并恢复自动跟随，即使用户上一轮上滚停留在历史里
  followBottom = true;
  createMessage("user", shown, "", sentAttachments); el.input.value = ""; autoGrowInput(); updateSendState();
  if ((state.session.meta.title || "").trim() === "New Session" && !state.session.messages.length) {
    const title = message.replace(/\s+/g, " ").trim().slice(0, 10);
    // 重命名不阻塞发送（fire-and-forget），避免 await 期间 state.session 已指向别的会话
    if (title) request(`/sessions/${encodeURIComponent(sessionId)}/rename`, {method:"PUT",body:JSON.stringify({title})}).then(() => {
      const entry = state.sessions.find(item => item.id === sessionId); if (entry) entry.title = title;
      if (state.session && state.session.meta.id === sessionId) { state.session.meta.title = title; el.currentTitle.textContent = title; }
      renderSessions();
    }).catch(() => {});
  }
  const assistant = createMessage("assistant", "", "");
  startLiveTiming(assistant, null);
  const controller = createAbortController();
  state.controllers.set(sessionId, controller);
  state.streams.set(sessionId, {hidden: false});
  setStatus(sessionId, "running");
  syncComposer();
  try {
    const selectedSkills = el.skill.value ? [{id:el.skill.value,params:{}}] : [];
    const response = await fetch("/chat/stream", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:sessionId,message,display_content:shown === message ? "" : shown,interaction_mode:state.mode,model_id:el.model.value || "",selected_skills:selectedSkills,attachments:sentAttachments})});
    await consumeSse(response, async (type, event) => backgroundSafe(sessionId, () => handleStreamEvent(sessionId, type, event, assistant)), controller);
    backgroundSafe(sessionId, () => finishAssistant(assistant));
    // 回合以选项/参数卡片收尾 → 「待确认」，否则「已完成」
    const ended = state.streams.get(sessionId);
    setStatus(sessionId, ended && ended.awaiting ? "waiting" : "done");
    await refreshSessions();
  } catch (error) {
    backgroundSafe(sessionId, () => finishAssistant(assistant));
    const stopped = error.name === "AbortError";
    // 停止时这条气泡若已产出内容（没被 finishAssistant 当空气泡移除），就地在它上面标
    // 「已停止」：与刷新后重建的历史一致。只有一个字都没产出时才另起一条提示气泡，
    // 否则实时视图会多出一条刷新后就消失的提示，看起来像「气泡内容刷新有问题」。
    const kept = stopped && assistant.node.parentNode;
    if (kept) backgroundSafe(sessionId, () => markStopped(assistant));
    const notice = kept ? null : backgroundSafe(sessionId, () => stopped
      ? createMessage("assistant", "已停止接收当前响应。", "STOPPED")
      : renderFailure(error, {message, display: shown, attachments: sentAttachments}));
    if (notice) adoptIntoStash(sessionId, notice);
    // 停止/失败写进会话徽标；停止过的会话不再自动重连，否则切回时又会粘回后台流
    setStatus(sessionId, stopped ? "stopped" : "error");
  } finally {
    if (state.controllers.get(sessionId) === controller) state.controllers.delete(sessionId);
    state.streams.delete(sessionId);
    scrollLocked = false;
    syncComposer();
  }
}
// 重连后台仍在运行的回复流：后端会把断开前的全部事件回放一遍，
// 前端恢复停止按钮状态并继续实时渲染，用户离开前的进度原样接回
async function reconnectStream(sessionId, info, turnTs) {
  const assistant = createMessage("assistant", "", "");
  // 重连气泡时间/计时起点用本轮原始发送时间（落盘 ts），不是刷新时刻
  if (turnTs) setBubbleTime(assistant, turnTs);
  startLiveTiming(assistant, turnTs || null);
  const controller = createAbortController();
  state.controllers.set(sessionId, controller);
  state.streams.set(sessionId, {hidden: false});
  setStatus(sessionId, "running");
  syncComposer();
  let completed = false, endedHidden = false;
  try {
    const response = await fetch("/chat/stream", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:sessionId,message:info.last_message,resume:true,interaction_mode:state.mode,model_id:(state.session && state.session.meta.model_id) || el.model.value || "",selected_skills:[]})});
    await consumeSse(response, async (type, event) => backgroundSafe(sessionId, () => handleStreamEvent(sessionId, type, event, assistant)), controller);
    backgroundSafe(sessionId, () => finishAssistant(assistant));
    completed = true;
    const ended = state.streams.get(sessionId);
    setStatus(sessionId, ended && ended.awaiting ? "waiting" : "done");
  } catch (error) {
    backgroundSafe(sessionId, () => finishAssistant(assistant));
    const notice = backgroundSafe(sessionId, () => error.name === "AbortError"
      ? createMessage("assistant", "已停止接收当前响应。", "STOPPED")
      : renderFailure(error, null));
    adoptIntoStash(sessionId, notice);
    setStatus(sessionId, error.name === "AbortError" ? "stopped" : "error");
  } finally {
    endedHidden = hiddenFor(sessionId);
    if (state.controllers.get(sessionId) === controller) state.controllers.delete(sessionId);
    state.streams.delete(sessionId);
    scrollLocked = false;
    syncComposer();
  }
  // 正常收完后用服务端历史重渲染兜底（任务恰好已结束时后端只回放最终文本，
  // 工具卡片/用量以落盘数据为准）；用户已切走或流被中断时不动当前视图
  if (completed && !endedHidden && state.session && state.session.meta.id === sessionId) {
    try { await refreshSessions(); await loadSession(sessionId); } catch (_) {}
  }
}
// 切回/刷新后：后台任务还在跑但前端没有活跃流时，重建本轮气泡并重连
async function maybeReconnect(id) {
  // 该会话已有活跃流，或用户明确停止过（徽标 stopped）时不重连
  if (state.controllers.has(id) || state.streams.has(id) || state.status.get(id) === "stopped") return;
  let info;
  try { info = await request(`/sessions/${encodeURIComponent(id)}/background`); } catch (_) { return; }
  if (!info || !info.active || !info.last_message) return;
  // 等待状态接口期间用户可能又切走或发了新消息，重连前重新校验
  if (state.controllers.has(id) || state.streams.has(id) || !state.session || state.session.meta.id !== id) return;
  // 裁掉服务端历史里本轮已落盘的部分（用户消息在回合开始即持久化），再重建用户气泡
  const messages = state.session.messages || [];
  const cut = info.turn_msg_count == null ? messages.length : Math.min(info.turn_msg_count, messages.length);
  const kept = messages.slice(0, cut);
  el.messages.innerHTML = "";
  if (kept.length) renderHistory(kept);
  // messages[cut] 即本轮用户消息（turn_msg_count 在其落盘前记录），取原始 ts 显示
  const turnMsg = messages[cut];
  const turnTs = turnMsg && turnMsg.role === "user" && turnMsg.ts ? turnMsg.ts : null;
  const userMsg = createMessage("user", info.display_content || info.last_message || "");
  if (turnTs) setBubbleTime(userMsg, turnTs);
  scrollMessages(true);
  await reconnectStream(id, info, turnTs);
}
async function runWorkflow(steps) {
  const sessionId = currentId();
  if (!state.session || state.controllers.has(sessionId)) return;
  const controller = createAbortController();
  state.controllers.set(sessionId, controller);
  state.streams.set(sessionId, {hidden: false, workflow: true});
  state.workflow = null;
  setStatus(sessionId, "running");
  syncComposer();
  try {
    const response = await fetch("/workflows/run", {method:"POST",headers:{"Content-Type":"application/json"},signal:controller.signal,body:JSON.stringify({session_id:sessionId,steps,selected_skills:el.skill.value?[{id:el.skill.value,params:{}}]:[]})});
    await consumeSse(response, async (type,event) => { event.type = type; if (["workflow_started","workflow_step","workflow_done"].includes(type)) renderWorkflowEvent(event); else if (type === "tool_approval_required") { queueApproval(sessionId, event); } else if (type === "error") throw streamFailure(event, "工作流失败"); }, controller);
    await refreshSessions();
    setStatus(sessionId, "done");
  } catch (error) {
    if (error.name !== "AbortError") showToast(failureText(error).replace(/\n/g, " · "));
    setStatus(sessionId, error.name === "AbortError" ? "stopped" : "error");
  } finally {
    if (state.controllers.get(sessionId) === controller) state.controllers.delete(sessionId);
    state.streams.delete(sessionId);
    syncComposer();
  }
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
function formatTokens(value) { return value >= 1048576 ? `${Math.round(value / 104857.6) / 10}M` : value >= 1024 ? `${Math.round(value / 102.4) / 10}K` : String(value); }
function candidateLimits(model) { return ((state.settings.discoveredModels || {})[model.provider] || []).find(item => (item.id || item.model_id) === model.id) || null; }
function autoLimit(model, name) { const limits = candidateLimits(model), value = limits && limits[name]; return value ? `自动 · ${formatTokens(value)}` : "保存时自动识别"; }
function renderModelCard(model, parent) {
  const card = document.createElement("details"); card.className = "settings-model"; card.innerHTML = `<summary><span><strong>${escapeHtml(model.name || model.id)}</strong><small>${escapeHtml(model.id)}</small></span><code>${escapeHtml(model.api || "继承供应商")}</code><button type="button" class="model-delete" aria-label="删除模型">×</button></summary><div class="model-advanced"><div class="form-grid">${modelField("显示名称","name",model.name || "")}${modelField("Context window","context_window",model.context_window ?? "","number",`min="1" placeholder="${autoLimit(model,"context_window")}"`)}${modelField("Max output tokens","max_output_tokens",model.max_output_tokens ?? "","number",`min="1" placeholder="${autoLimit(model,"max_output_tokens")}"`)}<label class="settings-field"><span>API 协议覆盖</span><select data-model-field="api"><option value="">继承供应商</option><option value="openai-chat">OpenAI Chat</option><option value="openai-responses">OpenAI Responses</option><option value="anthropic-messages">Anthropic Messages</option></select></label></div><div class="capability-grid">${CAPABILITIES.map(([key,label]) => `<label><input type="checkbox" data-capability="${key}" ${model.capabilities && model.capabilities[key] ? "checked" : ""}> ${label}</label>`).join("")}</div></div>`;
  card.querySelector('[data-model-field="api"]').value = model.api || ""; card.querySelectorAll("[data-model-field]").forEach(input => input.onchange = () => { const name = input.dataset.modelField; if (input.type === "number") { const value = input.value.trim(); if (value) model[name] = Number(value); else delete model[name]; } else model[name] = input.value || null; markSettingsDirty(); }); card.querySelectorAll("[data-capability]").forEach(input => input.onchange = () => { if (!model.capabilities) model.capabilities = {}; model.capabilities[input.dataset.capability] = input.checked; markSettingsDirty(); }); $(".model-delete",card).onclick = event => { event.preventDefault(); const deletedKey = `${model.provider}/${model.id}`; state.settings.draft.models = state.settings.draft.models.filter(item => item !== model); if (state.settings.draft.default_model === deletedKey) { const fallback = state.settings.draft.models.find(item => item.enabled !== false); state.settings.draft.default_model = fallback ? `${fallback.provider}/${fallback.id}` : ""; } markSettingsDirty(); renderSettings(); }; parent.append(card);
}
function renderCandidates(candidates, query = "") { const root = $("#model-candidates",el.providerEditor); if (!root) return; const added = new Set(providerModels().map(item => item.id)); root.innerHTML = ""; candidates.filter(item => !query || String(item.id || item.model_id).toLowerCase().includes(query.toLowerCase())).forEach(item => { const id = item.id || item.model_id, button = document.createElement("button"); button.type = "button"; button.disabled = added.has(id); const limits = [item.context_window ? `${formatTokens(item.context_window)} 输入` : "", item.max_output_tokens ? `${formatTokens(item.max_output_tokens)} 输出` : ""].filter(Boolean).join(" · "); button.innerHTML = `<span>${added.has(id) ? "✓" : "+"}</span><strong>${escapeHtml(item.name || id)}</strong><small>${escapeHtml(id)}${limits ? `<em>${escapeHtml(limits)}</em>` : ""}</small>`; button.onclick = () => addModel(id,item.name); root.append(button); }); if (!root.children.length) root.innerHTML = '<div class="listbox-empty">暂无候选，可手动添加</div>'; }
function addModel(rawId, name = "") { const id = String(rawId || "").trim(); if (!id) { el.settingsStatus.textContent = "模型 ID 不得为空"; return; } if (providerModels().some(item => item.id === id)) { el.settingsStatus.textContent = "该模型已添加"; return; } const model = {id,provider:state.settings.activeProviderId,api:null,name:name || id,enabled:true,capabilities:{tools:false,parallel_tools:false,reasoning:false,vision:false,stream_usage:false},compat:{}}; state.settings.draft.models.push(model); if (!state.settings.draft.default_model) state.settings.draft.default_model = `${model.provider}/${model.id}`; markSettingsDirty(); renderSettings(); }
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
  checkVoiceHealth();
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
el.send.onclick = () => { const id = currentId(); if (id && state.controllers.has(id)) stopSession(id); else sendMessage(); };
el.input.oninput = () => { autoGrowInput(); updateSendState(); };
el.input.onkeydown = event => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); if (!isBusy() && !el.send.disabled) sendMessage(); } };
autoGrowInput();
window.addEventListener("resize", autoGrowInput);
// 浮层高度会随卡片折叠/换提问而变，计划窗口得跟着重新贴合（变大顶开、变小落回）
if (typeof ResizeObserver === "function" && el.choiceOverlay) new ResizeObserver(syncPhasePanelLift).observe(el.choiceOverlay);
// Esc 收起询问浮层，露出被盖住的输入框；设置弹窗打开时交给弹窗自己处理。
// 审批卡要等后端回执，Esc 收掉就等于把这次审批漏掉，所以不放行
document.addEventListener("keydown", event => {
  if (event.key !== "Escape") return;
  const modal = $("#settings-modal");
  if (modal && !modal.classList.contains("hidden")) return;
  if (!el.choiceOverlay || el.choiceOverlay.classList.contains("hidden")) return;
  if (el.choiceOverlay.querySelector(".choice-card.approval")) return;
  event.preventDefault();
  closeChoiceOverlay();
});
// --- 附件交互：选择按钮、文件 input、芯片移除、全局拖拽 ---
if (el.attachBtn) el.attachBtn.onclick = () => { if (el.fileInput) el.fileInput.click(); };
if (el.fileInput) el.fileInput.onchange = () => { addFiles(el.fileInput.files); el.fileInput.value = ""; };
if (el.attachBar) el.attachBar.addEventListener("click", event => {
  const button = event.target.closest("[data-remove]");
  if (button) removeAttachment(Number(button.dataset.remove));
});
let dragDepth = 0;
function showDropOverlay(show) { if (el.dropOverlay) el.dropOverlay.classList.toggle("hidden", !show); }
document.addEventListener("dragenter", event => {
  if (!dragHasFiles(event)) return;
  event.preventDefault(); dragDepth += 1; showDropOverlay(true);
});
document.addEventListener("dragover", event => {
  if (!dragHasFiles(event)) return;
  event.preventDefault(); if (event.dataTransfer) event.dataTransfer.dropEffect = "copy";
});
document.addEventListener("dragleave", event => {
  if (!dragHasFiles(event)) return;
  dragDepth = Math.max(0, dragDepth - 1); if (dragDepth === 0) showDropOverlay(false);
});
document.addEventListener("drop", event => {
  if (!dragHasFiles(event)) return;
  event.preventDefault(); dragDepth = 0; showDropOverlay(false);
  if (event.dataTransfer && event.dataTransfer.files) addFiles(event.dataTransfer.files);
});
document.querySelectorAll("[data-mode]").forEach(button => button.onclick = () => { state.mode = button.dataset.mode; document.querySelectorAll("[data-mode]").forEach(item => item.classList.toggle("active",item === button)); });
document.addEventListener("click", event => { if (state.modelListOpen && !event.target.closest(".model-control:not(.skill-control)")) closeModelList(); if (state.skillListOpen && !event.target.closest(".skill-control")) closeSkillList(); if (!el.sessionPanel.classList.contains("hidden") && !event.target.closest("#session-panel,#session-trigger,.dialog-backdrop")) closeSessions(); });
document.addEventListener("keydown", event => { if (event.key === "Escape") { if (state.settings.open) closeSettings(); else { closeModelList(); closeSkillList(); closeSessions(); } } if (event.key === "Tab" && state.settings.open) { const focusable = [...el.settingsModal.querySelectorAll('button:not([disabled]),input:not([disabled]),select:not([disabled]),details summary')].filter(item => item.offsetParent !== null); if (focusable.length && ((event.shiftKey && document.activeElement === focusable[0]) || (!event.shiftKey && document.activeElement === focusable[focusable.length - 1]))) { event.preventDefault(); focusable[event.shiftKey ? focusable.length - 1 : 0].focus(); } } });

// --- 语音转文字（voice_asr）：点击录音 → 浏览器端编码 16kHz WAV → POST /asr → 回填输入框 ---
const voice = {
  ready: false, recording: false, transcribing: false,
  stream: null, context: null, processor: null, source: null, gain: null,
  chunks: [], sourceRate: 16000, timer: null,
  MAX_MS: 300000, TARGET_RATE: 16000,
  // 静音门限：窗口 RMS 低于 VOICE_RMS（约 -40dBFS）视为无声，有效人声不足 MIN_VOICE_MS 则不送识别
  VOICE_RMS: 0.01, MIN_VOICE_MS: 300,
};
// 依据录音/转写/就绪状态切换按钮的 class、disabled 与提示文案（四态）
function setVoiceState() {
  const btn = el.voiceBtn; if (!btn) return;
  btn.classList.toggle("recording", voice.recording);
  btn.classList.toggle("loading", voice.transcribing);
  btn.disabled = !voice.ready || voice.transcribing;
  let label = "语音输入";
  if (voice.recording) label = "停止录音";
  else if (voice.transcribing) label = "转写中";
  else if (!voice.ready) label = "语音服务未就绪";
  btn.title = label; btn.setAttribute("aria-label", label);
}
// 启动时探测后端就绪状态；未就绪则禁用按钮
async function checkVoiceHealth() {
  try { const data = await request("/asr/health"); voice.ready = Boolean(data && data.ready); }
  catch (_) { voice.ready = false; }
  setVoiceState();
}
function stopVoiceStream() {
  if (voice.stream) { voice.stream.getTracks().forEach(track => track.stop()); voice.stream = null; }
}
async function startRecording() {
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) { showToast("当前浏览器不支持录音"); return; }
  try { voice.stream = await navigator.mediaDevices.getUserMedia({audio: true}); }
  catch (error) { showToast("无法访问麦克风：" + (error && (error.message || error.name) || "未知错误")); return; }
  const Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) { showToast("当前浏览器不支持 Web Audio"); stopVoiceStream(); return; }
  // 优先请求 16kHz 上下文；老浏览器忽略该参数时按实际采样率录制并在编码前重采样
  try { voice.context = new Ctx({sampleRate: voice.TARGET_RATE}); }
  catch (_) { voice.context = new Ctx(); }
  voice.sourceRate = voice.context.sampleRate || voice.TARGET_RATE;
  voice.source = voice.context.createMediaStreamSource(voice.stream);
  voice.processor = voice.context.createScriptProcessor(4096, 1, 1);
  voice.gain = voice.context.createGain(); voice.gain.gain.value = 0; // 静音回环，避免扬声器回放麦克风
  voice.chunks = [];
  voice.processor.onaudioprocess = event => { voice.chunks.push(new Float32Array(event.inputBuffer.getChannelData(0))); };
  voice.source.connect(voice.processor);
  voice.processor.connect(voice.gain);
  voice.gain.connect(voice.context.destination);
  voice.recording = true; setVoiceState();
  voice.timer = setTimeout(() => { showToast("已达最长录音时长（5 分钟），自动停止"); stopRecording(); }, voice.MAX_MS);
}
async function stopRecording() {
  if (!voice.recording) return;
  voice.recording = false;
  if (voice.timer) { clearTimeout(voice.timer); voice.timer = null; }
  if (voice.processor) { try { voice.processor.disconnect(); } catch (_) {} voice.processor.onaudioprocess = null; voice.processor = null; }
  if (voice.source) { try { voice.source.disconnect(); } catch (_) {} voice.source = null; }
  if (voice.gain) { try { voice.gain.disconnect(); } catch (_) {} voice.gain = null; }
  const context = voice.context; voice.context = null;
  stopVoiceStream();
  const chunks = voice.chunks; voice.chunks = [];
  let total = 0; chunks.forEach(chunk => total += chunk.length);
  const captured = new Float32Array(total);
  let offset = 0; chunks.forEach(chunk => { captured.set(chunk, offset); offset += chunk.length; });
  if (context && context.state !== "closed") { try { await context.close(); } catch (_) {} }
  const seconds = total / (voice.sourceRate || voice.TARGET_RATE);
  if (total === 0 || seconds < 0.3) { showToast("未录到有效声音，请重试"); setVoiceState(); return; }
  // whisper 对纯静音会输出幻觉文本（实测 1s 静音被识别成"你不要再想我了"），先在前端拦掉
  const voicedMs = measureVoicedMs(captured, voice.sourceRate || voice.TARGET_RATE);
  if (voicedMs < voice.MIN_VOICE_MS) { showToast("未检测到人声，请靠近麦克风重试"); setVoiceState(); return; }
  const resampled = voice.sourceRate === voice.TARGET_RATE ? captured : resampleTo16k(captured, voice.sourceRate);
  await transcribe(encodeWav(resampled, voice.TARGET_RATE));
}
// 以 100ms 为窗计算 RMS，统计达到人声门限的总时长（毫秒）
function measureVoicedMs(samples, sampleRate) {
  const windowSize = Math.max(1, Math.round(sampleRate * 0.1));
  let voicedWindows = 0;
  for (let start = 0; start < samples.length; start += windowSize) {
    const end = Math.min(start + windowSize, samples.length);
    let sum = 0;
    for (let i = start; i < end; i++) sum += samples[i] * samples[i];
    if (Math.sqrt(sum / (end - start)) >= voice.VOICE_RMS) voicedWindows++;
  }
  return Math.round(voicedWindows * windowSize / sampleRate * 1000);
}
// 线性插值重采样到 16kHz（whisper.cpp 要求）
function resampleTo16k(input, fromRate) {
  const toRate = voice.TARGET_RATE;
  if (fromRate === toRate) return input;
  const ratio = fromRate / toRate;
  const outLength = Math.max(1, Math.round(input.length / ratio));
  const output = new Float32Array(outLength);
  for (let i = 0; i < outLength; i++) {
    const pos = i * ratio, idx = Math.floor(pos), frac = pos - idx;
    const a = input[idx] || 0;
    const b = idx + 1 < input.length ? input[idx + 1] : a;
    output[i] = a + (b - a) * frac;
  }
  return output;
}
// 编码 44 字节 RIFF 头的 16bit 单声道 PCM WAV
function encodeWav(samples, sampleRate) {
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view = new DataView(buffer);
  const writeStr = (off, str) => { for (let i = 0; i < str.length; i++) view.setUint8(off + i, str.charCodeAt(i)); };
  writeStr(0, "RIFF"); view.setUint32(4, 36 + samples.length * 2, true); writeStr(8, "WAVE");
  writeStr(12, "fmt "); view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, sampleRate, true); view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  writeStr(36, "data"); view.setUint32(40, samples.length * 2, true);
  let off = 44;
  for (let i = 0; i < samples.length; i++, off += 2) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(off, s < 0 ? s * 0x8000 : s * 0x7fff, true);
  }
  return new Blob([view], {type: "audio/wav"});
}
async function transcribe(blob) {
  voice.transcribing = true; setVoiceState();
  try {
    const form = new FormData(); form.append("audio", blob, "recording.wav");
    const response = await fetch("/asr", {method: "POST", body: form});
    if (!response.ok) {
      let message = `转写失败 (${response.status})`;
      try { const data = await response.json(); message = data.error || data.detail || message; } catch (_) {}
      throw new Error(message);
    }
    const data = await response.json();
    const text = String(data.text || "").trim();
    if (!text) { showToast("未识别到语音内容"); return; }
    appendTranscript(text);
  } catch (error) { showToast((error && error.message) || "转写失败"); }
  finally { voice.transcribing = false; setVoiceState(); }
}
// 将识别文本追加到输入框末尾（必要时补空格），并触发 input 以刷新发送态
function appendTranscript(text) {
  const current = el.input.value;
  const needsSpace = current.length > 0 && !/\s$/.test(current);
  el.input.value = current + (needsSpace ? " " : "") + text;
  el.input.dispatchEvent(new Event("input", {bubbles: true}));
  el.input.focus();
}
function toggleVoice() {
  if (voice.transcribing || !voice.ready) return;
  if (voice.recording) stopRecording(); else startRecording();
}
if (el.voiceBtn) el.voiceBtn.onclick = toggleVoice;

/* ================= 轨迹视图：事件账本投影 ================= */
const TRAJ_LIVE_TYPES = ["user", "traj_system_prompt", "traj_context", "traj_request_start", "traj_request_end", "tool_call", "tool_result"];
const TRAJ_SOURCE_LABEL = {system: "系统", user: "用户", context: "上下文", assistant: "助手", tool: "工具"};
// 时间轴条的颜色类：跟账本徽标 source-* 对应（助手记为 model，其余同名），保证同一来源同色
const TRAJ_SPAN_CLASS = {system: "system", user: "user", context: "context", assistant: "model", tool: "tool"};
const TRAJ_FAILED_RE = /^(Tool error:|Tool execution denied|Tool blocked by active Skill policy|\[deferred\])/;

// 与后端 _estimate_tokens 一致：中日韩 1 token，其余 4 字符 1 token
function trajEstimateTokens(text) {
  const str = String(text == null ? "" : text);
  let cjk = 0;
  for (let i = 0; i < str.length; i++) {
    const code = str.charCodeAt(i);
    if ((code >= 0x3040 && code <= 0x30ff) || (code >= 0x4e00 && code <= 0x9fff) || (code >= 0xac00 && code <= 0xd7af)) cjk += 1;
  }
  return cjk + Math.floor((str.length - cjk) / 4);
}
function trajKey(ev) {
  switch (ev.type) {
    case "user": return "u" + ev.turn;
    case "traj_system_prompt": return "sp" + ev.turn;
    case "traj_context": return "cx" + ev.turn + "." + ev.request + "." + (ev.kind || "");
    case "traj_request_start": return "rs" + ev.turn + "." + ev.request;
    case "traj_request_end": return "re" + ev.turn + "." + ev.request;
    case "tool_call": return "tc" + ev.id;
    case "tool_result": return "tr" + ev.call_id;
    default: return "";
  }
}
// 摄入事件并按「基础键 + 出现序号」去重：落盘回放与实时流顺序一致，序号天然对齐
function trajIngest(events) {
  const st = state.traj;
  const list = Array.isArray(events) ? events : [events];
  for (let i = 0; i < list.length; i++) {
    const ev = list[i];
    if (!ev || !ev.type) continue;
    const base = trajKey(ev);
    if (!base) continue;
    const n = st.count[base] || 0;
    st.count[base] = n + 1;
    const key = base + "#" + n;
    if (st.keys[key]) continue;
    st.keys[key] = true;
    st.events.push(ev);
  }
}
function scheduleTrajRender() {
  const st = state.traj;
  if (st.renderPending) return;
  st.renderPending = true;
  requestAnimationFrame(() => { st.renderPending = false; renderTrajectory(); });
}
function trajTimeMs(rec) {
  const raw = (rec.timing && rec.timing.start) || rec.ts || "";
  const t = Date.parse(raw);
  return isNaN(t) ? NaN : t;
}
function trajFormatMs(ms) {
  if (ms == null || isNaN(ms)) return "—";
  if (ms < 1000) return Math.round(ms) + " 毫秒";
  return (ms / 1000).toFixed(2) + " 秒";
}
// 投影：原始事件流 → 业务记录（加载与实时共用同一套逻辑）
function trajProject(events) {
  const records = [];
  const openRequests = {};
  const openTools = {};
  let seq = 0;
  const push = rec => { rec.id = "r" + (++seq); records.push(rec); return rec; };
  const newRequest = (turn, req, model, startIso, raw) => push({
    source: "assistant", kind: "request", turn: turn, request: req, step: 0,
    status: "running", label: "请求 #" + req, model: model || "", content: "", reasoning: "",
    toolCalls: [], tokens: null, timing: {start: startIso || "", total_ms: null, ttft_ms: null, gen_ms: null, tok_per_s: null},
    raw: raw, ts: startIso || (raw && raw.ts) || "",
  });
  const newTool = (ev, resultEv) => push({
    source: "tool", kind: "tool_call", turn: ev.turn || 0, request: ev.request || 0, step: ev.step || 0,
    status: "running", name: ev.name || "", args: ev.args || {}, callId: ev.id || (resultEv && resultEv.call_id) || "",
    content: "", durationMs: null, tokens: null, timing: null,
    raw: resultEv ? {tool_call: ev, tool_result: resultEv} : ev, ts: (ev && ev.ts) || "",
  });
  events.forEach(ev => {
    const ts = ev.ts || "";
    if (ev.type === "user") {
      const content = ev.display_content || ev.content || "";
      push({source: "user", kind: "user", turn: ev.turn || 0, request: 0, step: 0, status: "completed",
        label: "用户消息", content: content, reasoning: "", model: "", toolCalls: [], timing: null,
        tokens: {total: trajEstimateTokens(content), reasoning: 0, content: trajEstimateTokens(content), estimated: true},
        raw: ev, ts: ts});
    } else if (ev.type === "traj_system_prompt") {
      push({source: "system", kind: "system_prompt", turn: ev.turn || 0, request: 0, step: 0, status: "completed",
        label: "初始系统提示词", content: ev.content || "", reasoning: "", model: "", toolCalls: [], timing: null,
        tokens: {total: trajEstimateTokens(ev.content), reasoning: 0, content: trajEstimateTokens(ev.content), estimated: true},
        raw: ev, ts: ts});
    } else if (ev.type === "traj_context") {
      const kind = ev.kind || "context";
      push({source: "context", kind: kind, turn: ev.turn || 0, request: ev.request || 0, step: 0, status: "completed",
        label: kind === "ledger_snapshot" ? "台账快照" : "上下文注入", content: ev.content || "",
        reasoning: "", model: "", toolCalls: [], timing: null,
        tokens: {total: trajEstimateTokens(ev.content), reasoning: 0, content: trajEstimateTokens(ev.content), estimated: true},
        raw: ev, ts: ts});
    } else if (ev.type === "traj_request_start") {
      openRequests[ev.turn + "." + ev.request] = newRequest(ev.turn, ev.request, ev.model, ev.start || ts, ev);
    } else if (ev.type === "traj_request_end") {
      const mapKey = ev.turn + "." + ev.request;
      let rec = openRequests[mapKey];
      if (!rec) { rec = newRequest(ev.turn, ev.request, ev.model, (ev.timing && ev.timing.start) || ts, ev); openRequests[mapKey] = rec; }
      rec.status = ev.status || "completed";
      rec.model = ev.model || rec.model;
      rec.content = ev.content || "";
      rec.reasoning = ev.reasoning || "";
      rec.toolCalls = (ev.tool_calls || []).map(tc => ({id: tc.id || "", name: tc.name || "", args: tc.args || {}}));
      if (ev.timing) rec.timing = {start: ev.timing.start || rec.timing.start, total_ms: ev.timing.total_ms, ttft_ms: ev.timing.ttft_ms, gen_ms: ev.timing.gen_ms, tok_per_s: ev.timing.tok_per_s};
      rec.raw = ev;
      const usage = ev.usage || {};
      const contentTok = trajEstimateTokens(rec.content);
      const reasoningTok = trajEstimateTokens(rec.reasoning);
      rec.tokens = {
        input: usage.input || null,
        output: usage.output || null,
        total: usage.total || (usage.output || (contentTok + reasoningTok)),
        reasoning: reasoningTok,
        content: contentTok,
        estimated: !usage.total,
      };
      delete openRequests[mapKey];
    } else if (ev.type === "tool_call") {
      openTools[ev.id] = newTool(ev, null);
    } else if (ev.type === "tool_result") {
      let rec = openTools[ev.call_id];
      if (!rec) { rec = newTool({id: ev.call_id, name: ev.name, turn: ev.turn, request: ev.request, step: ev.step, ts: ts}, ev); openTools[ev.call_id] = rec; }
      const result = String(ev.result == null ? "" : ev.result);
      rec.content = result;
      rec.status = TRAJ_FAILED_RE.test(result) ? "failed" : "completed";
      rec.durationMs = typeof ev.duration_ms === "number" ? ev.duration_ms : null;
      if (ev.ts) rec.ts = ev.ts;
      rec.raw = {tool_call: rec.raw && rec.raw.tool_call ? rec.raw.tool_call : rec.raw, tool_result: ev};
      delete openTools[ev.call_id];
    }
  });
  return records;
}
function trajRecordText(rec) {
  const parts = [rec.label || "", rec.name || "", rec.model || "", rec.content || "", rec.reasoning || ""];
  if (rec.args) parts.push(JSON.stringify(rec.args));
  (rec.toolCalls || []).forEach(tc => { parts.push(tc.name); parts.push(JSON.stringify(tc.args || {})); });
  return parts.join("\n");
}
function trajRecordMs(rec) {
  if (rec.kind === "request" && rec.timing && rec.timing.total_ms != null) return rec.timing.total_ms;
  if (rec.source === "tool" && rec.durationMs != null) return rec.durationMs;
  return null;
}
function trajPreview(text, limit) {
  const str = String(text == null ? "" : text).replace(/\s+/g, " ").trim();
  return str.length > limit ? str.slice(0, limit) + "…" : str;
}
// 概述里的参数用缩进 JSON 原样换行展示，长参数不再挤成一行
function trajArgsText(args) {
  let text;
  try { text = JSON.stringify(args == null ? {} : args, null, 2); } catch (_) { text = String(args); }
  return text.length > 1200 ? text.slice(0, 1200) + "\n…" : text;
}
function trajRowMeta(rec) {
  const bits = [];
  if (rec.tokens && rec.tokens.total) bits.push(rec.tokens.total + " tok" + (rec.tokens.estimated ? "≈" : ""));
  const ms = trajRecordMs(rec);
  if (ms != null) bits.push(trajFormatMs(ms));
  return bits.join(" · ");
}
// 账本与时间轴共用的排序：初始系统提示词不属于某一轮，固定排在最前
// 返回 {preamble, rest}：preamble 为首条系统提示词（无则为 null），rest 是其余记录且已剔除它
function trajSplitPreamble(records) {
  const at = records.findIndex(rec => rec.kind === "system_prompt");
  if (at < 0) return {preamble: null, rest: records};
  return {preamble: records[at], rest: records.slice(0, at).concat(records.slice(at + 1))};
}
// 账本行：平铺行、分组展开行、收起时保留的用户消息行共用同一套标记
function trajRowHtml(rec, maxMs, markTurnStart) {
  const cls = ["traj-row", "source-" + rec.source];
  if (rec.id === state.traj.selected) cls.push("selected");
  if (markTurnStart && rec.__turnStart) cls.push("turn-start");
  const main = rec.source === "tool"
    ? `<strong>${escapeHtml(rec.name)}</strong><span class="traj-row-preview">${escapeHtml(trajPreview(rec.status === "running" ? JSON.stringify(rec.args) : rec.content, 160))}</span>`
    : `<span class="traj-row-label">${escapeHtml(rec.label)}${rec.model ? ` <small>${escapeHtml(rec.model)}</small>` : ""}</span><span class="traj-row-preview">${escapeHtml(trajPreview(rec.content, 160))}</span>`;
  // 行内耗时条 = 每条记录的耗时占比（与本视图最长耗时比）：无耗时的记录只画空轨道（不臆造时长），保证右侧 token 列对齐
  const bar = maxMs > 0 ? (() => {
    const ms = trajRecordMs(rec);
    if (ms == null) return '<span class="traj-row-bar" title="该记录无耗时"></span>';
    const pct = Math.max(0.5, (ms / maxMs) * 100);
    const tip = "耗时 " + trajFormatMs(ms) + " · 本视图最长 " + trajFormatMs(maxMs) + "（" + pct.toFixed(1) + "%）";
    return `<span class="traj-row-bar" title="${escapeHtml(tip)}"><i style="width:${pct.toFixed(2)}%"></i></span>`;
  })() : "";
  const statusCls = rec.status === "failed" ? " failed" : (rec.status === "running" ? " running" : "");
  return `<div class="${cls.join(" ")}${statusCls}" role="listitem" tabindex="0" data-id="${rec.id}"><span class="traj-badge-cell"><span class="traj-badge source-${rec.source}">${TRAJ_SOURCE_LABEL[rec.source]}</span></span><span class="traj-row-main">${main}</span><span class="traj-row-meta">${escapeHtml(trajRowMeta(rec))}</span>${bar}</div>`;
}
function renderTrajectory() {
  if (!el.trajView || state.viewTab !== "traj") return;
  const st = state.traj;
  st.records = trajProject(st.events);
  markTurnStarts(st.records);
  el.trajTimeline.classList.remove("hidden");
  renderTrajTimeline();
  const query = st.query.trim().toLowerCase();
  let records = st.records;
  if (query) records = records.filter(rec => trajRecordText(rec).toLowerCase().indexOf(query) >= 0);
  // 选区过滤按时间轴命中的记录 id（见 setRange 注释），不按时间戳回算
  if (st.range) records = records.filter(rec => st.range.ids.indexOf(rec.id) >= 0);
  let maxMs = 0;
  records.forEach(rec => { const ms = trajRecordMs(rec); if (ms != null && ms > maxMs) maxMs = ms; });
  const chunks = [];
  let lastGroup = null;
  if (!records.length) chunks.push('<div class="empty-state"><p>暂无轨迹记录</p></div>');
  // 分组开关未开启（平铺）时逐条平铺，不产生分组头
  const grouped = !!st.view;
  // 初始系统提示词不属于某一轮：任何视图（平铺/轮次/调用）都排到列表最前
  const split = trajSplitPreamble(records);
  const preamble = split.preamble;
  const list = split.rest;
  const groupCounts = {}, groupTok = {}, groupMs = {}, groupSteps = {}, groupTools = {};
  if (grouped) list.forEach(rec => {
    const k = trajGroupKey(st.view, rec);
    groupCounts[k] = (groupCounts[k] || 0) + 1;
    if (rec.tokens && rec.tokens.total) groupTok[k] = (groupTok[k] || 0) + rec.tokens.total;
    const ms = trajRecordMs(rec);
    if (ms != null) groupMs[k] = (groupMs[k] || 0) + ms;
    if (rec.source === "user") return;
    if (rec.kind === "request") groupSteps[k] = (groupSteps[k] || 0) + 1;
    if (rec.source === "tool") groupTools[k] = (groupTools[k] || 0) + 1;
  });
  if (preamble) chunks.push(trajRowHtml(preamble, maxMs, false));
  const summarized = {};
  list.forEach(rec => {
    const groupKey = grouped ? trajGroupKey(st.view, rec) : "";
    if (grouped && groupKey !== lastGroup) {
      lastGroup = groupKey;
      const isCollapsed = !!st.collapsed[groupKey];
      const label = st.view === "turn" ? `第 ${rec.turn} 轮`
        : (rec.request ? `第 ${rec.turn} 轮 · 请求 #${rec.request}` : `第 ${rec.turn} 轮 · 输入与上下文`);
      const metaBits = [groupCounts[groupKey] + " 条"];
      if (groupTok[groupKey]) metaBits.push(groupTok[groupKey] + " tok");
      if (groupMs[groupKey]) metaBits.push(trajFormatMs(groupMs[groupKey]));
      chunks.push(`<div class="traj-group${isCollapsed ? " collapsed" : ""}" role="button" tabindex="0" aria-expanded="${isCollapsed ? "false" : "true"}" data-group="${groupKey}"><span class="traj-group-caret">${isCollapsed ? "▸" : "▾"}</span><span class="traj-group-label">${escapeHtml(label)}</span><small>${groupCounts[groupKey] || 0}</small><span class="traj-group-meta">${escapeHtml(metaBits.join(" · "))}</span></div>`);
    }
    if (grouped && st.collapsed[groupKey]) {
      // 轮次视图收起时保留本轮的用户消息行，其余记录折成一行「… N 个步骤 · M 个工具调用」
      if (st.view === "turn" && rec.source === "user") { chunks.push(trajRowHtml(rec, maxMs, true)); return; }
      if (st.view === "turn" && !summarized[groupKey]) {
        summarized[groupKey] = true;
        chunks.push(`<div class="traj-summary" role="button" tabindex="0" data-group="${groupKey}" title="展开本轮全部记录">… ${groupSteps[groupKey] || 0} 个步骤 · ${groupTools[groupKey] || 0} 个工具调用</div>`);
      }
      return;
    }
    chunks.push(trajRowHtml(rec, maxMs, st.view !== "call"));
  });
  el.trajLedger.innerHTML = chunks.join("");
  renderTrajInspector();
}
// 分组键：轮次开关开启按 turn，调用开关开启按 turn.request；两个开关都关（平铺）时不分组
function trajGroupKey(view, rec) {
  if (!view) return "";
  return view === "call" ? "c" + rec.turn + "." + rec.request : "t" + rec.turn;
}
function trajAllGroupKeys(view, records) {
  if (!view) return [];
  const keys = [];
  records.forEach(rec => { const k = trajGroupKey(view, rec); if (keys.indexOf(k) < 0) keys.push(k); });
  return keys;
}
// 轮次边界粗线：投影后按 turn 变化打标
function markTurnStarts(records) {
  let last = null;
  records.forEach(rec => { rec.__turnStart = rec.turn !== last; last = rec.turn; });
}
// 概览投影由「时长」开关控制，两态（参考 deepseek-harness 的 sequence / duration）：
// 关（默认）= 等宽操作，每个记录等宽、按操作顺序排列，与时间无关；
// 开 = 实际时长，条长对应该记录的耗时，并折叠空转间隔，便于横向比较各步耗时长短。
function renderTrajTimeline() {
  const st = state.traj;
  const spans = [];
  if (!st.actualDuration) {
    // 等宽投影：横轴 = 记录序号，每条记录占 1 格，顺序与账本列表一致；首 token 占该请求格子的前一段
    const ordered = trajSplitPreamble(st.records);
    const sequence = ordered.preamble ? [ordered.preamble].concat(ordered.rest) : ordered.rest;
    let lastTs = 0;
    sequence.forEach((rec, index) => {
      const ts = trajTimeMs(rec);
      if (!isNaN(ts)) lastTs = ts;
      const isTool = rec.source === "tool";
      const isRequest = rec.kind === "request";
      const failed = rec.status === "failed" ? " failed" : "";
      const ms = trajRecordMs(rec);
      const label = isTool ? (rec.name || "工具") : (rec.label || TRAJ_SOURCE_LABEL[rec.source] || "");
      const title = label + (ms == null ? "" : " · " + trajFormatMs(ms));
      // start/end 仍存真实时刻，供时间轴选区反查；ds/de 才是等宽投影坐标
      spans.push({lane: isTool ? 2 : (isRequest ? 1 : 0), start: lastTs, end: lastTs,
        ds: index, de: index + 1, cls: (TRAJ_SPAN_CLASS[rec.source] || "user") + failed,
        id: rec.id, title: title});
      const timing = rec.timing || {};
      if (isRequest && timing.total_ms > 0 && timing.ttft_ms > 0 && timing.ttft_ms < timing.total_ms) {
        spans.push({lane: 1, start: lastTs, end: lastTs, ds: index, de: index + timing.ttft_ms / timing.total_ms,
          cls: "ttft" + failed, id: rec.id, title: title + " · 首 token " + trajFormatMs(timing.ttft_ms)});
      }
    });
  } else {
    st.records.forEach(rec => {
      const ms = trajRecordMs(rec);
      const durTxt = ms == null ? "" : " · " + trajFormatMs(ms);
      if (rec.source === "user") {
        const t = trajTimeMs(rec);
        if (!isNaN(t)) spans.push({lane: 0, start: t, end: t + 1, cls: "user", id: rec.id, title: (rec.label || "用户消息") + " · " + (rec.ts || "")});
      } else if (rec.kind === "request" && rec.timing && rec.timing.start) {
        const t = Date.parse(rec.timing.start);
        if (isNaN(t)) return;
        const total = rec.timing.total_ms != null ? rec.timing.total_ms : 0;
        const title = rec.label + durTxt;
        spans.push({lane: 1, start: t, end: t + Math.max(total, 1), cls: rec.status === "failed" ? "model failed" : "model", id: rec.id, title: title});
        if (rec.timing.ttft_ms != null && rec.timing.ttft_ms > 0) spans.push({lane: 1, start: t, end: t + rec.timing.ttft_ms, cls: "ttft", id: rec.id, title: title + " · 首 token " + trajFormatMs(rec.timing.ttft_ms)});
      } else if (rec.source === "tool" && rec.durationMs != null) {
        const endT = trajTimeMs(rec);
        if (!isNaN(endT)) spans.push({lane: 2, start: endT - rec.durationMs, end: endT, cls: rec.status === "failed" ? "tool failed" : "tool", id: rec.id, title: (rec.name || "工具") + durTxt});
      }
    });
    // 时长：没有任何 span 覆盖的时段（空转）不计入宽度，条长直接对应记录本身的耗时
    let removed = 0, covered = null;
    spans.slice().sort((a, b) => a.start - b.start || a.end - b.end).forEach(sp => {
      if (covered !== null && sp.start > covered) removed += sp.start - covered;
      sp.__idle = removed;
      covered = covered === null ? sp.end : Math.max(covered, sp.end);
    });
    spans.forEach(sp => { sp.ds = sp.start - sp.__idle; sp.de = sp.end - sp.__idle; });
  }
  st.spans = spans;
  const lanes = [[], [], []];
  let dMin = Infinity, dMax = -Infinity;
  spans.forEach(sp => { lanes[sp.lane].push(sp); if (sp.ds < dMin) dMin = sp.ds; if (sp.de > dMax) dMax = sp.de; });
  if (!isFinite(dMin)) { el.trajTimeline.innerHTML = '<div class="traj-lanes"><div class="traj-lane"><span class="traj-lane-label">输入</span><div class="traj-lane-track"></div></div><div class="traj-lane"><span class="traj-lane-label">模型</span><div class="traj-lane-track"></div></div><div class="traj-lane"><span class="traj-lane-label">工具</span><div class="traj-lane-track"></div></div></div>'; st.scale = null; return; }
  // 铺满轨道：两端不留边距，首尾条直接贴住两侧；域宽为 0（单条瞬时记录）时补 1 格避免除零
  if (dMax - dMin <= 0) dMax = dMin + 1;
  st.scale = {min: dMin, max: dMax};
  const names = ["输入", "模型", "工具"];
  const laneHtml = lanes.map((list, idx) => {
    const items = list.map(sp => {
      const left = ((sp.ds - dMin) / (dMax - dMin)) * 100;
      const width = ((sp.de - sp.ds) / (dMax - dMin)) * 100;
      // 相邻条各让出 min(宽度 8%, 1px) 的缝，最短保留 2px 保证可见（与 dsh 的 span 写法一致）
      const gap = Math.min(width * 0.08, 1).toFixed(3);
      const sel = sp.id === st.selected ? " selected" : "";
      const style = `left:calc(${left.toFixed(3)}% + min(${gap}%,1px));width:max(2px,calc(${width.toFixed(3)}% - 2 * min(${gap}%,1px)))`;
      return `<i class="traj-span ${sp.cls}${sel}" data-id="${sp.id}" title="${escapeHtml(sp.title)}" style="${style}"></i>`;
    }).join("");
    return `<div class="traj-lane"><span class="traj-lane-label">${names[idx]}</span><div class="traj-lane-track">${items}</div></div>`;
  }).join("");
  const rangeHtml = st.range ? (() => {
    const left = Math.max(0, ((st.range.d0 - dMin) / (dMax - dMin)) * 100);
    const right = Math.min(100, ((st.range.d1 - dMin) / (dMax - dMin)) * 100);
    return `<div class="traj-range" style="left:${left.toFixed(3)}%;width:${Math.max(right - left, 0).toFixed(3)}%"></div>`;
  })() : "";
  el.trajTimeline.innerHTML = `<div class="traj-lanes">${laneHtml}${rangeHtml}</div>`;
}
function trajInspectorRows(rec) {
  const rows = [];
  const pos = [];
  if (rec.turn) pos.push("第 " + rec.turn + " 轮");
  if (rec.request) pos.push("请求 #" + rec.request);
  if (rec.step) pos.push("步骤 " + rec.step);
  const sourceText = rec.source === "assistant" ? ("请求 #" + rec.request + " ›")
    : rec.source === "tool" ? (rec.name || "工具")
    : TRAJ_SOURCE_LABEL[rec.source];
  rows.push(["来源", escapeHtml(sourceText) + (pos.length ? ` <small>${escapeHtml(pos.join(" · "))}</small>` : "")]);
  rows.push(["状态", rec.status === "completed" ? "已完成" : rec.status === "failed" ? "失败" : rec.status === "interrupted" ? "已停止" : "进行中"]);
  if (rec.tokens) {
    rows.push(["Token", rec.tokens.total + " tok" + (rec.tokens.estimated ? "（估算）" : "")]);
    rows.push(["推理", rec.tokens.reasoning + " tok"]);
    rows.push(["内容", rec.tokens.content + " tok"]);
  }
  if (rec.source === "tool") rows.push(["耗时", trajFormatMs(rec.durationMs)]);
  return rows;
}
function renderTrajInspector() {
  const st = state.traj;
  const rec = st.records.find(item => item.id === st.selected);
  if (!rec) { el.trajInspector.classList.add("hidden"); el.trajInspector.innerHTML = ""; return; }
  el.trajInspector.classList.remove("hidden");
  const pos = [];
  if (rec.turn) pos.push("第 " + rec.turn + " 轮");
  if (rec.request) pos.push("请求 #" + rec.request);
  if (rec.step) pos.push("步骤 " + rec.step);
  const tabs = ["overview", "preview", "raw"];
  const tabLabel = {overview: "概述", preview: "预览", raw: "原始内容"};
  if (tabs.indexOf(st.inspectorTab) < 0) st.inspectorTab = "overview";
  let body = "";
  if (st.inspectorTab === "overview") {
    const rows = trajInspectorRows(rec);
    let html = '<div class="traj-insp-grid">' + rows.map(row => `<div class="traj-insp-row"><span>${row[0]}</span><b>${row[1]}</b></div>`).join("") + "</div>";
    if (rec.source === "tool") html += `<div class="traj-insp-section">参数</div><pre class="traj-insp-args">${escapeHtml(trajArgsText(rec.args))}</pre>`;
    if (rec.kind === "request" && rec.timing) {
      const t = rec.timing;
      const timingRows = [
        ["开始时间", escapeHtml(String(t.start || "—"))],
        ["总时长", trajFormatMs(t.total_ms)],
        ["首 token 延迟", trajFormatMs(t.ttft_ms)],
        ["生成", trajFormatMs(t.gen_ms)],
        ["吞吐量", t.tok_per_s != null ? t.tok_per_s + " tok/s" : "—"],
      ];
      html += '<div class="traj-insp-section">请求计时</div><div class="traj-insp-grid">' + timingRows.map(row => `<div class="traj-insp-row"><span>${row[0]}</span><b>${row[1]}</b></div>`).join("") + "</div>";
    }
    if (rec.kind === "request" && rec.toolCalls && rec.toolCalls.length) {
      html += '<div class="traj-insp-section">工具调用</div><div class="traj-insp-grid">' + rec.toolCalls.map((tc, idx) => `<div class="traj-insp-row"><span>${idx + 1}</span><b>${escapeHtml(tc.name)}</b></div>`).join("") + "</div>";
    }
    body = html;
  } else if (st.inspectorTab === "preview") {
    body = `<div class="traj-insp-preview">${rec.content ? basicMarkdown(rec.content) : '<p class="traj-empty">（无内容）</p>'}</div>`;
  } else {
    body = `<pre class="traj-insp-raw">${escapeHtml(JSON.stringify(rec.raw, null, 2))}</pre>`;
  }
  el.trajInspector.innerHTML =
    `<header class="traj-insp-head"><span class="traj-badge source-${rec.source}">${TRAJ_SOURCE_LABEL[rec.source]}</span><span class="traj-insp-pos">${escapeHtml(pos.join(" · ") || "—")}</span><button class="icon-button" type="button" data-insp-close aria-label="关闭详情">×</button></header>` +
    `<div class="traj-insp-tabs" role="tablist">${tabs.map(tab => `<button type="button" data-insp-tab="${tab}" class="${tab === st.inspectorTab ? "active" : ""}">${tabLabel[tab]}</button>`).join("")}</div>` +
    `<div class="traj-insp-body">${body}</div>`;
}
async function loadTrajectory() {
  if (!state.session) return;
  const st = state.traj;
  if (st.loading) return;
  st.loading = true;
  try {
    const data = await request(`/sessions/${encodeURIComponent(state.session.meta.id)}/trajectory`);
    // 服务端先落盘后入队：落盘集 ⊇ 已实时收到集，重置后回放安全
    st.events = []; st.keys = {}; st.count = {};
    trajIngest(data.events || []);
    renderTrajectory();
  } catch (error) { showToast(error.message); }
  finally { st.loading = false; }
}
function switchViewTab(tab) {
  if (state.viewTab === tab) return;
  state.viewTab = tab;
  const traj = tab === "traj";
  el.tabChat.classList.toggle("active", !traj);
  el.tabChat.setAttribute("aria-selected", String(!traj));
  el.tabTraj.classList.toggle("active", traj);
  el.tabTraj.setAttribute("aria-selected", String(traj));
  el.trajView.classList.toggle("hidden", !traj);
  el.messages.classList.toggle("hidden", traj);
  el.composer.classList.toggle("hidden", traj);
  if (traj) { el.phasePanel.classList.add("hidden"); loadTrajectory(); }
  else { if (el.phasePanel.innerHTML.trim() && !planComplete(state.phasePlan)) el.phasePanel.classList.remove("hidden"); scrollMessages(); }
  syncTurnRail();
}
// 时间轴命中检测：优先取包含该点的最窄跨度，否则取投影上最近的跨度（deepseek-harness 式点击定位）
function trajSpanHit(d) {
  const spans = state.traj.spans || [];
  if (d == null || !spans.length) return null;
  let best = null, bestDist = Infinity, bestWidth = Infinity;
  spans.forEach(sp => {
    const dist = d < sp.ds ? sp.ds - d : (d > sp.de ? d - sp.de : 0);
    const width = sp.de - sp.ds;
    if (dist < bestDist || (dist === bestDist && width < bestWidth)) { best = sp; bestDist = dist; bestWidth = width; }
  });
  return best ? best.id : null;
}
// 选中账本记录：打开详情面板并把账本行滚动到可视区
function selectTrajRecord(id) {
  const st = state.traj;
  st.selected = id;
  renderTrajectory();
  const row = el.trajLedger.querySelector('.traj-row[data-id="' + id + '"]');
  if (row) row.scrollIntoView({block: "nearest"});
}
// 时间轴拖拽选区间；单击（未拖动）定位到该处的账本记录并展示详情
(function bindTrajTimelineDrag() {
  let drag = null;
  const displayAt = clientX => {
    const scale = state.traj.scale;
    // span 按 .traj-lane-track 宽度百分比定位，坐标换算须用轨道矩形（含泳道标签偏移与内边距会失准）
    const track = el.trajTimeline.querySelector(".traj-lane-track");
    const rect = (track || el.trajTimeline).getBoundingClientRect();
    if (!scale || !rect.width) return null;
    const ratio = Math.min(1, Math.max(0, (clientX - rect.left) / rect.width));
    return scale.min + ratio * (scale.max - scale.min);
  };
  // 区间记投影坐标（画选区）+ 命中的记录 id（过滤账本）。
  // 用命中记录而非回算时间戳：等宽投影的横轴是记录序号，且初始系统提示词排在用户消息之前、
  // 时间戳却更晚，按时间戳回算会漏掉选中的记录。
  const setRange = (dA, dB) => {
    const st = state.traj;
    const d0 = Math.min(dA, dB), d1 = Math.max(dA, dB);
    const ids = (st.spans || []).filter(sp => sp.ds <= d1 && sp.de >= d0).map(sp => sp.id);
    st.range = {d0: d0, d1: d1, ids: ids};
  };
  el.trajTimeline.addEventListener("mousedown", event => {
    const d = displayAt(event.clientX);
    if (d == null) return;
    drag = {startD: d, startX: event.clientX, moved: false};
    event.preventDefault();
  });
  document.addEventListener("mousemove", event => {
    if (!drag) return;
    const d = displayAt(event.clientX);
    if (d == null) return;
    if (Math.abs(event.clientX - drag.startX) > 3) drag.moved = true;
    if (!drag.moved) return;
    setRange(drag.startD, d);
    scheduleTrajRender();
  });
  document.addEventListener("mouseup", event => {
    if (!drag) return;
    if (!drag.moved) {
      state.traj.range = null;
      const id = trajSpanHit(displayAt(event.clientX));
      if (id) selectTrajRecord(id);
      else scheduleTrajRender();
    }
    drag = null;
  });
  el.trajTimeline.addEventListener("dblclick", () => { state.traj.range = null; scheduleTrajRender(); });
})();
el.tabChat.addEventListener("click", () => switchViewTab("chat"));
el.tabTraj.addEventListener("click", () => switchViewTab("traj"));
// 工具栏三个按钮都是开关：亮 = 开启。「时长」控概览投影（与分组互不影响）；
// 「轮次」「调用」控分组且两者互斥，都关闭时逐条平铺显示
function trajSyncViewButtons() {
  const st = state.traj;
  Array.prototype.forEach.call(document.querySelectorAll("[data-traj-view]"), other => {
    const view = other.getAttribute("data-traj-view");
    const on = view === st.view;
    other.classList.toggle("active", on);
    other.setAttribute("aria-pressed", String(on));
    const name = view === "turn" ? "轮次分组（每轮带用户消息）" : "模型调用分组";
    other.title = on ? `正在按${name}显示；点击切回平铺` : `点击按${name}显示`;
  });
  const metric = document.querySelector("[data-traj-metric]");
  if (metric) {
    const on = !!st.actualDuration;
    metric.classList.toggle("active", on);
    metric.setAttribute("aria-pressed", String(on));
    metric.title = on ? "正在按实际时长展示（条长=各步耗时，空转已折叠）；点击切到等宽操作" : "正在按等宽操作展示；点击切到实际时长";
  }
}
// 「时长」开关：关 = 真实时间轴（真实耗时）；开 = 按时长对比（折叠空转）。换投影后旧选区不再对应，清掉
document.querySelector("[data-traj-metric]").addEventListener("click", () => {
  const st = state.traj;
  st.actualDuration = !st.actualDuration;
  st.range = null;
  trajSyncViewButtons();
  renderTrajectory();
});
Array.prototype.forEach.call(document.querySelectorAll("[data-traj-view]"), button => {
  button.addEventListener("click", () => {
    const view = button.getAttribute("data-traj-view");
    const st = state.traj;
    // 开关语义：再点已开启的分组按钮即关闭，回到逐条平铺显示
    st.view = st.view === view ? "" : view;
    // 进入分组视图默认全部折叠（每轮/每请求一行摘要）；平铺时无分组可折叠
    st.collapsed = {};
    trajAllGroupKeys(st.view, st.records).forEach(key => { st.collapsed[key] = true; });
    trajSyncViewButtons();
    renderTrajectory();
  });
});
el.trajSearch.addEventListener("input", () => { state.traj.query = el.trajSearch.value; renderTrajectory(); });
el.trajLedger.addEventListener("click", event => {
  const group = event.target.closest(".traj-group, .traj-summary");
  if (group) {
    const key = group.getAttribute("data-group");
    if (state.traj.collapsed[key]) delete state.traj.collapsed[key];
    else state.traj.collapsed[key] = true;
    renderTrajectory();
    return;
  }
  const row = event.target.closest(".traj-row");
  if (!row) return;
  state.traj.selected = row.getAttribute("data-id");
  renderTrajectory();
});
el.trajInspector.addEventListener("click", event => {
  if (event.target.closest("[data-insp-close]")) { state.traj.selected = null; renderTrajectory(); return; }
  const tabBtn = event.target.closest("[data-insp-tab]");
  if (tabBtn) { state.traj.inspectorTab = tabBtn.getAttribute("data-insp-tab"); renderTrajInspector(); }
});
// 点击详情面板、账本行与时间轴之外的区域收起 inspector；
// 用捕获阶段判定，避免各处理器重建 innerHTML 后 event.target 脱离文档导致 contains 误判
document.addEventListener("click", event => {
  const st = state.traj;
  if (state.viewTab !== "traj" || st.selected == null) return;
  const target = event.target;
  if (!target || !target.closest) return;
  if (el.trajInspector.contains(target) || el.trajTimeline.contains(target)
    || target.closest(".traj-row") || target.closest(".traj-group") || target.closest(".traj-summary") || target.closest(".traj-span")) return;
  st.selected = null;
  const sel = el.trajLedger.querySelector(".traj-row.selected");
  if (sel) sel.classList.remove("selected");
  renderTrajInspector();
}, true);

bootstrap();

/* --- 皮肤切换 --- */
const THEME_KEY = "gs-theme";
const THEME_NAMES = { dark: "深色", silver: "银白", blue: "蔚蓝" };
const themeTrigger = $("#theme-trigger");
const themeListbox = $("#theme-listbox");

function applyTheme(name) {
  if (!THEME_NAMES[name]) name = "dark";
  document.documentElement.dataset.theme = name;
  const label = $("#theme-label");
  if (label) label.textContent = THEME_NAMES[name];
  document.querySelectorAll(".theme-option").forEach(btn => {
    const on = btn.dataset.themeOpt === name;
    btn.classList.toggle("active", on);
    btn.setAttribute("aria-selected", on ? "true" : "false");
  });
}

function setThemeListOpen(open) {
  themeListbox.classList.toggle("hidden", !open);
  themeTrigger.setAttribute("aria-expanded", open ? "true" : "false");
}

themeTrigger.addEventListener("click", () => setThemeListOpen(themeListbox.classList.contains("hidden")));
themeListbox.addEventListener("click", event => {
  const opt = event.target.closest(".theme-option");
  if (!opt) return;
  const name = opt.dataset.themeOpt;
  applyTheme(name);
  try { localStorage.setItem(THEME_KEY, name); } catch (err) {}
  setThemeListOpen(false);
});
document.addEventListener("click", event => {
  if (!themeListbox.classList.contains("hidden") && !event.target.closest("#theme-switch")) setThemeListOpen(false);
});
applyTheme(document.documentElement.dataset.theme || "dark");