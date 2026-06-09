---
name: rtk-safe
description: 使用 RTK 稳妥模式回答与执行命令（手动 rtk 前缀），适合 Windows 原生环境。
allowed-tools: Bash, Read, Glob
---

# RTK Safe Mode

当用户询问“RTK 有什么用”“怎么用 RTK 更稳妥”或要求执行命令时，采用稳妥模式：

1. 不依赖自动 hook 重写。
2. 优先显式使用 `rtk <command>` 执行。
3. 先验证可用性：`rtk --version`。
4. 再执行用户目标命令（如 `rtk git status`、`rtk test <cmd>`）。
5. 返回简洁结果与下一步建议。

## 默认建议模板

- 当前环境推荐：手动 RTK 前缀（稳妥模式）。
- 常用示例：
  - `rtk git status`
  - `rtk git diff`
  - `rtk test npm test`
  - `rtk gain`

## 执行规则

- 若 `rtk --version` 失败，先提示修复 PATH 或重启会话。
- 若命令失败，展示核心报错并给出最短修复建议。
- 不做与请求无关的改动。
