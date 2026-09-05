# 语音转文字（Voice ASR）设计

日期：2026-09-05
状态：已与用户对齐全部关键决策，待实现

## 1. 背景与目标

在 WebUI 聊天输入区的"发送"按钮旁增加一个"语音"按钮，点击后录音、再次点击结束并转写为文字，回填到消息输入框，供用户校对后再发送。

转写引擎复用本机已编译好的 **whisper.cpp**（CPU 构建）与 **Whisper base 模型**，后端用 **Python** 实现，并放在项目根新增的独立目录 `voice_asr/` 中。

目标：
- 零新增系统依赖（不引入 ffmpeg、pydub、soundfile）。
- 后端模块自包含、可独立测试，通过 FastAPI `APIRouter` 挂载进现有单服务（端口 1231），同源、无跨域。
- 商用合规（whisper.cpp 与模型均为 MIT）。

## 2. 关键决策（已与用户确认）

| 决策点 | 结论 |
|---|---|
| 后端形态 | 新目录 `voice_asr/` 做成 FastAPI `APIRouter`，由 `app.py` `include_router` 挂载（单服务、同源、无跨域） |
| 新目录位置/命名 | 项目根 `d:\TRAE_project\GridStarAgent\voice_asr\` |
| 录音交互 | 点击切换：点一下开始录音、再点一下结束并转写 |
| 转写语言 | 强制中文 `-l zh`（领域为中文，稳定、省一次语言检测） |
| 文本落位 | 转写结果**追加**到输入框已有内容之后（非替换） |
| 转写时机 | 一次性出全文（非实时流式）；转写期间按钮显示加载态 |
| 音频转码 | **浏览器端**用 Web Audio API 采集 PCM 并编码为 16kHz/16bit/单声道 WAV 后上传；后端零音频依赖 |
| whisper-cli 构建 | 用 **CPU 版**（GPU 版启动即崩溃 0xC0000005） |
| 录音节点 | `ScriptProcessorNode`（当前浏览器均可用、可内联、保持 webui 三文件极简）；`AudioWorklet` 列为后续可选升级 |

## 3. 环境探测结论

- 模型：`D:\whiseper_cpp\models\ggml-base.bin`（约 147MB）存在且可用。
- 可执行文件：CPU 版 `D:\whiseper_cpp\build_cpu\bin\Release\whisper-cli.exe` 可用；GPU 版 `D:\whiseper_cpp\build\bin\Release\whisper-cli.exe` 崩溃，弃用。
- 调用约定：`whisper-cli.exe -m <模型> -f <wav> -l zh -nt`
  - **转写文本走 stdout**（`-nt` 时无时间戳；实测文本带**前导空格**，需 `.strip()`）；
  - 计时/诊断走 **stderr**；成功退出码 0。
  - 输入须为 16kHz / 16bit / 单声道 WAV。
  - 1s 静音音频实测约 6.4s（模型加载 ~0.4s + encode/decode）。
- 运行环境：Python 3.13.5，已装 numpy、fastapi；**无 ffmpeg、pydub、soundfile**。
- 权限：进程不可写入 `D:\whiseper_cpp\`（沙箱/权限限制）→ 临时 WAV 必须写到**系统临时目录**（`tempfile`），whisper-cli 只读该路径即可。
- 现有后端：单一 FastAPI 应用 `agent/agent/app.py`（端口 1231），路由用装饰器注册，`/ui` 挂载 `webui/`，对 `127.0.0.1:*` 已开 CORS。
- 现有前端：发送按钮 `#send` 位于 `.input-wrap`，紧邻 `#message-input`；`app.js` 有 `request()`、`el` 映射、`state`。

## 4. 总体架构

```
浏览器(webui)                        后端(同一 FastAPI, 端口1231)              本地 whisper.cpp
┌──────────────┐  点击切换录音   ┌──────────────────────────┐  子进程   ┌────────────────────┐
│ #voice-btn   │ ──────────────> │ voice_asr/ (APIRouter)    │ ────────> │ whisper-cli.exe(CPU)│
│ Web Audio 采集│  POST /asr      │  POST /asr  GET /asr/health│  临时WAV  │ ggml-base.bin       │
│ →16kHz WAV   │ <────────────── │  落临时WAV→跑CLI→读stdout │ <──────── │ -l zh -nt           │
└──────────────┘   {text}        └──────────────────────────┘   文本     └────────────────────┘
       │ 追加到 #message-input
```

