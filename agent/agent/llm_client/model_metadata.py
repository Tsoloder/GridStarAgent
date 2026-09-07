"""模型元数据快照：把 models.dev 的上下文窗口与最大输出打包成本地数据。

添加模型时用来自动填入真实上限，避免所有模型一律回落到 32768/4096 的保守默认值，
导致 context.py 的压缩预算远小于模型实际窗口。

数据是离线快照，需要更新时执行：

    python -m llm_client.model_metadata build

快照缺失或损坏时解析结果为 None，调用方继续使用原有默认值。
"""
import json
import logging
import re
import sys
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Any, Mapping, Optional
from urllib.parse import urlparse
from urllib.request import urlopen

logger = logging.getLogger(__name__)

SNAPSHOT_PATH = Path(__file__).with_name("model_metadata.json")
SOURCE_URL = "https://models.dev/api.json"

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
}

_DATE_SUFFIX = re.compile(r"-(?:20\d{2}-\d{2}-\d{2}|20\d{6})$")
_TAG_SUFFIX = re.compile(r":[^:/]+$")


@dataclass(frozen=True)
class ModelLimits:
    """解析出的模型上限。max_output_tokens 为 None 表示数据源没有给出。"""

    context_window: int
    max_output_tokens: Optional[int]
    provider: str


@dataclass(frozen=True)
class _Index:
    providers: Mapping[str, Mapping[str, tuple]]
    shared: Mapping[str, tuple]


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


def _build_index(payload: Mapping[str, Any]) -> _Index:
    providers: dict[str, dict[str, tuple]] = {}
    shared: dict[str, tuple] = {}
    for provider_key, provider in payload.get("providers", {}).items():
        if not isinstance(provider, Mapping):
            continue
        entries: dict[str, tuple] = {}
        for model_id, limits in provider.items():
            if not isinstance(limits, (list, tuple)):
                continue
            context, output = limits[0], limits[1] if len(limits) > 1 else None
            key = str(model_id).lower()
            entries[key] = (context, output)
            # 同一个模型 ID 在多家供应商下登记时取最小值：
            # 估小只会让压缩提前一点，估大会直接换来 400 报错。
            previous = shared.get(key)
            if previous is None:
                shared[key] = (context, output)
            else:
                merged_output = output
                if previous[1] is not None:
                    merged_output = previous[1] if output is None else min(previous[1], output)
                shared[key] = (min(previous[0], context), merged_output)
        if entries:
            providers[str(provider_key)] = entries
    return _Index(providers, shared)


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
    """从 API 地址推断 models.dev 的 provider key，逐级剥掉子域名匹配。"""
    try:
        host = (urlparse(base_url or "").hostname or "").lower()
    except ValueError:
        return ""
    while host and host not in HOST_PROVIDERS:
        _, dot, tail = host.partition(".")
        host = tail if dot else ""
    return HOST_PROVIDERS.get(host, "")


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
            context, output = scoped[key]
            return ModelLimits(context, output, hint)
        if key in index.shared:
            context, output = index.shared[key]
            return ModelLimits(context, output, "")
    return None


def build_snapshot(source: str = SOURCE_URL, out_path: Path = SNAPSHOT_PATH) -> int:
    """从 models.dev 拉取数据并写出压缩快照，返回收录的模型数量。"""
    raw = Path(source).read_bytes() if not source.startswith(("http://", "https://")) else urlopen(source, timeout=120).read()  # noqa: S310
    payload = json.loads(raw.decode("utf-8"))
    providers: dict[str, dict[str, list]] = {}
    for provider_key, provider in payload.items():
        models = provider.get("models") if isinstance(provider, Mapping) else None
        if not isinstance(models, Mapping):
            continue
        entries = {}
        for model_id, meta in models.items():
            limits = _limits_of(meta)
            if limits is not None:
                entries[str(model_id)] = [limits[0], limits[1]]
        if entries:
            providers[str(provider_key)] = entries
    snapshot = {
        "source": SOURCE_URL,
        "fetched_at": date.today().isoformat(),
        "providers": providers,
    }
    count = sum(len(item) for item in providers.values())
    out_path.write_text(
        json.dumps(snapshot, separators=(",", ":"), ensure_ascii=False, sort_keys=True),
        encoding="utf-8",
    )
    return count


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
