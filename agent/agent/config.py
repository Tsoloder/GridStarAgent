import json
import logging
from dataclasses import dataclass, field, asdict
from typing import Literal, Optional

from paths import CONFIG_PATH

logger = logging.getLogger(__name__)


@dataclass
class ModelEntry:
    provider: str = ""
    model_id: str = ""


@dataclass
class ApiConfig:
    api_type: Literal["openai", "anthropic"] = "openai"
    api_url: str = ""
    api_key: str = ""
    models: list[ModelEntry] = field(default_factory=list)
    default_model_index: int = 0

    def ResolveModelId(self) -> str:
        """返回当前默认选中的模型 ID。"""
        if not self.models:
            return ""
        idx = self.default_model_index
        if idx < 0 or idx >= len(self.models):
            idx = 0
        return self.models[idx].model_id

    def ResolveProvider(self) -> str:
        """返回当前默认选中的供应商名称。"""
        if not self.models:
            return ""
        idx = self.default_model_index
        if idx < 0 or idx >= len(self.models):
            idx = 0
        return self.models[idx].provider

    def ModelById(self, model_id: str) -> Optional[ModelEntry]:
        """按 model_id 查找，不区分大小写。"""
        for m in self.models:
            if m.model_id.lower() == model_id.lower():
                return m
        return None


def model_entry_dict(entry: ModelEntry) -> dict:
    return {"provider": entry.provider, "model_id": entry.model_id}


def api_config_to_dict(cfg: ApiConfig) -> dict:
    return {
        "api_type": cfg.api_type,
        "api_url": cfg.api_url,
        "api_key": cfg.api_key,
        "models": [model_entry_dict(m) for m in cfg.models],
        "default_model_index": cfg.default_model_index,
    }


def load_config() -> Optional[ApiConfig]:
    if CONFIG_PATH.exists():
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            # 兼容旧格式：只有 model_id 字符串，没有 models 列表
            if "models" not in data and "model_id" in data:
                old_id = str(data.get("model_id", "")).strip()
                if old_id:
                    data["models"] = [{"provider": "默认供应商", "model_id": old_id}]
                else:
                    data["models"] = []
                data["default_model_index"] = 0
            models_raw = data.get("models", [])
            models = []
            for m in models_raw:
                if isinstance(m, dict):
                    models.append(ModelEntry(
                        provider=str(m.get("provider", "")),
                        model_id=str(m.get("model_id", "")),
                    ))
            return ApiConfig(
                api_type=str(data.get("api_type", "openai")),
                api_url=str(data.get("api_url", "")),
                api_key=str(data.get("api_key", "")),
                models=models,
                default_model_index=int(data.get("default_model_index", 0)),
            )
        except Exception as e:
            logger.warning(f"load_config failed: {e}")
    return None


def save_config(cfg: ApiConfig):
    from session import atomic_write
    atomic_write(str(CONFIG_PATH), json.dumps(api_config_to_dict(cfg), ensure_ascii=False, indent=2))
    logger.info("config saved")
