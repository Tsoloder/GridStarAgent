"""GridStar HTTP 客户端。"""

import logging

import requests

logger = logging.getLogger(__name__)
url = "http://127.0.0.1:1313"
headers = {"Content-Type": "application/json"}

def _server_reported_error(payload) -> str:
    """后端是否在自己的响应里显式报了错。

    只认约定明确的失败信号，不去猜字段含义。HTTP 200 只说明请求到达，
    后端把失败写在自己的响应结构里时，这里若放过，就等于把失败报成成功。
    """
    if not isinstance(payload, dict):
        return ""
    for key in ("error", "errorMessage", "error_message", "errMsg"):
        value = payload.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    for key in ("success", "ok"):
        if payload.get(key) is False:
            return "后端返回 %s=false" % key
    if payload.get("failed") is True:
        return "后端返回 failed=true"
    return ""


def _result_is_explicit_success(value) -> bool:
    """result 是否给出了明确的成功值。

    后端把业务结果放在 result 里（"true"/"false"），这是 MCP 层与模型的既有契约，
    result 已经给值时不在这一层做二次判定，交给 base_prompt 的失败处理规则。
    """
    return value is True or (isinstance(value, str) and value.strip().lower() == "true")


def send_post_request(function_name: str, params: dict, timeout=None):
    """发送 POST 请求到指定 URL.

    Args:
        function_name: 函数名称.
        params: 参数字典.
        timeout: 超时时间(秒),默认 10 秒。耗时操作(如部件分割)需适当加大.

    Returns:
        包含状态和结果的字典.如果请求成功,返回 {"status": "success", "result": result_value}；
        如果请求失败,返回 {"status": "error", "result": 错误信息}.
    """
    data = {"function": function_name, "param": params}
    try:
        response = requests.post(url, headers=headers, json=data, timeout=timeout)
        response.raise_for_status()
        response_data = response.json()
        logger.info("[%s] response: %s", function_name, str(response_data)[:500])
        if not isinstance(response_data, dict):
            return {"status": "error", "result": "响应格式异常：期望 JSON 对象，实际为 %s" % type(response_data).__name__}
        result_value = response_data.get("result")
        # result 缺失或为 null、而后端又在自己的响应里报了错时，必须如实转成 error：
        # 早先这里只取 result，把后端自报的失败信号整个丢掉，失败会被当成成功传给模型。
        error_text = _server_reported_error(response_data)
        if error_text and not _result_is_explicit_success(result_value):
            logger.error("[%s] 后端报告失败: %s", function_name, error_text)
            return {"status": "error", "result": error_text}
        return {"status": "success", "result": result_value}
    except requests.exceptions.RequestException as e:
        logger.error("[%s] 请求失败: %s", function_name, e)
        return {"status": "error", "result": f"请求失败: {str(e)}"}
    except ValueError as e:
        logger.error("[%s] 响应解析失败: %s", function_name, e)
        return {"status": "error", "result": f"响应解析失败: {str(e)}"}