## 5. 后端模块设计（`voice_asr/`）

| 文件 | 职责 |
|---|---|
| `__init__.py` | 包标识，导出 `router` |
| `config.py` | 配置项与默认值，全部支持环境变量覆盖；提供 `is_ready()`（exe 与模型是否都存在） |
| `transcriber.py` | 核心：`transcribe(wav_bytes) -> str`；落临时 WAV → `subprocess.run` 调 CLI（固定参数列表，无 shell）→ 取 `stdout.strip()` → `finally` 清理临时文件；区分超时/CLI 缺失/非零退出等错误 |
| `router.py` | `APIRouter`：`POST /asr`（multipart `audio`）→ 线程池执行 `transcribe` → JSON；`GET /asr/health` 报告就绪状态 |
| `THIRD_PARTY_LICENSES` | 注明 whisper.cpp 与 Whisper 模型均为 MIT |
| `tests/` | 模块自带 pytest（mock 子进程，不依赖真实模型） |

### 5.1 配置（`config.py`）

| 配置 | 环境变量 | 默认值 |
|---|---|---|
| CLI 路径 | `WHISPER_CLI` | `D:\whiseper_cpp\build_cpu\bin\Release\whisper-cli.exe` |
| 模型路径 | `WHISPER_MODEL` | `D:\whiseper_cpp\models\ggml-base.bin` |
| 语言 | `WHISPER_LANG` | `zh` |
| 超时(秒) | `WHISPER_TIMEOUT` | `120` |
| 上传上限(字节) | `ASR_MAX_BYTES` | `15728640`（约 15MB） |

> 路径可被环境变量覆盖，避免写死（注意目录名实际为 `whiseper_cpp`，含拼写）。

### 5.2 API 契约

`POST /asr`
- 请求：`multipart/form-data`，字段 `audio`（WAV 二进制）。
- 成功 `200`：`{"text": "...", "language": "zh", "duration_ms": 6436}`
  （`duration_ms` = 后端转写耗时毫秒数，仅用于前端提示/日志，非音频时长）
- 错误：
  - `400` 空音频/无 `audio` 字段
  - `413` 超过 `ASR_MAX_BYTES`
  - `503` CLI 或模型缺失（未就绪）
  - `504` 转写超时
  - `500` whisper-cli 非零退出（响应含 stderr 摘要）

`GET /asr/health`
- `200`：`{"ready": true, "cli": true, "model": true, "language": "zh"}`

### 5.3 转写流程（`transcriber.py`）

1. 校验 `wav_bytes` 非空（可选校验 RIFF 头）。
2. `tempfile` 在系统临时目录写 `*.wav`。
3. `subprocess.run([cli, "-m", model, "-f", tmp, "-l", lang, "-nt"], capture_output=True, timeout=...)`。
4. 退出码 0 → `text = stdout.decode("utf-8", "ignore").strip()`；多行则去除空行后拼接（中文不留空格）。
5. `finally` 删除临时 WAV。
6. 异常映射：`FileNotFoundError`→CLI 缺失；`TimeoutExpired`→超时；非零退出→带 stderr 摘要的错误。
7. 该函数为阻塞式，由路由层用 `run_in_threadpool`  offload，避免阻塞事件循环。

## 6. 与 `app.py` 集成（沿用其"守卫式导入"风格）

- 计算 `PROJECT_ROOT = Path(__file__).resolve().parents[2]`，若不在 `sys.path` 则插入。
- `try: from voice_asr.router import router as voice_router; app.include_router(voice_router)`；
  `except Exception: logger.warning(...)` 并跳过（voice_asr 异常不影响主应用启动）。

## 7. 前端设计（webui 三文件）

### 7.1 index.html
- `.input-wrap` 内 `#send` 旁新增 `#voice-btn`（麦克风 SVG 图标，`type="button"`，`aria-label="语音输入"`，`title`）。
- 缓存版本升级：`app.js?v=23`、`style.css?v=18`。

