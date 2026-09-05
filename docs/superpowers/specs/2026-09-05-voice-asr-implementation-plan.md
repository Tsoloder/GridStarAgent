# 语音转文字（Voice ASR）实施计划

## 实施约束

- 依据已批准的 [设计文档](./2026-09-05-voice-asr-design.md) 实施。
- 后端零新增系统依赖（不引入 ffmpeg、pydub、soundfile），仅用标准库 + 现有 fastapi。
- 每阶段先补测试再实现；后端测试 mock `subprocess`，不依赖真实模型/CLI。
- 保持 webui 三文件极简（index.html / app.js / style.css），不引入框架与构建步骤。
- 不改动现有聊天、会话、配置等接口与行为。
- 子进程一律用固定参数列表（非 `shell=True`），临时文件 `finally` 清理。

## 阶段 1：后端模块 `voice_asr/`

### 任务 1.1：配置层 `config.py`

新增：

- `voice_asr/__init__.py`
- `voice_asr/config.py`
- `voice_asr/tests/__init__.py`
- `voice_asr/tests/test_config.py`

实现：

- 用环境变量覆盖默认值：`WHISPER_CLI`、`WHISPER_MODEL`、`WHISPER_LANG`(zh)、`WHISPER_TIMEOUT`(120)、`ASR_MAX_BYTES`(15728640)。
- 默认 CLI=`D:\whiseper_cpp\build_cpu\bin\Release\whisper-cli.exe`、模型=`D:\whiseper_cpp\models\ggml-base.bin`。
- `cli_exists()` / `model_exists()` / `is_ready()`（两者都存在）。
- 配置以 dataclass 或模块级只读对象暴露，便于测试注入。

验证：

```powershell
python -m pytest voice_asr/tests/test_config.py -q
```

### 任务 1.2：转写核心 `transcriber.py`

新增：

- `voice_asr/transcriber.py`
- `voice_asr/tests/test_transcriber.py`

实现：

- 自定义异常：`TranscribeError`（含 `kind`：`cli_missing`/`timeout`/`nonzero_exit`/`empty`）。
- `transcribe(wav_bytes: bytes) -> str`：
  1. 校验非空（空 → `TranscribeError(kind="empty")`）；
  2. `tempfile` 在系统临时目录写 `*.wav`；
  3. `subprocess.run([cli, "-m", model, "-f", tmp, "-l", lang, "-nt"], capture_output=True, timeout=...)`；
  4. 退出码 0 → `stdout.decode("utf-8","ignore")`，按行 `strip()` 去空行后拼接（中文不留空格）；
  5. `finally` 删除临时 WAV；
  6. 异常映射：`FileNotFoundError`→`cli_missing`；`TimeoutExpired`→`timeout`；非零退出→`nonzero_exit`（带 stderr 摘要）。

测试（mock `subprocess.run`）：

- 成功解析含前导/尾随空格与多行；
- 临时文件被清理（mock 后断言 `os.remove`/`unlink` 调用或路径不存在）；
- 超时、非零退出、CLI 缺失、空输入分别映射到对应 `kind`。

验证：

```powershell
python -m pytest voice_asr/tests/test_transcriber.py -q
```

### 任务 1.3：路由层 `router.py`

新增：

- `voice_asr/router.py`
- `voice_asr/tests/test_router.py`

实现：

- `router = APIRouter()`。
- `POST /asr`：`UploadFile`/`File` 读取 `audio` → 校验大小（超 `ASR_MAX_BYTES`→413）→ `run_in_threadpool(transcribe, ...)` → 200 `{"text","language","duration_ms"}`；
  - 未就绪（`is_ready()` False）→ 503；空音频 → 400；超时 → 504；`nonzero_exit` → 500（含 stderr 摘要）。
- `GET /asr/health`：200 `{"ready","cli","model","language"}`。
- `duration_ms` = 后端转写耗时（`time.perf_counter` 计时）。

测试（`TestClient` + mock `transcribe`/`is_ready`）：

- 200 正常返回文本与字段；
- 400 空音频；413 超大；503 未就绪；504 超时；500 非零退出；
- `GET /asr/health` 字段正确。

验证：

```powershell
python -m pytest voice_asr/tests/test_router.py -q
```

### 任务 1.4：许可声明

新增：

- `voice_asr/THIRD_PARTY_LICENSES`

内容：注明 whisper.cpp（ggml）与 OpenAI Whisper 模型均为 MIT，再分发需随附版权声明。

