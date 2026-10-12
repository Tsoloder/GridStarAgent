# Whisper 语音识别集成 WebUI —— 实施方案

> 状态：方案（待评审）
> 目标模型：`D:\TRAE_project\GridStar-history-package\GS_Pack-20261010155413\res\whisperModels\ggml-base.bin`
> 相关既有文档：[voice-asr-design.md](docs/superpowers/specs/2026-09-05-voice-asr-design.md)、[voice-asr-implementation-plan.md](docs/superpowers/specs/2026-09-05-voice-asr-implementation-plan.md)

---

## 一、结论先行

GS 包里的 whisper 资产是**可用的**，本方案把它接进 WebUI。但直接走"Python ctypes 调 `whisper.dll`"这条路会踩一个很深的坑，所以方案采用 **C++ shim 中转**的结构。

已在本机实测通过（证据见第七节）：

| 用例 | 结果 |
|---|---|
| 英文 JFK 11.0s | `And so my fellow Americans, ask not what your country can do for you, ask what you can do for your country.` 逐字正确，RTF 0.18 |
| 中文 TTS 5.0s | `生成一个原住体的网格,边界层设置武层。` RTF 0.42（原文："生成一个圆柱体的网格，边界层设置五层"） |
| 模型加载 | 0.24s（GPU 缺失时自动回落 CPU），模型 147.37MB / type=base / n_langs=99 |

---

## 二、为什么不直接用 ctypes 调 whisper.dll

这是本方案最重要的一条设计约束，值得先讲清楚。

`whisper_full()` 的第二个参数 `whisper_full_params` 是一个约 300 字节的结构体，**由值传递**。Python 侧要用 ctypes 调它，就必须手写这个结构体的精确布局。而实测表明：

- 包内 `whisper.dll` 自报版本 **1.8.3**（`whisper_version()` 返回）
- 用公开 v1.8.3 头文件布局手写的 ctypes 结构体喂进去，`whisper_full` 返回 0（"成功"），但输出是乱码；且同一段音频每次结果不同，说明读到了未初始化字段
- 表现为：同一段中文音频，有时输出繁体夹杂英文，有时输出重复循环（`body's body's body's...`）

> **更正（2026-10-12）**：本方案早期版本曾断言「该 DLL 的 `whisper_full_params`
> 布局与公开 v1.8.3 头文件不一致，指针字段整体后移 8 字节」。该结论**不成立，已撤回**。
> 当时是用「逐字节 dump DLL 默认参数块」的方式反推偏移，同一份内存重复 dump 得到过互相
> 矛盾的结果，据此推出的布局自然不可信。后续用真正的头文件编译 shim 后，
> `DLL 中 structure size = sizeof(whisper_full_params) = 304`，与 v1.8.3 头文件**完全一致**，
> 且全链路转写正确（英文 JFK RTF 0.173、中文实测文本正确）。
> 即：DLL 与头文件是匹配的，出问题的只是"靠内存 dump 猜布局"这一手段。

**根因**：手写 ABI 猜测布局，任何一处偏移错位都会静默产生垃圾输出，且不会报错。

**解法**：让编译器来算布局。写一个薄 C++ 层，用真头文件编译，对外只暴露参数全是标量的 C 函数。Python 侧只认这 4 个函数，永远不接触结构体。

代价是引入一个自编译的 `asr_shim.dll`（约 155KB）；收益是把整类 ABI 风险一次性消除。

---

## 三、目标架构

```
浏览器                          后端（FastAPI 单服务，同源无跨域）
┌──────────────────────┐        ┌────────────────────────────────────────┐
│ getUserMedia         │        │ voice_asr/router.py                    │
│ 16kHz / 单声道        │        │   POST /asr        音频 -> 文本         │
│ ScriptProcessor      │  WAV   │   GET  /asr/health 就绪状态             │
│ Float32 -> Int16     │ ─────► │            │                           │
│ encodeWav            │        │            ▼                           │
│                      │        │ voice_asr/transcriber.py               │
│ fetch POST /asr      │        │   WAV 字节 -> float32 PCM -> shim 调用  │
│                      │ ◄───── │            │                           │
│ 追加到输入框          │  JSON  │            ▼                           │
└──────────────────────┘        │ voice_asr/native/asr_shim.dll (C++)    │
                                │   结构体在 C++ 侧填好                   │
                                │            │                           │
                                │            ▼                           │
                                │ whisper.dll (包内, v1.8.3)             │
                                │ ggml.dll / ggml-base.dll / ggml-cpu.dll│
                                │ res/whisperModels/ggml-base.bin (147MB)│
                                └────────────────────────────────────────┘
```

关键决策：

