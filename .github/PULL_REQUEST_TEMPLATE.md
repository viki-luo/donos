# 拉取请求 / Pull Request

## 变更说明 / Description

<!-- 这个 PR 做了什么，为什么 / What does this PR do and why -->

Closes #

## 变更类型 / Type of change

- [ ] 缺陷修复 / Bug fix
- [ ] 新功能 / New feature
- [ ] 重构 / Refactoring
- [ ] 文档 / Documentation
- [ ] 构建 / Build
- [ ] 测试 / Tests

## 设计律检查 / Design law check

<!-- 所有 PR 必须通过。见 docs/CONTRIBUTING.md / All PRs must pass. See docs/CONTRIBUTING.md -->

- [ ] 基座仍然无状态 / The base remains stateless
- [ ] 未引入「停止」等第二原语 / No second primitive such as "stop" was introduced
- [ ] 静止/运动的判定没有进入基座 / Stillness/motion decisions did not enter the base
- [ ] 其他信息仍与大脑直连 / Other information still connects directly to the brain
- [ ] 未引入「大脑槽」或任何特权接线 / No "brain slot" or privileged wire was introduced
- [ ] 运动事件仍无载荷、无模式字段 / Motion events still carry no payload or mode field

## 测试 / Testing

<!-- 如何验证 / How was this verified -->

- [ ] `make check` 通过（两架构构建 + 自检 + 测量）
  / `make check` passes (both architectures, self-test, measurements)
- [ ] 新增性能结论附有复现命令 / New performance claims ship with a reproduction command
- [ ] 未引入变量除数（无 `__udivdi3`）/ No variable divisor introduced (no `__udivdi3`)
- [ ] 真板验证 / Verified on hardware

## 文档 / Documentation

<!-- 本项目要求中英双语 / This project requires bilingual docs -->

- [ ] 中文文档已更新 / Chinese docs updated
- [ ] 英文文档已更新 / English docs updated
- [ ] CHANGELOG 已更新 / CHANGELOG updated

## 截图 / 日志 / Screenshots or logs

<!-- 如适用 / If applicable -->

## 备注 / Notes for reviewers