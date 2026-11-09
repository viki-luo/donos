/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * test_quat.c — host-side attitude integration and travel-limit boundaries.
 *
 * Every expected value is derived by hand from the documented formulas; none
 * is read back from the implementation:
 *
 *   left product : q̇ = ½ ω ⊗ q with ω expressed in frame coordinates
 *                  (the convention object_resolve uses when it rotates
 *                  cmd_motion[3..5] along the frame chain)
 *   normalise    : q' = q · 2^16 / isqrt(Σ qi²·2^32), signs preserved
 *   limits       : a declared [min,max] (not both zero) refuses the whole
 *                  step when the integrated position leaves the interval;
 *                  the step still counts as executed
 */
#include "../../base/base.c"
#include "../../base/fixed.c"
#include "../../base/quat.c"
#include "../../base/object.c"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define Q(v) ((motion_fixed_t)((v) * 4294967296.0))

static unsigned failures;

static void check(const char *what, int ok)
{
        printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
                failures++;
}

static bool near_q(motion_fixed_t got, motion_fixed_t want)
{
        /* 1e-4 absolute: far tighter than the 1/1024 pose tolerance. */
        const double tol = 4294967296.0 / 10000.0;
        double d = (double)got - (double)want;

        return d < tol && d > -tol;
}

/* Advance the execution clock ms milliseconds in 1 ms object_run calls.
 * The first sighting of an object only schedules it (no catch-up step), so
 * warm the scheduler up once before counting. */
static void run_ms(u64 ms)
{
        u32 stepped;
        u64 now = object_now_ns();

        object_run(now + 1000000ULL, &stepped);
        for (u64 k = 0; k < ms; k++) {
                now = object_now_ns();
                if (object_run(now + 1000000ULL, &stepped) != OBJECT_RUN_OK)
                        return;
        }
}

static motion_event_t make_cmd(wire_t w, u8 dof)
{
        motion_event_t ev;

        ev.wire = w;
        ev.frame = FRAME_WORLD;
        ev.issued_at_ns = 0;
        ev.duration_ns = UINT64_MAX;    /* measure mechanics, not freshness */
        ev.dof = dof;
        for (u8 i = 0; i < MOTION_DOF_MAX; i++)
                ev.motion[i] = 0;
        return ev;
}

