# 更新日志

本文件记录面向用户的重要功能更新。更完整的版本说明和下载文件见 [GitHub Releases](https://github.com/a1474776521/x64dbg_ai_agent/releases)。

## v0.1.1

### 新增

- 反汇编窗口右键增加「翻译选中汇编并写入注释」：使用当前 AI Provider 逐条生成简体中文释义，并写入对应指令的 x64dbg 行内注释。
- 翻译最多处理 128 条指令；写入前校验响应格式、地址顺序、调试目标和指令字节，避免将过期翻译写入其他代码。
- 新增金山云 KSPmas Provider，支持动态获取模型、API Key 设置和 OpenAI 兼容聊天接口。
- Agent 达到迭代上限后，可继续追加最多 50 轮推理。

### 发布

- 提供 x86 和 x64 两种插件：`x64dbg_ai_plugin.dp32`、`x64dbg_ai_plugin.dp64`。
- 同时提供双架构 ZIP 包：`x64dbg-ai-plugin-v0.1.1.zip`。
