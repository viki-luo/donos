/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mo_standard.c — 条款名称表
 *
 * 存在的理由很实际：测试失败时打印 "S-3" 对读者没有意义，
 * 打印 "measured-is-independent" 才让人知道哪一条被违反了。
 */
#include "mo_standard.h"

const char *mo_clause_name(mo_clause_t clause)
{
        switch (clause) {
        case MO_Q1_FIXED_ONLY:            return "Q-1 fixed-point-only";
        case MO_Q2_SI_UNITS:              return "Q-2 SI-units";
        case MO_Q3_MONOTONIC_TIME:        return "Q-3 monotonic-time";
        case MO_Q4_SIGN_IS_DIRECTION:     return "Q-4 sign-is-direction";

        case MO_D1_SET_VELOCITY:          return "D-1 set-velocity";
        case MO_D2_SAMPLE_OVERWRITES:     return "D-2 sample-overwrites";
        case MO_D3_NO_BLOCKING:           return "D-3 no-blocking";
        case MO_D4_NO_TYPE_KNOWLEDGE:     return "D-4 no-type-knowledge";

        case MO_S1_DECLARE_FIRST:         return "S-1 declare-first";
        case MO_S2_REFUSE_NOT_CLAMP:      return "S-2 refuse-not-clamp";
        case MO_S3_MEASURED_IS_INDEPENDENT:
                return "S-3 measured-is-independent";
        case MO_S4_FAILURE_IS_HONEST:     return "S-4 failure-is-honest";
        case MO_S5_NO_UNIT_LEAK:          return "S-5 no-unit-leak";
        case MO_S6_NO_TYPE:               return "S-6 no-type";
        case MO_S7_FIRST_SIGHTING_SCHEDULES:
                return "S-7 first-sighting-schedules";
        case MO_S8_HOT_PLUG_SAFE:         return "S-8 hot-plug-safe";
        }

        return "unknown-clause";
}
