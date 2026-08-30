import sys
sys.path.insert(0, r'D:\Code\nnwgridstar_chat\GridStar\bin\mcp\agent')
from context import ContextManager

ctx = ContextManager()

# 模拟 read_skill_resource 的返回（约 12000 字节的 skill 内容）
skill_content = '# 飞行器后缘面处理\n' + '步骤内容' * 5000  # ~60000 bytes, 超过 32KB 阈值
skill_bytes = len(skill_content.encode('utf-8'))
print('skill_content 原始大小: %d bytes' % skill_bytes)

# 模拟 tool result 消息（带 tool_name）
tool_msg_with_name = {
    'role': 'tool',
    'tool_call_id': 'tc_1',
    'content': skill_content,
    'tool_name': 'read_skill_resource'
}

# 模拟普通 tool result 消息（不带 tool_name）
tool_msg_without_name = {
    'role': 'tool',
    'tool_call_id': 'tc_2',
    'content': skill_content,
}

# 构造 20 条消息，把 tool result 放在前面（超过 stale_turns*2=12 的阈值）
padding = [{'role': 'user', 'content': 'padding msg'}] * 20
messages_with_name = [tool_msg_with_name] + padding
messages_without_name = [tool_msg_without_name] + padding

# 测试 1: 带 tool_name 的 skill 内容不被截断
result1 = ctx._layer2_stale_snip(messages_with_name, 6)
assert result1[0]['content'] == skill_content, 'FAIL: skill content was truncated!'
print('测试1 通过: read_skill_resource 的 tool result 不被截断')

# 测试 2: 不带 tool_name 的普通 tool result 被 head+tail 截断
result2 = ctx._layer2_stale_snip(messages_without_name, 6)
assert result2[0]['content'] != skill_content, 'FAIL: normal tool result was not truncated!'
assert '已省略' in result2[0]['content'], 'FAIL: truncation marker missing!'
truncated_bytes = len(result2[0]['content'].encode('utf-8'))
print('测试2 通过: 普通 tool result 被 head+tail 截断, 截断后大小: %d bytes' % truncated_bytes)

# 测试 3: head+tail 截断保留了头尾
assert '飞行器后缘面处理' in result2[0]['content'], 'FAIL: head not preserved!'
print('测试3 通过: head+tail 截断保留了头部')

# 测试 4: _keep_recent_by_token_budget
messages = [{'role': 'user', 'content': 'msg' * 100}] * 100
recent = ctx._keep_recent_by_token_budget(messages, 20000)
print('测试4 通过: 100 条消息中保留了 %d 条 (预算 20000 token)' % len(recent))

# 测试 5: _keep_recent_by_token_budget 空列表
recent_empty = ctx._keep_recent_by_token_budget([], 20000)
assert recent_empty == [], 'FAIL: empty list should return empty'
print('测试5 通过: 空消息列表返回空')

# 测试 6: _truncate_head_tail 短文本不截断
short_text = '短文本'
assert ctx._truncate_head_tail(short_text) == short_text, 'FAIL: short text should not be truncated!'
print('测试6 通过: 短文本不截断')

# 测试 7: _msg_text 提取 assistant tool_calls 时包含工具名
assistant_msg = {
    'role': 'assistant',
    'content': None,
    'tool_calls': [
        {'id': 'tc_1', 'type': 'function', 'function': {'name': 'DeleteDomain', 'arguments': '{"domainIDs": "78"}'}}
    ]
}
msg_text = ctx._msg_text(assistant_msg)
assert 'DeleteDomain' in msg_text, 'FAIL: tool name missing in _msg_text!'
assert 'tool_call' in msg_text, 'FAIL: tool_call marker missing!'
print('测试7 通过: _msg_text 包含工具名: %s' % msg_text[:80])

# 测试 8: _msg_text 提取 tool result 时包含 tool_name
tool_result_msg = {
    'role': 'tool',
    'tool_call_id': 'tc_1',
    'content': '{"success":true,"new_id":245}',
    'tool_name': 'SetConnectorPointCount'
}
result_text = ctx._msg_text(tool_result_msg)
assert 'SetConnectorPointCount' in result_text, 'FAIL: tool_name missing in tool result _msg_text!'
assert 'new_id' in result_text, 'FAIL: tool result content missing!'
print('测试8 通过: _msg_text 包含 tool_name: %s' % result_text[:80])

print('\n全部验证通过!')
