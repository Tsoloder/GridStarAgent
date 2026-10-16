"""模型元数据快照：把 models.dev 的上下文窗口、最大输出与能力项打包成本地数据。

添加模型时用来自动填入真实上限，避免所有模型一律回落到 32768/4096 的保守默认值，
导致 context.py 的压缩预算远小于模型实际窗口；同一份数据也用来预勾选视觉 / 视频 /
音频 / 推理这四项能力（数据源没标的项不猜，留给用户自己决定）。

快照里还带一张 host → provider 映射（从各家的 api 字段生成），供 _provider_hint
把用户填的 API 地址落到具体供应商上，不至于回头去跨供应商取最小值。

数据是离线快照，需要更新时执行：

    python -m llm_client.model_metadata build

设置中心的「模型信息」走同一个入口刷新（POST /config/model-metadata/refresh）。

快照缺失或损坏时解析结果为 None，调用方继续使用原有默认值。
"""
import json
import logging
import os
import re
import sys
from dataclasses import dataclass, field
from datetime import date, datetime
from pathlib import Path
from typing import Any, Mapping, Optional
from urllib.parse import urlparse
from urllib.request import Request, urlopen

logger = logging.getLogger(__name__)

SNAPSHOT_PATH = Path(__file__).with_name("model_metadata.json")
SOURCE_URL = "https://models.dev/api.json"

# models.dev 会拒绝默认的 urllib User-Agent（403），必须带上一个常规值。
USER_AGENT = "GridStarAgent/1.0 (+https://models.dev)"

# base_url 主机 → models.dev 的 provider key。
# 供应商 ID 是用户自己起的，只有 API 地址能可靠地指出数据该从哪一家查。
HOST_PROVIDERS = {
    "api.openai.com": "openai",
    "api.anthropic.com": "anthropic",
    "generativelanguage.googleapis.com": "google",
    "aiplatform.googleapis.com": "google-vertex",
    "api.deepseek.com": "deepseek",
    "dashscope.aliyuncs.com": "alibaba-cn",
    "dashscope-intl.aliyuncs.com": "alibaba",
    "api.moonshot.cn": "moonshotai-cn",
    "api.moonshot.ai": "moonshotai",
    "api.x.ai": "xai",
    "openrouter.ai": "openrouter",
    "api.groq.com": "groq",
    "inference.groq.com": "groq",
    "api.siliconflow.cn": "siliconflow-cn",
    "api.siliconflow.net": "siliconflow",
    "open.bigmodel.cn": "zai",
    "api.z.ai": "zai",
    "api.mistral.ai": "mistral",
    "api.cohere.ai": "cohere",
    "api.cohere.com": "cohere",
    "api.perplexity.ai": "perplexity",
    "api.minimax.io": "minimax",
    "api-inference.modelscope.cn": "modelscope",
    "ark.cn-beijing.volces.com": "volcengine",
    "api.cloudflare.com": "cloudflare-workers-ai",
    "api.cerebras.ai": "cerebras",
    "integrate.api.nvidia.com": "nvidia",
    "ollama.com": "ollama-cloud",
    # 下面几家是从 models.dev 各家的 api 字段照抄过来的主机：
    # 同一个云厂商按入口拆成好几个 provider key，地址不同就不能靠剥子域名猜。
    "coding.dashscope.aliyuncs.com": "alibaba-coding-plan-cn",
    "coding-intl.dashscope.aliyuncs.com": "alibaba-coding-plan",
    "token-plan.cn-beijing.maas.aliyuncs.com": "alibaba-token-plan-cn",
    "token-plan.ap-southeast-1.maas.aliyuncs.com": "alibaba-token-plan",
    "api.scnet.cn": "scnet-token-plan",
}

# 能力标志串：固定顺序 视觉(v) / 视频(d) / 音频(a) / 推理(r)，支持的才带那个字母。
# video 用 d 是因为 v 被 vision 占了；顺序固定，缺谁就少谁。
# 界面只摆这四项；tools / parallel_tools / stream_usage 在配置 schema 里保留只为兼容
# 存量 config.json，既不展示也不参与自动填充。
_CAPS_FIELDS = (("v", "vision"), ("d", "video"), ("a", "audio"), ("r", "reasoning"))

