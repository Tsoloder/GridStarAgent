# voice_asr 语音识别模块

把浏览器录到的语音转成文字，追加到 WebUI 的输入框。整个模块自包含在
`voice_asr/` 目录内，拷走这个目录就能跑，不依赖开发机上的 `D:\whiseper_cpp`。

---

## 一、它是怎么跑起来的

```
浏览器                                后端（FastAPI 单进程单端口，同源无跨域）
──────────────────────────────        ──────────────────────────────────────────
点击麦克风按钮 → getUserMedia
  → Web Audio 采集
  → 编码 16kHz / 16bit / 单声道 WAV
  → POST /asr (multipart, 字段名 audio) ──►  voice_asr/router.py
                                              ├─ config.load_settings() 判断就绪
                                              └─ run_in_threadpool(transcribe)
                                                   └─ voice_asr/transcriber.py
                                                        └─ voice_asr/shim.py
                                                             └─ asr_shim.dll (C++)
                                                                  └─ whisper.dll + ggml-base.bin
  ◄── {"text", "language", "duration_ms", "backend_ms"}
  → appendTranscript() 追加进输入框
```

关键点：

- **音频在浏览器端编码**，后端不引入 ffmpeg / pydub / soundfile，零音频依赖，
  也规避了 ffmpeg 的 GPL/LGPL 分发义务。
- **模型常驻内存**，进程内复用，不做子进程。首次转写时才加载模型
  （C++ 侧 `whisper_init` 实测 0.24s，整个首次请求含推理 1.78s），之后每段音频
  只算推理时间（实测中文 5s 音频 1.29s，RTF ≈ 0.26；英文 11s 音频 1.9s，RTF ≈ 0.17）。
- **为什么有一层 C++ shim**：`whisper_full()` 的第二个参数是按值传递的
  ~300 字节结构体，Python 用 ctypes 手写它的布局，一旦偏移错位
  `whisper_full` 仍返回 0（"成功"）却输出乱码，属于静默错误。让编译器算布局，
  Python 侧只接收标量参数，整类 ABI 风险一次性消除。

---

## 二、文件构成

### 代码

| 文件 | 作用 |
| --- | --- |
| `__init__.py` | 导出 `router` |
| `config.py` | 路径解析 + 环境变量覆盖 + 就绪判断 |
| `router.py` | `POST /asr`、`GET /asr/health` |
| `transcriber.py` | WAV 解析 → float32 → 调 shim → 组装结果 |
| `shim.py` | ctypes 封装的 8 个 shim 函数，进程级单例 + 线程锁 |
| `native/asr_shim.cpp` / `.h` | C++ shim 源码（结构体复杂度冻在这里） |
| `native/build_shim.bat` | 重新编译 shim（纯 ASCII，勿加中文注释） |
| `native/make_def.py` | 从 `dumpbin /exports` 生成 .def → .lib |
| `native/sync_runtime.py` | 从 GS 包同步运行资产到 `runtime/` |
| `tests/` | 71 个用例，全部 mock，不需要真模型 |

### 运行资产（不进 git，约 143 MB）

| 路径 | 体积 | 说明 |
| --- | --- | --- |
| `runtime/models/ggml-base.bin` | 147,951,465 B | Whisper base 模型权重 |
| `runtime/whisper.dll` | 478,720 B | whisper.cpp v1.8.3（来自 GS 包） |
| `runtime/ggml.dll` | 56,832 B | ggml 核心 |
| `runtime/ggml-base.dll` | 536,064 B | ggml 基础算子 |
| `runtime/ggml-cpu.dll` | 730,624 B | CPU 后端 |
| `runtime/include/*.h` | 6 个头文件 | 编译 shim 用，不参与运行 |
| `native/bin/asr_shim.dll` | 158,720 B | 本机构建的 shim |

---

## 三、准备运行资产

在联网开发机上执行一次：

```powershell
python voice_asr/native/sync_runtime.py
```

