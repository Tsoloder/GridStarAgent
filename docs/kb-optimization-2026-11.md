# 知识库优化工单（2026-11-05）

本文件是执行依据。两个执行体按第 6 节分工动工，**不允许越界改对方的文件**。

- 项目根：`D:\TRAE_project\GridStarAgent`
- 索引层：`agent/agent/kb_index.py`
- 工具层：`agent/agent/kb_tools.py`
- 测试：`agent/agent/tests/test_kb_index.py`
- 跑测试用系统 Python（`.venv` 里没有 pytest）：
  `cd D:\TRAE_project\GridStarAgent && python -m pytest agent/agent/tests/test_kb_index.py -q`

## 0. 动工前的基线（已实测，必须保持不变）

```
python -m pytest agent/agent/tests -q
→ 3 failed, 589 passed
```

那 3 个失败是既有问题，与知识库无关，**不许去修**：
- `test_screen_window_fallback.py::test_find_any_falls_back_to_hidden_candidates`
- `test_screen_window_fallback.py::test_capture_restores_hidden_window_before_grabbing`
- `test_webui.py::test_webui_choice_card_contract`

完工判据：`passed` 数只增不减，`failed` 仍是上面这 3 个。

## 1. 真实库现状（用于人工比对，不要改动）

`C:\Users\84198\AppData\Roaming\GridstarAgent.data\kb\kb.db`

2 份 PDF / 299 块 / 120 条参数 / 9.6 万字。
**执行体一律不许对这个真实库做写操作**（index_file / remove_document / sources_gc 实删等）。
需要写数据的验证，一律 `kb_index.configure(tmp_path / "kb.db")` 用临时库。

---

## 2. 任务契约（字段名固定，两个执行体都必须遵守）

### 2.1 `find_params` 返回结构（任务一）

在现有字段基础上新增两个键，前端与工具层只认这两个名字：

```python
{
  "name": str,
  "domain": str, "module": str,
  "default": str,            # 组内第一个非空
  "min": float | None,       # 组内第一个非空
  "max": float | None,
  "unit": str,               # 组内第一个非空
  "description": str,        # 组内最长的一条
  "doc": str,                # 主记录所属文档
  "library_id": str,
  "heading_path": str,       # 主记录章节
  "page": int,               # 主记录页码
  "sources": [               # 新增：所有出处，最多 3 条，按 page 升序
      {"doc": str, "heading_path": str, "page": int},
  ],
  "source_count": int,       # 新增：合并前的记录条数
}
```

### 2.2 检索命中结构（任务二、四）

`search()` 返回的每条 hit 在现有字段上新增：

```python
{
  ...现状字段保留...,
  "coverage": float,       # 新增 0~1，命中 gram 数 / query gram 总数
  "matched_terms": int,    # 新增
  "total_terms": int,      # 新增
  "chapter_kind": str,     # 新增 "case" | "reference" | ""（未判定）
}
```

`score` 保留，但含义改为 2.3 的公式结果。

### 2.3 `_rank` 新公式（任务二）

```python
coverage     = matched_terms / max(1, total_terms)
path_depth   = heading_path.count(" / ") + 1
heading_norm = heading_hit / path_depth          # 按路径段数归一，消除长路径偏置
score = 0.55 * coverage + 0.25 * heading_norm + 0.10 * section_hit + 0.10 * exact
```

- 删掉 min-max 归一化的 `lexical` 项（它就是"每次第一名都 0.59"的根源）。
- bm25 不再进最终分，但**保留作同分 tiebreak**：score 四舍五入到 4 位后相等时，按 `rows` 里的原始顺序（即 bm25 粗排位次）排，保证排序稳定。
- `terms` 仍取 `[t for t in _terms(query) if len(t) >= 2][:24]`，与现有一致。
- **分母要排除整句 token**：`_terms` 会先放一个空格分词得到的整句（中文 query 下就是整句本身），它几乎不可能原样出现在正文里，白占分母会把所有 coverage 系统性压低。
  实现：`body_query = _norm(query)`，分母与分子都跳过 `t == body_query` 的那一项（整句完全相等这个信号已经由 `exact` 项承担）。
  若排除后 `total_terms == 0`，则 `coverage = exact`（退化为只靠整句匹配）。

### 2.4 日志表（任务六）

