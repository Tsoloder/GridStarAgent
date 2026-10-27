"""推理模型的正文输出契约（按模型能力动态注入）。

问题背景：推理型模型在长工具链里会把本该给用户看的结论全部写进
reasoning / thinking，最终正文留空。界面上思考内容是折叠的，用户看到的
就是一条只有「思考过程」、正文空白的气泡，等于这一轮没回答。

为什么不只写在基础提示词里：``base_prompt.md`` 已经三十多 KB，这条要求混在
里面会被稀释；而且模型认为自己在推理里「已经说过了」，不会再往正文写一遍。
参照 OpenHanako ``core/provider-prompt-patches.ts`` 的做法——这类模型侧缺陷
单独成块，每轮请求都贴在系统提示词末尾，靠位置和独立性保证它被读到。

生效条件：模型配置里 ``capabilities.reasoning`` 为真。查不到模型时返回空串
（fail-open）：少贴一段提醒不会破坏流程，多贴也只是多几十个 token。
"""

REASONING_OUTPUT_CONTRACT = "\n".join([
    "<output_contract>",
    "推理内容（reasoning / thinking）只用于内部推理草稿：它在界面上是折叠的，用户默认看不到。",
    "任何需要展示给用户的回答、建议、结论、关键数值、失败原因、下一步动作，"
    "都必须在思考结束后写进最终正文。",
    "不要只输出推理内容就结束一轮回复；正文留空等于这一轮没有回答。",
    "如果使用思考标签，必须先关闭思考标签，再输出最终正文。",
    "</output_contract>",
])


def reasoning_output_contract(config, model_key: str) -> str:
    """当前模型是推理模型时返回契约文本，否则返回空串。"""
    try:
        model = config.model(model_key)
    except Exception:  # noqa: BLE001 - 查不到模型时按「不需要补丁」处理
        return ""
    capabilities = getattr(model, "capabilities", None)
    if not getattr(capabilities, "reasoning", False):
        return ""
    return REASONING_OUTPUT_CONTRACT