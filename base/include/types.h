/*
 * types.h — 内核公共短类型名
 *
 * 单独成头文件，是为了让架构层（arch/）也能用同一套名字，
 * 而不必反过来依赖 base.h —— 架构层不应该依赖基座语义。
 */
#ifndef LITEOS_TYPES_H
#define LITEOS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  s64;

#endif /* LITEOS_TYPES_H */
