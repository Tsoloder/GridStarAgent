"""client.send_post_request 的响应判定。

契约：status 表示这一层请求是否送到，业务结果放在 result 里（"true"/"false"），
由 base_prompt 与 task_ledger 解读。这里只保证一件事：后端在自己的响应里显式
报了错时，不会被包装成 success 传给模型。
"""
import os
import sys

import pytest

# conftest 只把 agent/agent 加进 sys.path，client.py 在上一层，这里补一下。
AGENT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if AGENT_ROOT not in sys.path:
    sys.path.insert(0, AGENT_ROOT)

import client  # noqa: E402


class _FakeResponse:
    def __init__(self, payload, status_code=200):
        self._payload = payload
        self.status_code = status_code

    def raise_for_status(self):
        if self.status_code >= 400:
            raise client.requests.exceptions.HTTPError("HTTP %d" % self.status_code)

    def json(self):
        if isinstance(self._payload, Exception):
            raise self._payload
        return self._payload


def _stub_post(monkeypatch, payload, status_code=200):
    monkeypatch.setattr(
        client.requests, "post",
        lambda *args, **kwargs: _FakeResponse(payload, status_code),
    )


def test_result_true_is_success(monkeypatch):
    _stub_post(monkeypatch, {"result": "true"})
    assert client.send_post_request("SomeTool", {}) == {"status": "success", "result": "true"}


def test_result_false_keeps_legacy_contract(monkeypatch):
    """result 为 false 是既有契约（base_prompt 2.4 / task_ledger 处理），这层不改写。"""
    _stub_post(monkeypatch, {"result": "false"})
    assert client.send_post_request("SomeTool", {}) == {"status": "success", "result": "false"}


def test_backend_error_without_result_becomes_error(monkeypatch):
    _stub_post(monkeypatch, {"error": "水密处理失败：存在自由边"})
    out = client.send_post_request("DealWatertight", {})
    assert out["status"] == "error"
    assert "水密处理失败" in out["result"]


def test_backend_success_false_becomes_error(monkeypatch):
    _stub_post(monkeypatch, {"result": None, "success": False, "message": "对象不存在"})
    assert client.send_post_request("DeleteBlock", {})["status"] == "error"


def test_explicit_success_not_overridden_by_noise(monkeypatch):
    """result 已给出明确成功值时不因其它字段误判。"""
    _stub_post(monkeypatch, {"result": "true", "success": False})
    assert client.send_post_request("SomeTool", {})["status"] == "success"


def test_http_failure_is_error(monkeypatch):
    _stub_post(monkeypatch, {"result": "true"}, status_code=500)
    assert client.send_post_request("SomeTool", {})["status"] == "error"


def test_non_object_json_is_error(monkeypatch):
    _stub_post(monkeypatch, ["unexpected"])
    assert client.send_post_request("SomeTool", {})["status"] == "error"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))