# GridStarAgent

GridStar 网格生成软件的 AI 智能助手：把「CAD 导入 → 水密处理 → 部件分割 → 表面网格 → 各向异性 → 后缘面处理 → 体网格 → 边界条件 → 质量检查 → 导出」的 CFD 非结构网格工作流，交给大模型驱动的 Agent 自动规划并逐步执行。

项目由四部分组成：**MCP 工具服务**（把 GridStar 的 HTTP 接口包装成标准 MCP 工具）、**Python Agent 后端**（LLM 对话循环 + 会话 + 技能 + 工具审批）、**Web 前端**（浏览器聊天界面）与 **Qt 桌面客户端**（同样界面的 C++ 原生实现，供 GridStar 宿主内嵌）。

---

## 功能特性

- **多 Provider 模型接入**：OpenAI Chat Completions / OpenAI Responses / Anthropic Messages，以及 DeepSeek、Ollama、OpenRouter 等任意 OpenAI 兼容服务；模型以 `provider/model` 标识，独立配置上下文窗口、输出上限与能力（工具、并行工具、推理、视觉、流式用量）。
- **109 个 GridStar 工具，按业务域分组**：默认只暴露只读查询与工程文件管理，其余分组由模型按当前阶段调用 `enable_tool_group` 按需启用，避免每轮携带全量 Schema。
- **技能（Skill）系统**：`res/skills/` 下的 Markdown 技能包按需加载，例如 `cfd-meshing-workflow` 提供完整网格流程路由表，`trailing-edge-processing`、`wing-anisotropy-processing` 提供专项处理步骤；支持 `read_skill` / `read_skill_resource` / `create_skill`。
- **阶段台账与自动模式**：多阶段任务通过内置 `update_plan` 维护全量阶段计划，未创建计划就调用外部工具会被硬拦截；auto 模式下按计划自动推进，manual 模式下每步等待确认。
- **工具审批与人机确认**：有副作用的工具调用可挂起等待用户确认（`/sessions/{id}/tool-approvals/{call_id}`），需要拍板时模型调用 `ask_user_question` 弹出选择卡片。
- **后台任务与断线重连**：Agent 循环运行在独立后台 Task 中，切换会话或刷新页面不中断回复；SSE 断连后按事件序号整体回放，不丢不重；「停止」会真正取消后台任务并补齐被中断的 tool_calls。
- **完整轨迹与会话导出**：每轮请求的系统提示、上下文、请求/响应、工具调用与结果落盘 `trajectory.jsonl`，前端可查看轨迹面板；会话可导出为 Markdown。
- **对话界面**：一轮一张卡片（用户 / 助手 / 工具），思考过程与工具调用就地折叠，卡片底部可查看本轮用量与用时明细；对话区最左边缘的**轮次导航轨**按轮次排点，悬浮显示该轮提问、点击直接定位、当前所在轮次高亮；轨迹视图支持按时长 / 轮次 / 调用分组并用时间轴选区过滤；设置中心分「模型 / 技能 / MCP 工具」三页签；深色 / 银白 / 蔚蓝三套皮肤。
- **附件与多模态**：拖拽上传文本/代码/PDF/Word/Excel/图片（单文件 10 MB，单次最多 6 个附件 / 4 张图片），图片按模型视觉能力送入模型。
- **语音输入**：基于 whisper.cpp 的本地 WAV 转写（`POST /asr`），不上传云端。
- **内网离线分发**：一条命令生成源码 + 离线 wheel + 一键安装脚本的分发包。

---

## 系统架构

```text
┌────────────────────────────┐        ┌──────────────────────────────┐
│  WebUI (webui/)            │        │  Qt 客户端 (QtChartWidget/)  │
│  浏览器聊天界面 /ui/        │        │  Qt 5.12 原生 DLL + demo     │
└─────────────┬──────────────┘        └──────────────┬───────────────┘
              │  HTTP / SSE                          │  推数据 / 收信号
              ▼                                      │
┌─────────────────────────────────────────────────────┴───────────────┐
│  Agent 后端 (agent/agent/)  FastAPI  127.0.0.1:1231                 │
│  agent_loop · llm_client(多 Provider) · session · skill_runtime      │
│  task_ledger · context · document_loader · tool_memory · voice_asr   │
└───────────────────────────────┬─────────────────────────────────────┘
                                │  MCP over SSE (MCP_SERVER_URL)
                                ▼
┌─────────────────────────────────────────────────────────────────────┐
│  MCP 工具服务 (agent/)  FastMCP  SSE 127.0.0.1:5656                  │
│  registry(工具分组) · tools/{project,query,cad,mesh,generation,      │
│  boundary,quality,advanced} · client(HTTP)                          │
└───────────────────────────────┬─────────────────────────────────────┘
                                │  POST http://127.0.0.1:1313
                                ▼
                     GridStar 网格生成软件（本机运行）
```