_DATE_SUFFIX = re.compile(r"-(?:20\d{2}-\d{2}-\d{2}|20\d{6})$")
_TAG_SUFFIX = re.compile(r":[^:/]+$")
_IPV4 = re.compile(r"^\d{1,3}(?:\.\d{1,3}){3}$")


def _is_public_host(host: str) -> bool:
    """这个主机名能不能拿去认供应商：单标签名（localhost、内网机器名）与裸 IP 认不了。"""
    return bool(host) and "." in host and not _IPV4.match(host)


@dataclass(frozen=True)
class ModelLimits:
    """解析出的模型上限。max_output_tokens 为 None 表示数据源没有给出。

    capabilities 只在数据源确实标了支持的项上有 key（vision / video / audio / reasoning），
    没标的一律不在字典里，调用方不要拿 value 为 False 去反推「不支持」。
    """

    context_window: int
    max_output_tokens: Optional[int]
    provider: str
    capabilities: Optional[Mapping[str, bool]] = field(default=None, compare=False)


@dataclass(frozen=True)
class _Index:
    providers: Mapping[str, Mapping[str, tuple]]
    shared: Mapping[str, tuple]
    hosts: Mapping[str, str]


def _variant_ids(model_id: str) -> list[str]:
    """按可信度从高到低产出候选写法：原样 → 去日期后缀 → 去 tag → 都去掉。

    Anthropic 的 `claude-sonnet-4-5-20250929` 和 Ollama 的 `qwen3:14b` 在数据源里
    通常登记为不带日期、不带 tag 的形式。
    """
    candidates = [model_id]
    no_date = _DATE_SUFFIX.sub("", model_id)
    if no_date != model_id:
        candidates.append(no_date)
    for base in list(candidates):
        no_tag = _TAG_SUFFIX.sub("", base)
        if no_tag != base:
            candidates.append(no_tag)
    seen, ordered = set(), []
    for item in candidates:
        if item and item not in seen:
            seen.add(item)
            ordered.append(item)
    return ordered


def _limits_of(raw: Any) -> Optional[tuple]:
    limit = raw.get("limit") if isinstance(raw, Mapping) else None
    if not isinstance(limit, Mapping):
        return None
    context = limit.get("context")
    if type(context) is not int or context <= 0:
        return None
    output = limit.get("output")
    # 输出上限不得超过上下文窗口，否则 config 校验会直接拒绝保存。
    output = output if type(output) is int and 0 < output <= context else None
    return (context, output)


def _caps_flag(meta: Any) -> str:
    """从 models.dev 的模型对象里抽出能对上我们四项能力的那几位。"""
    if not isinstance(meta, Mapping):
        return ""
    modalities = meta.get("modalities")
    inputs = modalities.get("input") if isinstance(modalities, Mapping) else None
    seen = {str(item).lower() for item in inputs} if isinstance(inputs, (list, tuple)) else set()
    flag = ""
    if "image" in seen:
        flag += "v"
    if "video" in seen:
        flag += "d"
    if "audio" in seen:
        flag += "a"
    if meta.get("reasoning") is True:
        flag += "r"
    return flag


def _caps_from_flag(flag: Any) -> dict[str, bool]:
    """标志串 → 能力字典；认不出的字符直接忽略。"""
    text = flag if isinstance(flag, str) else ""
    return {name: True for letter, name in _CAPS_FIELDS if letter in text}


