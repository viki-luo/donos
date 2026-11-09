/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_wire.c — 接线句柄的世代纪律（宿主侧白盒测试）
 *
 * 为什么要把 base.c 整个包含进来：
 *   要验证的是「世代号耗尽后槽位退役」，而耗尽需要 2^32 次复用。
 *   在 QEMU 里跑四十亿次热插拔不是测试，是等。所以这里直接包含实现，
 *   把接线表推到边界上，再看分配器怎么回答 —— 边界行为与真跑到底
 *   完全一样，因为它读的就是同一个 static 表。
 *
 * 这是白盒测试，不进入目标镜像；它只证明一件事：
 *   一个槽位在无法再造出**新**句柄时退休，绝不会回绕到 1 让旧句柄复活。
 */
#include "../../base/base.c"

#include <stdio.h>

static int failures;

static void check(const char *what, int ok)
{
        printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
                failures++;
}

/*
 * 基座把事件递给物件，而这里关心的是分配器，不是运动语义。
 * 只要符号存在即可。
 */
motion_status_t object_submit(wire_t w, u64 now_ns,
                              const motion_event_t *ev)
{
        (void)w;
        (void)now_ns;
        (void)ev;
        return MOTION_E_NO_OBJECT;
}

int main(void)
{
        printf("\n=== 接线世代纪律 / wiring generation discipline ===\n\n");

        /* ---- 正常递增 ---- */
        base_init();
        wire_t w1 = base_arrive();
        check("the first handle is generation 1, slot 1",
              w1 == WIRE_MAKE(1, 1) && WIRE_INDEX(w1) == 1 && WIRE_GEN(w1) == 1);

        base_depart(w1);
        check("a departed handle stops being valid", !base_wire_valid(w1));

        wire_t w2 = base_arrive();
        check("the same slot is reused with a new generation",
              WIRE_INDEX(w2) == 1 && WIRE_GEN(w2) == 2);
        check("the reused slot does not resurrect the old handle",
              w2 != w1 && !base_wire_valid(w1) && base_wire_valid(w2));

        base_depart(w2);

        /* ---- 世代耗尽：槽位必须退役，而不是回绕 ---- */
        base_init();
        wire_table[0].generation = WIRE_GEN_MAX;

        wire_t w3 = base_arrive();
        check("an exhausted slot retires instead of wrapping to generation 1",
              wire_table[0].retired && !wire_table[0].in_use);
        check("the allocator moves on to the next slot",
              WIRE_INDEX(w3) == 2 && WIRE_GEN(w3) == 1);
        check("the generation-1 handle of the exhausted slot stays dead",
              !base_wire_valid(WIRE_MAKE(1, 1)));

        base_depart(w3);
        wire_t w4 = base_arrive();
        check("a retired slot is never handed out again",
              WIRE_INDEX(w4) != 1 && base_wire_valid(w4));
        check("only the exhausted slot retired",
              wire_table[0].retired && !wire_table[1].retired);
        base_depart(w4);

        /* ---- 每个槽位都耗尽时，分配必须失败而不是伪造句柄 ---- */
        base_init();
        for (u64 i = 0; i < WIRE_MAX; i++)
                wire_table[i].generation = WIRE_GEN_MAX;
        check("a fully exhausted table reports exhaustion, not a forged handle",
              base_arrive() == WIRE_NONE);
        {
                int all_retired = 1;
                for (u64 i = 0; i < WIRE_MAX; i++)
                        if (!wire_table[i].retired)
                                all_retired = 0;
                check("every slot is marked retired", all_retired);
        }

        /* ---- 句柄形状的伪造 ---- */
        base_init();
        wire_t good = base_arrive();
        check("a good handle is valid", base_wire_valid(good));
        check("a high-bit alias of the same handle is refused",
              !base_wire_valid(good | ((wire_t)1 << 48)));
        check("a bumped generation is refused",
              !base_wire_valid(WIRE_MAKE(WIRE_GEN(good) + 1, WIRE_INDEX(good))));
        check("a shifted slot is refused",
              !base_wire_valid(WIRE_MAKE(WIRE_GEN(good), WIRE_INDEX(good) + 1)));
        check("WIRE_NONE is never valid", !base_wire_valid(WIRE_NONE));
        check("an out-of-range slot index is refused",
              !base_wire_valid(WIRE_MAKE(1, WIRE_MAX + 1)));

        /* 离开一个看不见的接线必须是静默无害的 */
        base_depart(WIRE_NONE);
        base_depart(WIRE_MAKE(1234, 5));
        check("departing a handle that was never issued is a no-op",
              base_wire_valid(good));

        /* ---- 表满 ---- */
        base_init();
        u32 got = 0;
        for (u64 i = 0; i < WIRE_MAX; i++)
                if (base_arrive() != WIRE_NONE)
                        got++;
        check("the table hands out exactly WIRE_MAX handles", got == WIRE_MAX);
        check("a full table reports full", base_arrive() == WIRE_NONE);

        printf("\n  host failures = %d\n", failures);
        return failures == 0 ? 0 : 1;
}
