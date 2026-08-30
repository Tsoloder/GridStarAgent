"""GridStar HTTP 客户端。"""

import logging

import requests

logger = logging.getLogger(__name__)
url = "http://127.0.0.1:1313"
headers = {"Content-Type": "application/json"}

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
        return {"status": "success", "result": response_data.get("result")}
    except requests.exceptions.RequestException as e:
        logger.error("[%s] 请求失败: %s", function_name, e)
        return {"status": "error", "result": f"请求失败: {str(e)}"}
    except ValueError as e:
        logger.error("[%s] 响应解析失败: %s", function_name, e)
        return {"status": "error", "result": f"响应解析失败: {str(e)}"}