def _build_index(payload: Mapping[str, Any]) -> _Index:
    providers: dict[str, dict[str, tuple]] = {}
    # 兼容层要投票，先把各家登记的组合攒成票数再定案
    votes: dict[str, dict[tuple, int]] = {}
    caps_votes: dict[str, dict[str, bool]] = {}
    for provider_key, provider in payload.get("providers", {}).items():
        if not isinstance(provider, Mapping):
            continue
        entries: dict[str, tuple] = {}
        for model_id, limits in provider.items():
            if not isinstance(limits, (list, tuple)):
                continue
            context, output = limits[0], limits[1] if len(limits) > 1 else None
            caps = _caps_from_flag(limits[2] if len(limits) > 2 else "")
            key = str(model_id).lower()
            entries[key] = (context, output, caps)
            # 同一个模型 ID 各家登记的值能差好几倍（有家写 256K、有家写 1M），
            # 取最小值会被少数几家拖低，所以改按「(上下文, 输出) 组合」投票，
            # 用登记家数最多的那一组；票数相同时取更小的一组，保守一点。
            pair = (context, output if isinstance(output, int) else None)
            bucket = votes.setdefault(key, {})
            bucket[pair] = bucket.get(pair, 0) + 1
            # 能力取并集：各家登记的是同一个模型 ID 的能力，一家说支持就算支持
            caps_votes.setdefault(key, {}).update(caps)
        if entries:
            providers[str(provider_key)] = entries
    shared: dict[str, tuple] = {}
    for key, bucket in votes.items():
        ranked = sorted(
            bucket.items(),
            key=lambda item: (-item[1], item[0][0], -1 if item[0][1] is None else item[0][1]),
        )
        context, output = ranked[0][0]
        shared[key] = (context, output, dict(caps_votes.get(key, {})))
    raw_hosts = payload.get("hosts")
    hosts = {
        str(host).lower(): str(owner)
        for host, owner in (raw_hosts.items() if isinstance(raw_hosts, Mapping) else ())
    }
    return _Index(providers, shared, hosts)


_INDEX: Optional[_Index] = None
_LOAD_FAILED = False


def _index() -> Optional[_Index]:
    global _INDEX, _LOAD_FAILED
    if _INDEX is not None or _LOAD_FAILED:
        return _INDEX
    try:
        payload = json.loads(SNAPSHOT_PATH.read_text(encoding="utf-8"))
        _INDEX = _build_index(payload)
        logger.info("model metadata loaded: %d providers", len(_INDEX.providers))
    except (OSError, ValueError, TypeError) as exc:
        _LOAD_FAILED = True
        logger.warning("model metadata unavailable (%s), limits fall back to config defaults", exc)
    return _INDEX


def _provider_hint(base_url: str) -> str:
    """从 API 地址推断 models.dev 的 provider key，逐级剥掉子域名匹配。

    先查固定表（写死的常见家，稳），再查快照自带的 host → provider 映射
    （build 时从各家的 api 字段生成，覆盖面跟着数据走），最后才剥一层子域名再试。
    """
    try:
        host = (urlparse(base_url or "").hostname or "").lower()
    except ValueError:
        return ""
    index = _index()
    hosts = index.hosts if index is not None else {}
    while host:
        if host in HOST_PROVIDERS:
            return HOST_PROVIDERS[host]
        # 生成的那张表只认带点的域名：localhost / 内网机器名 / 裸 IP 不能反推供应商
        if "." in host and host in hosts:
            return str(hosts[host])
        _, dot, tail = host.partition(".")
        host = tail if dot else ""
    return ""


def resolve(model_id: str, base_url: str = "") -> Optional[ModelLimits]:
    """按模型 ID 解析真实的上下文窗口与最大输出，查不到返回 None。

    先在 base_url 指向的供应商下找，找不到再跨供应商找；每一级都优先精确匹配，
    再退到去掉日期后缀、去掉 tag 的写法。
    """
    index = _index()
    if index is None:
        return None
    stripped = str(model_id or "").strip()
    if not stripped:
        return None
    hint = _provider_hint(base_url)
    scoped = index.providers.get(hint) if hint else None
    for variant in _variant_ids(stripped):
        key = variant.lower()
        if scoped is not None and key in scoped:
            context, output, caps = scoped[key]
            return ModelLimits(context, output, hint, caps or None)
        if key in index.shared:
            context, output, caps = index.shared[key]
            return ModelLimits(context, output, "", caps or None)
    return None


