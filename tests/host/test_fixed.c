/* Host-side boundary tests for base/fixed.c. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "fixed.h"

#define Q(x) ((motion_fixed_t)(x) * 4294967296LL)
#define NS_PER_SECOND 1000000000ULL

static unsigned failures;

static void check(const char *name, int ok)
{
        printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
        if (!ok)
                failures++;
}

static void test_add_sub(void)
{
        motion_fixed_t out = 0;

        check("checked add accepts ordinary values",
              fixed_add_checked(Q(2), Q(3), &out) == FIXED_OK && out == Q(5));
        check("checked add rejects positive overflow",
              fixed_add_checked(INT64_MAX, 1, &out) == FIXED_E_OVERFLOW);
        check("checked add rejects negative overflow",
              fixed_add_checked(INT64_MIN, -1, &out) == FIXED_E_OVERFLOW);
        check("checked sub rejects negative overflow",
              fixed_sub_checked(INT64_MIN, 1, &out) == FIXED_E_OVERFLOW);
        check("checked sub rejects positive overflow",
              fixed_sub_checked(INT64_MAX, -1, &out) == FIXED_E_OVERFLOW);
        check("checked multiply-by-two rejects overflow",
              fixed_mul2_checked(INT64_MAX, &out) == FIXED_E_OVERFLOW);
}

static void test_qmul(void)
{
        motion_fixed_t out = 0;

        check("Q32.32 multiply keeps exact integer product",
              fixed_mul_checked(Q(3), Q(4), &out) == FIXED_OK && out == Q(12));
        check("Q32.32 multiply rounds negative values symmetrically",
              fixed_mul_checked(-Q(3), Q(4), &out) == FIXED_OK && out == -Q(12));
        check("Q32.32 multiply rejects result outside int64",
              fixed_mul_checked(INT64_MAX, INT64_MAX, &out) == FIXED_E_OVERFLOW);
        check("Q32.32 multiply rejects null output",
              fixed_mul_checked(Q(1), Q(1), 0) == FIXED_E_INVAL);
}

static void test_velocity_step(void)
{
        motion_fixed_t delta = 0;
        motion_fixed_t carry = 0;
        motion_fixed_t position = 0;

        check("3 m/s over one second no longer overflows",
              fixed_velocity_step_checked(Q(3), NS_PER_SECOND, 0,
                                          &delta, &carry) == FIXED_OK &&
              delta == Q(3) && carry == 0);

        delta = 0;
        carry = 0;
        check("INT64_MAX velocity over one second is representable",
              fixed_velocity_step_checked(INT64_MAX, NS_PER_SECOND, 0,
                                          &delta, &carry) == FIXED_OK &&
              delta == INT64_MAX && carry == 0);

        delta = 0;
        carry = 0;
        check("INT64_MIN velocity over one second is representable",
              fixed_velocity_step_checked(INT64_MIN, NS_PER_SECOND, 0,
                                          &delta, &carry) == FIXED_OK &&
              delta == INT64_MIN && carry == 0);

        carry = 0;
        position = 0;
        for (unsigned i = 0; i < 9; i++) {
                motion_fixed_t step;
                motion_fixed_t next;
                fixed_status_t st = fixed_velocity_step_checked(Q(1), 1000000ULL,
                                                                 carry, &step, &next);
                if (st != FIXED_OK) {
                        failures++;
                        break;
                }
                position += step;
                carry = next;
        }
        check("nine 1 ms steps preserve the carried remainder",
              position == 38654705LL && carry == 664000000LL);

        {
                motion_fixed_t slow_position = 0;
                motion_fixed_t slow_carry = 0;
                for (unsigned i = 0; i < 3; i++) {
                        motion_fixed_t step;
                        motion_fixed_t next;
                        fixed_status_t st = fixed_velocity_step_checked(Q(1), 3000000ULL,
                                                                         slow_carry,
                                                                         &step, &next);
                        if (st != FIXED_OK) {
                                failures++;
                                break;
                        }
                        slow_position += step;
                        slow_carry = next;
                }
                check("1 ms and 3 ms paths remain bit-identical",
                      slow_position == position && slow_carry == carry);
        }

        check("carry lower boundary is rejected",
              fixed_velocity_step_checked(0, 1, -1000000000LL,
                                          &delta, &carry) == FIXED_E_INVAL);
        check("carry upper boundary is rejected",
              fixed_velocity_step_checked(0, 1, 1000000000LL,
                                          &delta, &carry) == FIXED_E_INVAL);
        check("period above one second is rejected",
              fixed_velocity_step_checked(Q(1), NS_PER_SECOND + 1, 0,
                                          &delta, &carry) == FIXED_E_INVAL);
}

int main(void)
{
        puts("=== host fixed-point boundary tests ===");
        test_add_sub();
        test_qmul();
        test_velocity_step();
        printf("  host failures = %u\n", failures);
        return failures == 0 ? 0 : 1;
}