### 7.2 app.js
- `el.voiceBtn = $("#voice-btn")`；新增状态：`voiceRecording`、`voiceBusy`、`audioCtx`、`mediaStream`、`processor`、`pcmChunks`。
- `startRecording()`：`getUserMedia({audio:true})` → `new AudioContext({sampleRate:16000})` → `MediaStreamAudioSourceNode` → `ScriptProcessorNode(4096,1,1)`，`onaudioprocess` 收集 Float32；按钮进入"录音中"态。
- `stopRecording()`：停轨、断开节点、关闭 AudioContext → 合并样本 →（若 `ctx.sampleRate !== 16000` 则 JS 降采样兜底）→ `encodeWav()` → `FormData` POST `/asr` → 成功后**追加**文本到 `#message-input` 并 `dispatchEvent(new Event("input"))`；失败给出状态提示；按钮复位。
- `encodeWav(samples, sampleRate)`：写 44 字节 RIFF 头 + 16bit PCM 小端单声道。
- 点击切换；`voiceBusy`（转写中）忽略重复点击；启动时 `GET /asr/health`，未就绪则禁用按钮并提示。
- 客户端最长录音自动停止（默认 300s）。

### 7.3 style.css
- `#voice-btn` 常态 / 录音中（红色脉冲）/ 加载（禁用 + spinner）/ 禁用 样式，含响应式。

## 8. 数据流

点击开始 → 采集 16kHz PCM → 点击结束 → 编码 WAV → `POST /asr` → 后端落临时文件跑 CLI 读 stdout → 返回 `{text}` → 追加进输入框 → 用户校对后按原有"发送"。

## 9. 错误处理与限制

- 麦克风权限被拒 / 浏览器不支持 `getUserMedia`/`AudioContext` → 状态提示并复位按钮（localhost 视为安全上下文，`getUserMedia` 可用）。
- 空录音 → `400`"未录到声音"；超大 → `413`。
- CLI/模型缺失 → `503`（health 预先禁用按钮）；超时 → `504`；CLI 非零退出 → `500`（带 stderr 摘要）。
- 客户端最长录音自动停止，避免超长音频导致转写过久。

## 10. 安全

- 子进程使用**固定参数列表**（非 `shell=True`），输入为后端控制的临时文件路径，无命令注入风险。
- 语言取自配置（非用户输入）；如后续开放用户指定语言，需白名单校验。
- 上传大小上限 + 临时文件 `finally` 清理。

## 11. 测试计划

- 后端 `voice_asr/tests/`（mock `subprocess`，不依赖真实模型）：
  - `test_transcriber`：成功解析（含前导空格 strip）、临时文件清理、超时映射、非零退出映射、CLI 缺失映射。
  - `test_router`：`TestClient` 验证 `POST /asr` 200/400/413/503 与 `GET /asr/health`。
  - 运行：项目根 `python -m pytest voice_asr -q`。
- 前端：`node --check webui/app.js`；在 `agent/agent/tests/test_webui.py` 增加断言（`#voice-btn` 存在、app.js 含录音/编码函数）。
- 浏览器实测：录音 → 转写 → 文本回填（后端与 whisper-cli 真实链路）。

## 12. 性能预期与后续可选优化（YAGNI）

- CPU 版 base 模型：模型加载 ~0.4s + 处理（1s 音频实测约 6s，随时长增长）。
- whisper-cli 为一次性进程，每次请求重新加载模型。短语音可接受，转写期间显示加载态。
- 若日后延迟敏感，可升级为常驻进程或 `whisper-server.exe`（当前不做）。

## 13. 许可与合规

- whisper.cpp（含 ggml）：**MIT**（本地 `D:\whiseper_cpp\LICENSE` 与 GitHub 均确认）。
- OpenAI Whisper 模型（含 base 权重）：**MIT**，代码与权重同许可，可自由商用。
- 本方案**不使用 ffmpeg**（浏览器端编码 WAV），规避 GPL/LGPL 分发义务；用 **CPU 构建**，不涉及 CUDA 再分发。
- 唯一义务：若对外**再分发** whisper.cpp 或模型，需随附 MIT 版权/许可声明。`voice_asr/THIRD_PARTY_LICENSES` 予以标注。
- 新写的 Python 后台模块与前端改动版权归本项目，无限制。