```sql
CREATE TABLE IF NOT EXISTS kb_query_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL DEFAULT '',
    library_id TEXT NOT NULL DEFAULT '',
    query TEXT NOT NULL DEFAULT '',
    cleaned TEXT NOT NULL DEFAULT '',
    hits INTEGER NOT NULL DEFAULT 0,
    top_coverage REAL,
    top_doc TEXT NOT NULL DEFAULT '',
    top_chunk INTEGER NOT NULL DEFAULT 0,
    source TEXT NOT NULL DEFAULT 'tool'
);
CREATE INDEX IF NOT EXISTS idx_kb_query_log_ts ON kb_query_log(ts);
```

新增函数签名（固定）：
```python
def log_query(query: str, *, cleaned: str = "", hits: int = 0,
              top: Optional[dict] = None, library_id: str = "",
              source: str = "tool") -> None
def query_report(days: int = 30, limit: int = 20) -> dict[str, Any]
def prune_query_log(keep_days: int = 90) -> int
```

`query_report` 返回：
```python
{"days": int, "total": int,
 "empty": [{"query": str, "count": int}],        # 零命中，按次数降序
 "frequent": [{"query": str, "count": int, "avg_hits": float}],
 "low_coverage": [{"query": str, "coverage": float, "doc": str}]}
```

### 2.5 源文件回收（任务五）

```python
def sources_gc(dry_run: bool = True, library_id: Optional[str] = None) -> dict[str, Any]
```
返回：
```python
{"dry_run": bool, "orphans": [{"path": str, "name": str, "size": int}],
 "total_size": int, "deleted": [str], "failed": [{"path": str, "error": str}]}
```

`delete_library(library_id, *, purge_sources: bool = False)` 增加关键字参数，默认 `False` 保持现有行为。

---

## 3. 任务一：参数表去重（`find_params`）

**现状**：去重键是 `"%s@%s" % (name, heading_path)`，同名不同节各留一条。实测 120 条里 29 条重复（增长率 6 条、离散点数 6 条、首层高度 4 条），`limit=3` 时常被同一个参数占满。

**做法**：读出侧合并，存储不动（写入侧不动，重导不需要）。
1. 分组键改为 `name`（不再带 heading_path）。
2. 组内排序：`description` 长度降序 → 有 default 优先 → 有 min/max 优先。
3. 主记录取排序后第一条；`default/min/max/unit` 若为空，从组内取第一个非空补齐。
4. `sources`：组内每条的 `(doc, heading_path, page)` 去重，按 page 升序，最多 3 条。
5. `source_count`：合并前条数。
6. 组间排序沿用现有相关度分（`name in text` → `len(name)*2`；`text in name` → `len(text)`），同分按 name 长度降序。

**验收**：
- `find_params("附面层首层高度")` 返回 1 条「首层高度」，`source_count >= 3`，`sources` 里 3 个不同页码。
- `find_params("增长率")` 返回 1 条，`default == "1.2"`（组内有默认值的那条被补上来）。
- 新增测试：同名跨章节合并、缺字段补齐、sources 不重复、limit 语义不变。

---

## 4. 任务二：分数换成绝对量（`_rank`）

**现状**：`lexical` 是 min-max 归一化，每批候选的第一名恒为 1.0，乘 0.55 后每个 query 首选都落在 0.5 上下。实测「几何导入支持什么格式」第一名 0.594 却是完全无关的「6.0.5 输出网格」。

**做法**：
1. 按 2.3 换公式，去掉 `lexical`。
2. 计算 `matched_terms = sum(1 for t in terms if t in body)`，`total_terms = len(terms)`。
3. `coverage = matched_terms / total_terms`。
4. 保留 `_snippet` 与 `max_per_doc` 逻辑不变（那是另一个议题，不在本次范围）。

**风险提示**：`pool=200` 是 bm25 粗排的截断值，改成 coverage 精排后，理论上存在"coverage 高但 bm25 排不进前 200 被截掉"的情况。当前库 299 块，pool 接近全覆盖，不会触发。**本次不改 pool**，只在函数注释里记一笔。

**验收**：
- 用临时库造两份文档，一份含完整 query 词、一份只含一个词，前者 `coverage` 明显高于后者。
- 真实库副本上「几何导入支持什么格式」的 top1 不再是「输出网格」（人工确认，不作为自动化断言）。
- 新增测试：coverage 与 matched/total 自洽；无关块 coverage 为 0 或极低。

---

## 5. 任务三：降级路径（`_search_rows`）