---

## 目录结构

```text
GridStarAgent/
├── agent/
│   ├── server.py              MCP 服务入口（FastMCP，SSE 5656）
│   ├── registry.py            工具分组注册表（各分组默认是否启用）
│   ├── client.py              GridStar HTTP 客户端（127.0.0.1:1313）
│   ├── tools/                 工具实现：project / query / cad / mesh /
│   │                          generation / boundary / quality / advanced
│   ├── prompt/                内置提示词（CFD 工作流路由表）
│   └── agent/                 Agent 后端
│       ├── app.py             FastAPI 入口（默认端口 1231）
│       ├── agent_loop.py      对话循环：流式、工具执行、审批、计划拦截
│       ├── llm_client/        多 Provider / 多协议 LLM 运行时
│       ├── config.py          模型与 Provider 配置校验、脱敏、落盘
│       ├── paths.py           数据目录（配置 / 会话 / 上传 / 日志 / 导出）解析
│       ├── logging_setup.py   运行日志初始化
│       ├── session.py         会话持久化（messages.jsonl / trajectory.jsonl）
│       ├── skill_runtime.py   技能发现、加载与工具白名单
│       ├── task_ledger.py     阶段计划台账
│       ├── context.py         上下文压缩与大结果外置
│       ├── document_loader.py 附件解析（文本 / PDF / Excel / 图片）
│       ├── tool_memory.py     工具参数默认值的用户确认记忆
│       ├── mcp_bridge.py      MCP 客户端桥接与工具分组管理
│       ├── workflow_runner.py 预定义步骤工作流执行器
│       └── tests/             pytest 套件（含 fixtures 事件序列）
├── webui/                     浏览器端聊天界面（原生 JS，无构建步骤）
├── QtChartWidget/             Qt 5.12 原生 UI 库 + demo + 接线示例
├── res/skills/                Skill 包（CFD 流程、后缘面、各向异性、技能创建）
├── voice_asr/                 语音转写模块（whisper.cpp）
├── docs/                      设计文档
├── package_intranet.py        内网离线分发包打包脚本
└── LICENSE
```

---

## 环境要求

