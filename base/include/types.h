/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * types.h — 内核公共短类型名
 *
 * 单独成头文件，是为了让架构层（arch/）也能用同一套名字，
 * 而不必反过来依赖 base.h —— 架构层不应该依赖基座语义。
 */
#ifndef DONOS_TYPES_H
#define DONOS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  s64;

/*
 * 128 位无符号整数只用于中间量：乘法的**结果**比乘数宽，
 * 用 128 位接住是唯一不丢位的办法。
 * 注意：__int128 的**除法**会在两个目标上都调用运行时符号
 * （__udivti3 / __divti3），裸机链接里没有它，所以 `fixed_div_checked`
 * 只用 __int128 做移位、比较与减法，除法自己用移位相减实现。
 */
typedef unsigned __int128 u128;

#endif /* DONOS_TYPES_H */
