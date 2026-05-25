# 文档索引

x64dbg AI 插件项目文档全集。按"先读什么"顺序排列。

## 阅读顺序

| # | 文档 | 用途 | 何时读 |
|---|---|---|---|
| 1 | [`architecture.md`](./architecture.md) | 模块分层 / 线程模型 / 数据流 / 存储 schema / 关键设计决策 | 第一次接触项目；想理解"为什么这样组织代码" |
| 2 | [`features.md`](./features.md) | 14 大功能模块 + 子功能详细说明（Provider / RAG / Trace / Agent / 预设 / Reasoning UI 等） | 想知道"项目能做什么 / 怎么用" |
| 3 | [`skills-roadmap.md`](./skills-roadmap.md) | 已实现 + 未实现的工具能力 / 预设清单 / S 阶段与 G 增强项路线图 | 想知道"还有什么没做 / 下一步做什么" |
| 4 | [`development-log.md`](./development-log.md) | 按里程碑（M1-M4.6f / S0-S9 / G-2）流水账记"做了什么" | 想追溯"某个功能是哪一阶段加的" |
| 5 | [`decisions.md`](./decisions.md) | ADR-lite：带 2+ 候选方案权衡的关键决策（为什么不那样做） | 想理解"为什么否决了 X 方案" |
| 6 | [`known-issues.md`](./known-issues.md) | K-01..K-26 已知问题 + N-01..N-06 设计取舍（不是 bug 是不打算做） | 想知道"哪里不要踩 / 已修了什么" |
| 7 | [`review-auto-debug.md`](./review-auto-debug.md) | 自动调试能力一次性评审快照 | 仅供历史参考；不维护 |

## 文档维护规则

- **commit message 前缀**：纯文档改动用 `docs: …`；不打 git tag
- **流水账去哪**：实施细节 → `development-log.md`；带权衡的决策 → `decisions.md`；问题/取舍 → `known-issues.md`
- **数字一致性**：工具总数 / 预设数 / schema 版本 改动时，**同步**修改 `architecture.md` §5.1、`features.md` §11/§12、`skills-roadmap.md` §2.1/§5
- **review-auto-debug.md 不动**：评审快照按设计冻结
- **AGENTS.md / opencode 配置不在 docs/ 下**：项目本身的协作约定在仓库根 `AGENTS.md`

## 快速字段速查

| 想知道 | 去哪查 |
|---|---|
| 当前出厂预设有哪些 | `features.md` §12.出厂预设 / `skills-roadmap.md` §2.1 |
| 工具总数 / 分组 | `features.md` §11.工具清单 |
| 预设 schema 当前版本 | `architecture.md` §5.1 |
| 某 K-xx 是否已修 | `known-issues.md`（标题带 ✅ = 已修复）|
| 某功能为什么不做 sub-agent / 不全量中文化 | `decisions.md` 2026-05-25 段 |
| 日志路径 / spdlog 配置 | `architecture.md` §3 + `known-issues.md` 维护清单 |
| G-2 cache 字段映射表 | `architecture.md` §4.6 + `known-issues.md` K-24 |