def build_snapshot(source: str = SOURCE_URL, out_path: Path = SNAPSHOT_PATH) -> int:
    """从 models.dev 拉取数据并写出压缩快照，返回收录的模型数量。"""
    if source.startswith(("http://", "https://")):
        request = Request(source, headers={"User-Agent": USER_AGENT})
        raw = urlopen(request, timeout=120).read()  # noqa: S310
    else:
        raw = Path(source).read_bytes()
    payload = json.loads(raw.decode("utf-8"))
    providers: dict[str, dict[str, list]] = {}
    hosts: dict[str, str] = {}
    for provider_key, provider in payload.items():
        models = provider.get("models") if isinstance(provider, Mapping) else None
        if not isinstance(models, Mapping):
            continue
        # 各家自己的 api 地址就是最可靠的 host → provider 映射，顺手记下来，
        # 省得以后每加一家都要回来改 HOST_PROVIDERS。
        host = (urlparse(str(provider.get("api") or "")).hostname or "").lower()
        if _is_public_host(host):
            hosts.setdefault(host, str(provider_key))
        entries = {}
        for model_id, meta in models.items():
            limits = _limits_of(meta)
            if limits is not None:
                flag = _caps_flag(meta)
                entries[str(model_id)] = [limits[0], limits[1], flag] if flag else [limits[0], limits[1]]
        if entries:
            providers[str(provider_key)] = entries
    snapshot = {
        "source": SOURCE_URL,
        "fetched_at": date.today().isoformat(),
        "hosts": hosts,
        "providers": providers,
    }
    count = sum(len(item) for item in providers.values())
    # 先写临时文件再替换：读端（_index/resolve）可能正好在读，不能让它看到半截文件。
    tmp_path = out_path.with_name(out_path.name + ".tmp")
    tmp_path.write_text(
        json.dumps(snapshot, separators=(",", ":"), ensure_ascii=False, sort_keys=True),
        encoding="utf-8",
    )
    os.replace(tmp_path, out_path)
    return count


def snapshot_info() -> dict:
    """快照自身的元信息：来源、抓取日期、收录规模。

    fetched_at 是快照内部记录的抓取日期；file_time 是文件自身的修改时间，
    留给手工替换过文件的情况兜底。快照缺失或损坏时返回空信息，不抛异常。
    """
    info: dict = {"source": SOURCE_URL, "fetched_at": None, "file_time": None,
                  "providers": 0, "models": 0, "hosts": 0, "available": False}
    try:
        stat = SNAPSHOT_PATH.stat()
    except OSError:
        return info
    info["file_time"] = datetime.fromtimestamp(stat.st_mtime).strftime("%Y-%m-%d %H:%M")
    try:
        payload = json.loads(SNAPSHOT_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return info
    if not isinstance(payload, Mapping):
        return info
    providers = payload.get("providers")
    providers = providers if isinstance(providers, Mapping) else {}
    fetched_at = payload.get("fetched_at")
    hosts = payload.get("hosts")
    info["source"] = str(payload.get("source") or SOURCE_URL)
    info["fetched_at"] = str(fetched_at) if fetched_at else None
    info["providers"] = len(providers)
    info["models"] = sum(len(item) for item in providers.values() if isinstance(item, Mapping))
    info["hosts"] = len(hosts) if isinstance(hosts, Mapping) else 0
    info["available"] = bool(providers)
    return info


def refresh_snapshot() -> dict:
    """重新抓取快照，并让模块内索引失效，返回刷新后的元信息。

    网络与写盘都是阻塞的，调用方必须放到线程里跑，别堵事件循环。
    抓取失败会直接抛出，原快照保持不动。
    """
    global _INDEX, _LOAD_FAILED
    build_snapshot()
    _INDEX, _LOAD_FAILED = None, False
    return snapshot_info()


def _main(argv: list[str]) -> int:
    if not argv or argv[0] not in ("build", "query"):
        print(__doc__)
        return 2
    if argv[0] == "build":
        source = argv[1] if len(argv) > 1 else SOURCE_URL
        count = build_snapshot(source)
        print(f"wrote {SNAPSHOT_PATH} ({count} models)")
        return 0
    model_id = argv[1] if len(argv) > 1 else ""
    base_url = argv[3] if len(argv) > 3 and argv[2] == "--base-url" else ""
    print(resolve(model_id, base_url))
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
