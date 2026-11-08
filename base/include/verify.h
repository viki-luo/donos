/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * verify.h — 基座语义自检
 */
#ifndef LITEOS_VERIFY_H
#define LITEOS_VERIFY_H

/*
 * 跑完所有验证，返回失败条数（0 表示全过）。
 *
 * 两部分：
 *   1. 原语语义 —— 唯一原语、静止即相对运动、无效接线/参照系被拒
 *   2. 统一模型 —— 大脑与手脚都是普通接线，基座无法区分
 */
int base_verify(void);

#endif /* LITEOS_VERIFY_H */