## 阶段 2：集成进 `app.py`

### 任务 2.1：守卫式挂载路由

修改：

- `agent/agent/app.py`

实现：

- `PROJECT_ROOT = Path(__file__).resolve().parents[2]`；若不在 `sys.path` 则 `sys.path.insert(0, str(PROJECT_ROOT))`。
- `try: from voice_asr.router import router as voice_router; app.include_router(voice_router); except Exception as exc: logger.warning("voice_asr 未挂载: %s", exc)`。
- 放在现有路由注册之后、`uvicorn.run` 之前；不影响主应用启动。

验证：

```powershell
python -c "import sys; sys.path.insert(0,'.'); import agent.agent.app" 
python -m pytest agent/agent/tests/test_server_contract.py -q
```

## 阶段 3：前端（webui 三文件）

### 任务 3.1：index.html 加语音按钮

修改：

- `webui/index.html`

实现：

- `.input-wrap` 内 `#send` 旁新增 `#voice-btn`（`type="button"`、`aria-label="语音输入"`、`title`、内联麦克风 SVG）。
- 缓存版本升级：`app.js?v=23`、`style.css?v=18`。

### 任务 3.2：app.js 录音与转写逻辑

修改：

- `webui/app.js`

实现：

- `el.voiceBtn = $("#voice-btn")`；状态：`voiceRecording`、`voiceBusy`、`audioCtx`、`mediaStream`、`processor`、`pcmChunks`、`recordTimer`。
- 启动时 `GET /asr/health`，未就绪 → 禁用按钮 + `title` 提示。
- `startRecording()`：`getUserMedia({audio:true})` → `new AudioContext({sampleRate:16000})` → `createMediaStreamSource` → `createScriptProcessor(4096,1,1)`，`onaudioprocess` 收集 Float32；按钮进入"录音中"态；最长 300s 自动停止。
- `stopRecording()`：停轨/断节点/关 ctx → 合并样本 →（`ctx.sampleRate!==16000` 时 JS 降采样兜底）→ `encodeWav()` → `FormData` POST `/asr` → 成功**追加**文本到 `#message-input` 并 `dispatchEvent(new Event("input"))`；失败状态提示；按钮复位。
- `encodeWav(samples, sampleRate)`：44 字节 RIFF 头 + 16bit PCM 小端单声道。
- 点击切换；`voiceBusy` 期间忽略重复点击；权限被拒/不支持 → 提示并复位。

验证：

```powershell
node --check webui/app.js
```

### 任务 3.3：style.css 按钮样式

修改：

- `webui/style.css`

实现：

- `#voice-btn` 常态 / 录音中（红色脉冲动画）/ 加载（禁用 + spinner）/ 禁用 四态样式，含响应式与 `#send` 对齐。

## 阶段 4：测试与验收

### 任务 4.1：前端契约测试

修改：

- `agent/agent/tests/test_webui.py`

实现：

- 断言 `index.html` 含 `#voice-btn`；
- 断言 `app.js` 含 `encodeWav`、`startRecording`、`/asr` 调用；
- 断言缓存版本已升级（`app.js?v=23`、`style.css?v=18`）。

验证：

```powershell
python -m pytest agent/agent/tests/test_webui.py -q
```

### 任务 4.2：全量验证与浏览器实测

执行：

```powershell
python -m pytest voice_asr agent/agent/tests/test_webui.py -q
python -m compileall voice_asr
node --check webui/app.js
```

浏览器实测（启动服务后）：

- 打开 `http://127.0.0.1:1231/ui`；
- 点击语音按钮 → 授权麦克风 → 说一句话 → 再次点击结束；
- 确认文本回填到输入框（追加而非替换）；
- 验证未就绪时按钮禁用、超时/错误有提示。

验收核对：

- 后端 5 类错误码（400/413/503/504/500）行为正确；
- 临时 WAV 每次清理，无残留；
- 子进程无 shell 注入面；
- 前端录音→转写→回填链路通畅；
- 现有聊天/会话/配置功能不受影响。

## 推荐执行批次

1. 后端 `voice_asr/`（config → transcriber → router → 许可），每任务跑对应 pytest；
2. 集成进 `app.py` 并验证导入与既有契约测试；
3. 前端三文件（按钮 → 逻辑 → 样式），`node --check`；
4. 前端契约测试 + 全量验证 + 浏览器实测。

任何批次失败先修复该批，不跨批堆积未验证改动。