int main(void)
{
        printf("\n=== quaternion integration and travel limits ===\n");

        base_init();
        object_init();

        /* 1. ω ⊗ identity keeps the pure quaternion exactly. */
        {
                motion_fixed_t q[4] = { Q(1), 0, 0, 0 };
                motion_fixed_t w[3] = { Q(2), 0, 0 };
                motion_fixed_t out[4];

                check("omega (x) identity has zero scalar part",
                      quat_angular_product(q, w, out) && out[0] == 0);
                check("omega (x) identity keeps omega exact",
                      out[1] == Q(2) && out[2] == 0 && out[3] == 0);
        }

        /* 2. ω ⊗ q for q = 90° about z, ω = 1 rad/s about z:
         *    by hand (w=(0,0,0,1), q=(c,0,0,c)): (-c, 0, 0, c).
         *    dq0/dt = -½c (cos shrinks), dq3/dt = +½c (sin grows). */
        {
                motion_fixed_t c = Q(0.70710678);
                motion_fixed_t q[4] = { c, 0, 0, c };
                motion_fixed_t w[3] = { 0, 0, Q(1) };
                motion_fixed_t out[4];

                check("omega (x) q is computable", quat_angular_product(q, w, out));
                check("scalar part equals -cos45", near_q(out[0], -c));
                check("x part is zero", out[1] == 0);
                check("y part is zero", out[2] == 0);
                check("z part equals +cos45", near_q(out[3], c));
        }

        /* 3. Normalisation: exact rescale, signs preserved. */
        {
                motion_fixed_t scaled[4] = { Q(2), 0, 0, 0 };
                motion_fixed_t neg[4] = { -Q(2), 0, 0, 0 };
                motion_fixed_t zero[4] = { 0, 0, 0, 0 };
                motion_fixed_t huge[4] = { (motion_fixed_t)1 << 46, 0, 0, 0 };
                motion_fixed_t diag[4] = { 2 * 3037000500LL, 2 * 3037000500LL, 0, 0 };

                check("normalise rescales 2q to q",
                      quat_normalize(scaled) && scaled[0] == Q(1));
                check("normalise keeps the negative sign",
                      quat_normalize(neg) && neg[0] == -Q(1));
                check("normalise refuses the zero quaternion",
                      !quat_normalize(zero));
                check("normalise refuses absurd magnitudes",
                      !quat_normalize(huge));
                check("normalise rescales 2*(c,c,0,0) to (c,c,0,0)",
                      quat_normalize(diag) && near_q(diag[0], 3037000500LL) &&
                      near_q(diag[1], 3037000500LL));
        }

        /* 4. End to end: 1 rad/s about z for 1 s in 1 ms steps → 1 rad. */
        {
                wire_t spin = object_plug(0, 1000000);
                motion_event_t ev = make_cmd(spin, 6);
                object_state_t st;

                ev.motion[5] = Q(1);
                check("a 6-dof spin command is accepted",
                      base_apply(0, &ev) == MOTION_OK);

                run_ms(1000);

                check("the spinning object stepped 1000 times",
                      object_step_count(spin) == 1000);
                check("rotation leaves its own position at rest",
                      object_state(spin, &st) && st.position[0] == 0 &&
                      st.position[1] == 0 && st.position[2] == 0);

                double q0 = (double)st.orientation[0] / 4294967296.0;
                double q3 = (double)st.orientation[3] / 4294967296.0;
                double norm2 = q0 * q0 + q3 * q3;
                double theta = 2.0 * atan2(q3, q0);

                check("the pose stays a unit quaternion",
                      norm2 > 1.0 - 1.0 / 512.0 && norm2 < 1.0 + 1.0 / 512.0);
                check("1 rad/s for 1 s integrates to about 1 rad about z",
                      theta > 0.98 && theta < 1.02);
                check("x and y components stay zero",
                      st.orientation[1] == 0 && st.orientation[2] == 0);

                object_unplug(spin);
        }

        /* 5. Travel limit: [−1, +0.5] with v = +1 m/s, 1 ms period.
         *    500 steps land exactly on +0.5 (the carry telescopes to zero);
         *    step 501 would reach 0.501 > 0.5 and must be refused whole. */
        {
                wire_t ax = object_plug(0, 1000000);
                object_capability_t cap;
                motion_event_t ev = make_cmd(ax, 1);
                object_state_t st;

                for (u8 i = 0; i < MOTION_DOF_MAX; i++) {
                        cap.limit_min[i] = 0;
                        cap.limit_max[i] = 0;
                        cap.max_rate[i] = 0;
                }
                cap.dof = 1;
                cap.mass = Q(1);
                cap.limit_min[0] = -Q(1);
                cap.limit_max[0] = Q(1) / 2;
                cap.max_rate[0] = Q(2);
                check("the limited axis is declared", object_describe(ax, &cap));

                ev.motion[0] = Q(1);
                check("the 1 m/s command is within the declared rate",
                      base_apply(0, &ev) == MOTION_OK);

                run_ms(1000);

                check("the object holds exactly at the +0.5 boundary",
                      object_state(ax, &st) && st.position[0] == Q(1) / 2);
                check("the other axes never moved",
                      st.position[1] == 0 && st.position[2] == 0);
                check("refused steps still count as executed",
                      object_step_count(ax) == 1000);

                ev.motion[0] = -Q(1);
                check("the reversed command is accepted",
                      base_apply(0, &ev) == MOTION_OK);
                run_ms(1);
                check("moving back inside the interval works again",
                      object_state(ax, &st) && st.position[0] < Q(1) / 2 &&
                      st.position[0] > Q(1) / 2 - Q(1) / 100);
        }

        printf("\n  quat host failures = %u\n", failures);
        return failures ? 1 : 0;
}