它从 `D:\TRAE_project\GridStar-history-package\` 下按顺序找 GS 包
（优先 `GS_Pack-20261010155413`），复制 4 个 DLL 与模型到 `runtime/`，
并准备 `runtime/include/` 下的 6 个头文件。

头文件有四级回退：`runtime/include` 已存在 → `native/include` → 本机
whisper.cpp 源码树 → 上游 GitHub（需要联网）。模型和 DLL 必须来自 GS 包。

然后编译 shim：

```powershell
voice_asr\native\build_shim.bat
```

脚本自动在若干候选路径里找 `vcvars64.bat`（VS 18 Community 已验证），
依次做 `dumpbin /exports` → 生成 .def → `lib /def` 出 whisper.lib →
`cl /O2 /LD /EHsc /utf-8` 编译，产物落在 `native/bin/asr_shim.dll`。

> 注意：`whisper.dll` 是 CUDA 构建，但包内不含 `ggml-cuda.dll`，运行时会打印
> `whisper_backend_init_gpu: no GPU found` 并自动回落 CPU。这是预期行为，不是错误。

---

## 四、配置

全部通过环境变量，默认值开箱即用。

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `WHISPER_MODEL` | `runtime/models/ggml-base.bin` | 模型权重路径 |
| `WHISPER_DLL` | `runtime/whisper.dll` | whisper.dll 路径 |
| `WHISPER_RUNTIME` | `voice_asr/runtime` | 运行资产总目录 |
| `WHISPER_LANG` | `zh` | 识别语言；填 `auto` 走自动检测 |
| `WHISPER_THREADS` | `min(8, 物理核数)` | 解码线程数 |
| `WHISPER_TIMEOUT` | `120` | 单次转写超时（秒） |
| `ASR_MAX_BYTES` | `15728640`（15 MB） | 单次上传上限 |

就绪判断要**三项齐备**：`native/bin/asr_shim.dll` + `runtime/whisper.dll` +
模型权重。任一缺失，`/asr` 返回 503。

---

## 五、接口

### `GET /asr/health`

探活，**不触发模型加载**，可以频繁轮询。

```json
{
  "ready": true,
  "shim": true,
  "model": true,
  "language": "zh",
  "loaded": false,
  "backend": "1.8.3"
}
```

`ready` 为 `false` 时看 `shim` / `model` 哪一项是 `false` 就知道缺什么。
`loaded` 表示模型是否已在内存里（即是否已发生过至少一次转写）。

### `POST /asr`

`multipart/form-data`，字段名 `audio`，内容为 16kHz / 16bit / 单声道 WAV。

成功（200）：

```json
{"text": "生成一个原住体的网格,边界层设置武层。", "language": "zh", "duration_ms": 5007, "backend_ms": 1287}
```

- `duration_ms` 是**音频本身的时长**（5.01s 录音 → 5007）
- `backend_ms` 是**后端推理耗时**（模型已常驻时的稳态值，实测中文 5s 约 1.3s）

注意别把这两个读反：名字里带 `duration` 的是素材时长，带 `backend` 的才是算力开销。
真实 HTTP 实测（`TestClient` 走完整 app）：

| 场景 | 端到端 | backend_ms |
| --- | --- | --- |
| 5.01s 中文，首次（含模型加载） | 1778 ms | 1754 |
| 5.01s 中文，第二次（模型已常驻） | 1301 ms | 1287 |

失败：

| 状态码 | detail | 原因 |
| --- | --- | --- |
| 400 | 缺少 audio 字段 | 请求里没有 audio |
| 400 | 未录到声音 | 音频为空 |
| 400 | 音频格式无法识别 | 不是合法 WAV |
| 413 | 音频过大 | 超过 `ASR_MAX_BYTES` |
| 503 | 语音服务未就绪 | 组件或模型缺失 |
| 504 | 语音识别超时 | 超过 `WHISPER_TIMEOUT` |
| 500 | 语音识别内部错误 | 其他异常，detail 带 stderr 摘要 |

---

## 六、排障

| 现象 | 原因与处置 |
| --- | --- |
| `/asr` 一直 503 | 看 `/asr/health` 的 `shim`/`model`。/ 资产没同步 → 跑 `sync_runtime.py`；shim 没编译 → 跑 `build_shim.bat` |
| 日志出现 `whisper_backend_init_gpu: no GPU found` | 正常。CUDA 版 DLL 回落 CPU |
| 首次转写卡约 0.3s | 正常，模型加载 |
| 中文出繁体 | base 模型默认输出繁体。shim 内置简体提示词已处理；若误清空提示词会复现 |
| 中文个别字错（圆柱体→原住体、五层→武层） | base 模型的容量上限，不是集成问题。换 `ggml-small.bin` 可改善，代码零改动，只需换 `WHISPER_MODEL` |
| 长静音被识别成无意义文本 | base 模型的幻觉倾向。前端已按最短时长过滤 |
| `voice_asr not mounted` 警告 | `app.py` 里挂载失败，看具体异常；该警告不阻断主应用启动 |
| .bat 构建脚本报 `'CK' is not recognized` | 批处理文件被写入了中文注释。文件必须保持纯 ASCII（cmd 代码页 936 会解析崩坏） |

---

## 七、测试

```powershell
python -m pytest voice_asr -q          # 71 个用例，全 mock，不需要真模型
python voice_asr\smoke_test.py         # 真实链路冒烟：必须要有运行资产
```

`tests/` 里的 71 个用例把 DLL 与模型全部 mock 掉，不需要运行资产，适合 CI。
它验证不了「真模型能不能出正确文本」，那是 `smoke_test.py` 的职责：

```
=== 1. 运行资产检查 ===        ← shim / whisper.dll / 模型 三项在不在
=== 2. 后端版本（触发模型加载）=== ← 真加载模型并转写内置中文素材，打印 RTF
[HTTP 全链路] POST /asr        ← 挂载真实 app 走一遍完整 HTTP
```

内置素材 `tests/fixtures/tts_zh_16k.wav` 是一段 5.01s 的中文录音
（"生成一个圆柱体的网格，边界层设置五层。"，16kHz 单声道 16bit，与浏览器
编码规格一致）。脚本会把 base 模型的已知偏差折算掉再判定，只在**严重跑偏**时
报警，那种情况通常意味着模型与 DLL 不匹配。

用真实部署验证时：

```powershell
python voice_asr\smoke_test.py D:\some\your_16k.wav
python voice_asr\smoke_test.py --backend-only     # 不起 HTTP，只验后端
```

WebUI 侧的契约检查在 `agent/agent/tests/test_webui.py::test_webui_voice_input_contract`，
它锁定前端必须存在 `voice-btn`、`encodeWav(`、`fetch("/asr"` 等符号。
**改了 `webui/app.js` 或 `style.css` 要同步升 `?v=` 版本号并改该测试。**

---

## 八、内网离线部署

`package_intranet.py` 会把源码（走 `git ls-files`）与语音资产（走
`ASR_ASSET_DIRS` 递归复制 `runtime/` 和 `native/bin/`）一起打进 zip。
语音资产约 143 MB，压缩后整体约 130 MB。

资产缺失时打包不会失败，但会打印告警。装完发现语音不能用，先查这一条。

---

## 九、许可

whisper.cpp 与 Whisper 模型权重均为 MIT，可自由商用与再分发。
详见 `THIRD_PARTY_LICENSES`，其中含随包分发的完整清单与
`runtime/include/LICENSE.whisper.cpp` 全文。