**现状**：FTS 不可用时走 `c.text LIKE '%整句话%'`，中文长问句必然 0 条，且对外表现为「知识库未收录」。

**做法**：
```python
terms = [t for t in _terms(clean_query(query)) if len(t) >= 2][:8]
if not terms:
    # 退回原来的整句 LIKE
score_expr = " + ".join(["(CASE WHEN c.text LIKE ? THEN 1 ELSE 0 END)"] * len(terms))
sql = (f"SELECT c.id AS id, -(0.0 + {score_expr}) AS rank FROM kb_chunk c"
       f" JOIN kb_doc d ON d.id = c.doc_id"
       f" WHERE d.status IN {_SEARCHABLE} AND ({' OR '.join(['c.text LIKE ?'] * len(terms))})"
       f" {where} ORDER BY rank LIMIT ?")
```
参数顺序：先 score_expr 的 8 个，再 WHERE 的 8 个，再 where 子句参数，最后 LIMIT。
用负的命中词数当 rank，保持「rank 越小越好」的语义一致。

**验收**：临时库里把 `_STORE._fts = False`，`search("附面层首层高度怎么确定")` 必须能返回结果。
新增测试：`monkeypatch` 关掉 `_STORE._fts` 后长问句仍有命中。

**两个实现细节（容易错）**：
1. LIKE 的参数要手工加通配符：`["%" + t + "%" for t in terms]`，**不要**用 `"%%%s%%" % t` 拼接，因为 SQL 里已经有别的 `%` 占位符会冲突。
2. 测试里改 `_STORE._fts` 必须用 `try/finally` 或 fixture 恢复原值，否则会污染后续用例（它是个长命对象）。

---

## 6. 任务四：章节信号（`_rank` + 新增 `_chapter_kind`）

**现状**：`heading_hit` 未按路径长度归一，长路径天然命中更多 gram。真实库实测「设置边界条件怎么操作」top1/top2 都是案例章（12.0.4、10.0.3），功能说明页 7.4.7 排第三。

**做法**：
1. `heading_hit` 改为除以 `path_depth`（见 2.3）。
2. 新增：
```python
_CASE_MODEL_RE = re.compile(r"^[A-Za-z]{1,4}\d")   # M6 / F6 / F16

def _chapter_kind(heading_path: str) -> str:
    """判「这是案例章还是功能参考章」。判不准就返回空串，宁可不标。"""
```
   判定规则：
   - 顶层段（`heading_path` 第一个 ` / ` 之前的部分）含「某」→ `"case"`
   - 顶层段匹配 `_CASE_MODEL_RE`（形如 `M6机翼结构网格`、`F6混合网格`）→ `"case"`
   - 其余 → `""`（不标，不猜测）

3. hit 里填 `chapter_kind`。

**明确不做**：不给案例章降权。功能说明页和案例步骤是两种有用，排序只做长度归一，类型信息交给模型。

**验收**：
- `_chapter_kind("某飞行器非结构网格 / 12.0.1 几何导入")` == `"case"`
- `_chapter_kind("M6机翼结构网格 / 7.0.1 几何导入")` == `"case"`
- `_chapter_kind("网格生成 / 7.4 辅助 / 7.4.7 设置边界")` == `""`
- 新增测试覆盖这三种。

---

## 7. 任务五：源文件回收（`sources_gc` / `delete_library`）

**现状**：`sources` 目录 8 个文件 28.7 MB，只有 2 个被文档引用，6 个是删文档后的孤儿。`delete_library` 的注释明说不动 sources，且没有任何回收入口。

**做法**：
1. 新函数 `sources_gc(dry_run=True, library_id=None)`，按 2.5 返回。
   判定孤儿：`KB_SOURCES_DIR` 下所有普通文件，其 `resolve()` 小写路径不在任何 `kb_doc.path` 的 `resolve()` 小写集合里。
   传了 `library_id` 时只报告该库文档「曾经可能用过」的范围无法精确判断，故**一律按全库引用集合判定**，`library_id` 参数只用于在返回值里附注，不影响判定。
2. `dry_run=True`（默认）绝不删文件。
3. `dry_run=False` 时逐个 `unlink()`，失败进 `failed`，不中断。
4. `delete_library(library_id, *, purge_sources=False)`：为 True 时，在该库文档记录删除后，调用一次 `sources_gc(dry_run=False)`。