| 组件 | 要求 |
| --- | --- |
| Python | 3.10+（开发环境为 3.13） |
| 依赖 | 见 [agent/agent/requirements.txt](file:///d:/TRAE_project/GridStarAgent/agent/agent/requirements.txt)：fastapi、uvicorn、fastmcp 3.x、openai、httpx、pypdf、openpyxl、requests、pywin32(Windows) |
| GridStar 软件 | 本机运行，HTTP 接口监听 `127.0.0.1:1313`（否则工具调用返回连接失败） |
| Qt 客户端（可选） | Qt 5.12.2 msvc2017_64 + MSVC x64（qmake / nmake） |
| 语音输入（可选） | 本地 whisper.cpp 的 `whisper-cli` 与 ggml 模型 |

---

## 快速开始

### 1. 安装依赖

```bash
python -m venv .venv
.venv\Scripts\activate
pip install -r agent/agent/requirements.txt
```

### 2. 启动 MCP 工具服务

需要 GridStar 软件已在本机运行。

```bash
cd agent
python server.py
```

默认以 SSE 方式监听 `127.0.0.1:5656`；后端可用环境变量 `MCP_SERVER_URL` 指向其他地址，如 `http://127.0.0.1:5656/sse`。

### 3. 启动 Agent 后端

```bash
cd agent/agent
python app.py --host 127.0.0.1 --port 1231
```

打开 <http://127.0.0.1:1231/> 即进入聊天界面（自动跳转到 `/ui/`）。

### 4. 配置模型

首次启动没有可用模型，在界面「设置」中：

1. 新增供应商：填写名称、API 地址（如 `https://api.deepseek.com/v1`）、API Key（也可只填「API Key 环境变量」走环境变量，避免密钥落盘），并选供应商类型：`OpenAI / Compatible / Ollama`、`Anthropic` 或「不支持模型发现」。
2. 「测试连接」→「读取模型」拉取候选列表，勾选需要启用的模型；默认 API 协议按供应商类型预填（`openai-chat` / `openai-responses` / `anthropic-messages`），未显式填写上下文窗口时会按内置元数据自动补齐。
3. 设为默认模型并保存。

配置写入数据目录下的 `config.json`，接口返回与日志中的 API Key 一律脱敏。

### 5. 开始使用

新建会话 → 选择模型与（可选）Skill → 描述目标，例如「导入 D 盘的这个 step 文件，生成一套完整的机翼网格」，Agent 会按流程规划阶段、按需启用工具分组并逐步执行；有副作用的操作会弹出审批卡片，需要决策时会给出选择卡片。

---

## 数据目录

会话、配置、日志、上传文件统一放在用户数据目录，可用环境变量 `CLINELIKECHAT_DATA_DIR` 覆盖：

| 平台 | 默认位置 |
| --- | --- |
| Windows | `%APPDATA%\ClineLikeChat` |
| Linux / macOS | `~/.local/share/ClineLikeChat` |

```text
ClineLikeChat/
├── config.json               模型与 Provider 配置
├── tool_parameter_memory.json 工具参数默认值的确认记忆
├── sessions/<uuid>/          messages.jsonl、trajectory.jsonl、meta
├── skills/                   运行期可写技能目录
├── uploads/                  附件原文件
└── logs/                     运行日志
```

---

## 工具分组

MCP 服务共注册 109 个业务工具 + `GetToolGroups` 发现入口，按域分组：

| 分组 | 默认启用 | 说明 |
| --- | :---: | --- |
| `project` | ✅ | 工程文件：打开/保存/另存 SPD、导入导出 CAD 与网格、清空数据、撤销重做、导出求解器格式 |
| `query` | ✅ | 只读查询：对象 ID、网格线起终点与点数、选中对象、屏幕法向、几何参数(MAC)、默认参数、分部件分组属性 |
| `cad` | | CAD 几何：曲面平移/旋转/缩放/镜像、曲面加工与损伤修复、水密处理、求交、手动/自动提线、Coons 曲面、分部件分组增删改 |
| `mesh` | | 网格拓扑编辑：网格线/面/块删除、平移旋转镜像、拼接装配分裂、端点移动、点数与间距分布 |
| `generation` | | 网格生成：表面网格、体网格/块创建、长窄面网格、机翼各向异性处理 |
| `boundary` | | 边界条件：分组增删、属性配置、保存到计算域 |
| `quality` | | 质量检查：网格线 / 网格面 / 网格块质量评估 |
| `advanced` | | 高级批处理：服务端组合流程、后缘面分类与类型一/类型二批量处理、前缘识别 |

技能可通过 frontmatter 中的 `allowed-tools` 进一步收窄当前会话允许调用的工具范围。

---

## Qt 桌面客户端

[QtChartWidget](file:///d:/TRAE_project/GridStarAgent/QtChartWidget/README.md) 是 `webui/` 的 Qt 5.12 原生复刻，编译为 `QtChartWidget.dll` + 可交互示例 `demo.exe`。库本身**不发任何网络请求**：宿主通过 setter 推数据、通过信号收交互，便于直接嵌入 GridStar 主程序。

```bat
cd QtChartWidget
build.bat          rem 构建 DLL、demo 与接线示例
build.bat shot     rem 构建后离屏截图到 QtChartWidget\shot.png
```

若本机 Qt / VS 路径不同，修改 `build.bat` 顶部的 `QTDIR`、`VCVARS` 即可（该脚本须保持纯 ASCII）。

---

## 语音输入（可选）

`voice_asr` 通过本地 whisper.cpp 把录音 WAV 转成文字，由后端 `include_router` 同源挂载：

- `GET /asr/health`：报告 CLI / 模型是否就绪，未就绪时前端自动禁用语音按钮。
- `POST /asr`：multipart 上传 `audio`，返回转写文本。

配置项均可用环境变量覆盖：`WHISPER_CLI`、`WHISPER_MODEL`、`WHISPER_LANG`（默认 `zh`）、`WHISPER_TIMEOUT`、`ASR_MAX_BYTES`。

---

## 测试

```bash
cd agent/agent
python -m pytest tests -q
```

```bash
# 语音模块（在仓库根目录执行）
python -m pytest voice_asr/tests -q
```

`agent/agent/tests/` 以 mock 的 `ModelRuntime.stream` 与 MCP 调用为基准，用 fixtures 里的预设事件序列校验 Agent 循环的事件流（工具调用、结构化续写、格式重试、auto 模式计划拦截等）。

---

## 内网离线分发

```bash
python package_intranet.py                          # 源码 + 当前平台离线依赖
python package_intranet.py --no-wheels               # 只打包源码
python package_intranet.py --python-version 311 --platform win_amd64
```

产物为 `dist/GridStarAgent_intranet_<日期>.zip`，内含源码、`wheels/`、`requirements.txt` 与 `install_offline.bat`（创建 venv 并以 `--no-index` 离线安装）。

---

## 许可证

[MIT](file:///d:/TRAE_project/GridStarAgent/LICENSE) © 2026 Tsolodancer