| 项 | 决策 | 理由 |
|---|---|---|
| 进程模型 | **模型常驻内存**，模块级单例 + 锁 | 现状 `transcribe()` 每次起一个 `whisper-cli.exe` 子进程，每次都要重新加载 147MB 模型；常驻后首次 0.24s，之后每次仅 ~2s（5s 音频） |
| GPU | `use_gpu=0` 强制 CPU | 包内 whisper.dll 是 CUDA 构建，但包里没有 `ggml-cuda.dll`；本机实测日志 `whisper_backend_init_gpu: no GPU found` 后自动回落 CPU。显式关掉可避免在有 CUDA 的机器上因缺 DLL 而初始化失败 |
| 音频格式 | 浏览器直接产出 **16kHz/单声道/float32** | 与 shim 入参零转换。`sample_rate=16000` 必须显式传给 `AudioContext` |
| 中文输出 | 内置简体中文 `initial_prompt` | base 模型默认输出繁体（实测"一個圓住體"→"一个原住体"）。这是 whisper.cpp 官方推荐做法，不改变模型文件 |
| 时间戳 | `want_ts=1` 时启用真实时间戳预测 | `no_timestamps=true` 时段落 t0/t1 是未初始化值（实测恒为 `-1007.28`）。另外 whisper 会把输入补齐到 30s 窗口，末段 t1 会虚报到 30s，需按真实采样点数夹取 |
| 依赖 | 不引入任何新 Python 包 | 只用 `ctypes` / `wave` / `array` 标准库，内网离线安装不受影响 |

---

## 四、C++ shim 设计（语音识别的对接面）

### 4.1 对外 API（`voice_asr/native/asr_shim.h`）

```c
int         asr_init(const char * model_path, int use_gpu, int n_threads);
int         asr_transcribe(const float * samples, int n_samples,
                           const char * language, int translate, int want_ts,
                           char * out_ts, int out_ts_cap);
const char * asr_last_text(void);
int         asr_set_prompt_for(const char * language, const char * prompt);
void        asr_free(void);
const char * asr_backend_version(void);
int         asr_effective_threads(void);
```

参数全是 `int` / `const char*`，没有任何结构体跨越 DLL 边界。

### 4.2 shim 内部做掉的事

- `whisper_context_default_params()` / `whisper_full_default_params(WHISPER_SAMPLING_GREEDY)` 由 C++ 填
- `p.use_gpu`、`p.n_threads`、`p.no_context`、`p.no_timestamps = (want_ts==0)`
- `p.initial_prompt` 按语言选择（`zh` 走内置简体提示词）
- `p.greedy.best_of=1`、`p.temperature=0.0f`、`p.temperature_inc=0.0f` → 关闭温度回退，同一段音频结果稳定可复现
- 关闭所有 `print_*`，避免 whisper 日志污染宿主控制台
- 时间戳按真实采样点数夹取

### 4.3 构建

只在本机（有 MSVC 的机器）构建一次，产物随包分发；内网目标机不需要编译器。

过程：`dumpbin /exports whisper.dll` → 生成 `.def`（900 个导出符号）→ `lib /def` 生成导入库 → `cl /LD` 编译链接。

环境依赖（已确认本机具备）：MSVC 14.51.36231（VS 18 Community）、CMake、Python 3.13。

---

## 五、改动清单

### 后端

| 文件 | 改动 |
|---|---|
| `voice_asr/native/asr_shim.h` | 新增（shim 头文件） |
| `voice_asr/native/asr_shim.cpp` | 新增（shim 实现） |
| `voice_asr/native/build_shim.bat` | 新增（本机构建脚本，含 vcvars 调用） |
| `voice_asr/native/make_def.py` | 新增（解析 dumpbin 输出生成 .def） |
| `voice_asr/native/bin/asr_shim.dll` | 新增（构建产物，随包分发，约 155KB） |
| `voice_asr/shim.py` | 新增（ctypes 绑定：加载、声明 argtypes、GIL 释放、错误映射） |
| `voice_asr/config.py` | 改：定位包内 `whisper.dll`、`res/whisperModels/ggml-base.bin`；`is_ready()` 改为校验 4 个 DLL + 模型文件；新增 `ASR_USE_GPU` / `ASR_PROMPT_ZH` |
| `voice_asr/transcriber.py` | 改：删掉 `subprocess` 调 CLI 的整条路径，改为 WAV→float32→shim；保留 `TranscribeError` 的 kind 分类（`cli_missing` → `backend_missing`，`nonzero_exit` → `backend_error`） |
| `voice_asr/router.py` | 小改：`/asr/health` 增加 `backend` / `threads` / `model_type` 字段；其余契约不变 |
| `voice_asr/tests/*` | 改：`test_transcriber.py` 从 mock `subprocess` 改为 mock `shim`；新增"模型缺失时不抛异常而是返回未就绪"用例 |
| `package_intranet.py` | 改：新增 `copy_binary_assets()`，把模型与 4 个 DLL 打进 zip（`.gitignore` 除外，见 5.3） |
| `voice_asr/THIRD_PARTY_LICENSES.md` | 新增：whisper.cpp / ggml 均为 MIT，附上游链接与版本 1.8.3 |

### 前端