**安全要求**：`dry_run=False` 是破坏性操作。代码里必须保证默认值是 `True`，且 `delete_library` 的新参数默认 `False`。

**验收**：临时库里造两个文件、索引一个，`sources_gc()` 报告 1 个孤儿；`dry_run=False` 后文件真的没了、被引用的还在。

---

## 8. 任务六：检索日志

**做法**：
1. `_SCHEMA` 里加 2.4 的表和索引（`CREATE TABLE IF NOT EXISTS`，旧库自动补建）。
2. `log_query` 写一条；失败不抛（日志不能影响检索），用 `try/except sqlite3.Error` 包住并 `return`。
3. `log_query` 里做轻量修剪：每 200 次写或表行数超 20000 时，删掉 90 天前的行。用一个模块级计数器，不要每次 COUNT。
4. `query_report(days, limit)` 按 2.4 返回三组统计：
   - `empty`：`hits = 0` 的 query 按次数降序
   - `frequent`：全部 query 按次数降序，带平均命中数
   - `low_coverage`：`top_coverage < 0.34` 且 `hits > 0` 的，按覆盖率升序
5. `prune_query_log(keep_days=90)` 返回删除行数。
6. `kb_tools.search_tool` 在**返回前**调用 `kb_index.log_query(...)`，传 `cleaned=kb_index.clean_query(query)`、`hits=len(hits)`、`top=hits[0] if hits else None`、`library_id=scope`（`LIBRARY_ALL` 原样传）、`source="tool"`。整段 `try/except Exception: pass`。

**验收**：临时库里搜 3 次（含一次零命中），`query_report()` 的 `total == 3`，`empty` 里有那次零命中。

---

## 9. 工具层展示（`kb_tools.py`）

1. `_fmt_param`：现有输出之后，若 `source_count > 1`，追加一行 `   另见：文档名 章节 P页码；...`（最多 2 条，来自 `sources[1:]`）。
2. `_fmt_hit`：
   - `score=%.2f` 改为 `覆盖 %d/%d 词`（用 `matched_terms` / `total_terms`）
   - `chapter_kind == "case"` 时在 origin 前加 `［案例］`
3. `search_tool` 里写日志（见第 8 节第 6 条）。
4. `KB_SEARCH_TOOL` 的 description 补一句：「命中行会标注［案例］，表示该节是具体算例流程而非功能说明；回答功能性问题时优先用非案例节。」

不得改动的：`_disabled_message` 文案、三个工具名、`is_kb_tool` / `execute` 的分发结构。

---

## 10. 分工与边界

**执行体 A（索引层）**：只允许改 `agent/agent/kb_index.py` 与追加 `agent/agent/tests/test_kb_index.py`。
负责：任务一（第 3 节）、任务二（第 4 节）、任务三（第 5 节）、任务四（第 6 节）、任务五（第 7 节）、任务六的索引层部分（第 8 节 1~5 条）。

**执行体 B（工具层）**：只允许改 `agent/agent/kb_tools.py`。
负责：第 9 节全部 + 第 8 节第 6 条（调用 `log_query`）。
B 按第 2 节的契约写，**不依赖 A 的当前代码**；若 A 尚未落地，B 写完的代码在 A 完成前跑测试会失败，这是预期的，不算缺陷。

**双方共同禁止**：
- 碰 `agent/agent/kb_index.py` 之外的索引逻辑文件
- 改 `_SEARCHABLE`、`DOC_STATUS_*`、`clean_query`、`build_fts_query`、`cjk_ngrams` 的语义
- 对真实库 `C:\Users\84198\AppData\Roaming\GridstarAgent.data\kb\kb.db` 做任何写操作
- 修那 3 个既有失败测试
- 删除或弱化任何现有测试用例
- 用 `except Exception: pass` 吞掉业务错误（日志写入除外）

**B 的额外要求**：读新字段一律用 `hit.get("coverage")` / `hit.get("chapter_kind")` 这类 `.get()`，并给默认值。这样 A 尚未落地时 B 的代码只少显示、不报错。

## 11. 完工交付

每个执行体交回：
1. 改了哪些函数（函数名 + 一句话说明）
2. 新增测试的名字与断言要点
3. `python -m pytest agent/agent/tests/test_kb_index.py -q` 的输出
4. 自己拿不准或有取舍的地方，明确写出来，不要自行决定后沉默

主控验收时会跑全量测试 + 在真实库副本上做改造前后对比，不通过则打回。