| 文件 | 改动 |
|---|---|
| `webui/app.js` | 改：`encodeWav` 输出改 **float32** 而不仅是 int16；`AudioContext({sampleRate:16000})`；`fetch("/asr")` 的 `Content-Type` 与响应处理对齐；`appendTranscript()` 已存在，保持追加语义 |
| `webui/style.css` | 改：`.voice.recording` / `.voice.loading` / `.voice:disabled` 与两个 keyframes 已存在，仅微调 |
| `webui/index.html` | 改：按钮与 aria-label 已存在，仅升版本号 |

### 5.3 打包（这是当前最容易漏的一环）

`collect_source_files()` 走的是 `git ls-files`，因此**未被 git 跟踪的文件不会进包**。模型 147MB 与二进制 DLL 都不适合进 git，必须显式复制：

```
zip 内布局
├── voice_asr/native/bin/asr_shim.dll
├── voice_asr/runtime/whisper.dll
├── voice_asr/runtime/ggml.dll
├── voice_asr/runtime/ggml-base.dll
├── voice_asr/runtime/ggml-cpu.dll
└── voice_asr/runtime/models/ggml-base.bin   (147MB)
```

`ggml-cpu.dll` 必须一起带上：whisper.dll 启动时要加载 CPU 后端。

### 5.4 测试断言需同步

`agent/agent/tests/test_webui.py` 把前端资源版本号写死在断言里，且**出现 3 处**（第 299、300 行与第 332、333 行）：

```
assert "app.js?v=75" in index
assert "style.css?v=58" in index
```

改前端必须同步升版本号并改这些断言，否则测试红。

---

## 六、实施顺序

1. 把已验证的 shim 源码从 `preview/_shim/` 落到 `voice_asr/native/`，产出 `bin/asr_shim.dll`
2. `voice_asr/shim.py`：ctypes 绑定 + 单例 + 锁；用 `ctypes.CDLL(..., winmode=...)` 或 `os.add_dll_directory()` 让 `asr_shim.dll` 能找到同目录的 `whisper.dll`
3. `config.py` 改为包内路径自解析（`Path(__file__).parents[1] / "runtime"`），保留环境变量覆盖
4. `transcriber.py` 换后端，跑通 `python -m pytest voice_asr -q`
5. 前端三件套改造 + 版本号 + 改测试断言
6. `package_intranet.py` 加二进制复制，本地解压 zip 验证
7. 端到端实测：起服务、点麦克风、说"生成一个圆柱体的网格"，确认文本进输入框

---

## 七、已完成的实测证据

全部在本机跑通，报告落盘在 `preview/_shim/`：

| 文件 | 内容 |
|---|---|
| `preview/_shim/verify_report.json` | 4 个用例（中/英 × 指定语言/自动检测/翻译）的文本、时间戳、RTF、UTF-8 原始 hex |
| `preview/_shim/prompt_report.json` | `initial_prompt` 对简体输出的对照组实验 |

关键数值：

- `backend_version: 1.8.3`（`whisper_version()` 实测）
- 模型：`type=2 (base)`、`n_vocab=51865`、`n_audio_layer=6`、`ftype=1`、`CPU total size = 147.37 MB`、`n_langs=99`
- 延迟：模型加载 0.237s；11.0s 英文音频 1.90s（RTF 0.173）；5.0s 中文音频 2.11s（RTF 0.422）
- `initial_prompt` 对照组：
  - 空提示词 → `生成一個圓住體的網格,編界層設置武層。`（繁体）
  - 内置简体提示词 → `生成一个原住体的网格,边界层设置武层。`（简体 + 标点规范化）
- 时间戳：11.0s 音频 → `0.00:11.00`；5.0s 音频 → `0.00:5.01`（夹取生效，不再是 `-1007.28`）

---

## 八、已知限制与后续可选项

### 8.1 `base` 模型的能力上限

实测中文错字（`圆柱体`→`原住体`、`五层`→`武层`）不是集成问题，是 base 模型的容量问题。可选改善：

- 换 `ggml-small.bin`（约 466MB，中文明显更准）。代码无需改动，只换模型文件路径
- 在 `initial_prompt` 里塞 CAD 领域高频词（实测对本次短句无额外增益，但对长句通常有效）

### 8.2 兼容包里更新的版本

包里还有 `GS_Pack-20260924-112531`，同样带 whisper.dll 与 ggml-base.bin。方案按路径配置，换包只需改一个常量。

### 8.3 流式识别

当前是一次性出全文（沿用既有设计决策）。若要做边说边出，需要改 shim 用回调式 `whisper_full` + 前端分片上传，属于独立迭代，建议先不做。

---

## 九、需要确认的两个点

1. **模型分发**：`ggml-base.bin` 147MB 直接打进内网 zip 会让包体积增加约 147MB。是否接受？替代做法是首次启动时从内网共享目录拷贝。
2. **`asr_shim.dll` 的来源**：方案假设"本机构建一次、随包分发"。如果内网部署流程要求所有二进制都能从源码重建，需要把 MSVC 构建步骤也写进部署文档，或改为在打包机上构建后固化校验和。

---

*文档版本：v1，基于 2026-10 实测数据*