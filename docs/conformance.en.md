# Object Conformance Standard

English | [简体中文](conformance.md)

This document defines what a motion object must satisfy to attach to the DONos base.
For the design rationale see [design.en.md](design.en.md), especially the bidirectional interface contract in §3.6–3.8.

---

## 1. Definition

> **An object conforms if and only if it can be driven by motion alone — its entire control surface can be expressed as one motion vector, one frame of reference, and one duration.**

An object that needs a **second kind of command** such as "open", "reset", or "switch mode" does not conform.
It is not necessarily a bad object; accepting it would require a second primitive, which breaks the narrow waist.

---

## 2. Requirements

| # | Requirement | Meaning | How to check | Verification |
|---|---|---|---|---|
| **R1** | Single control surface | Driven only by `motion_event_t`; no second kind of command exists | With all motion set to 0, the object becomes relatively still without errors or other commands | **tested** |
| **R2** | Self-sustaining | Keeps moving by itself once an event is accepted; the base does not keep feeding it | Stop sending after the freshness window closes; the object is still moving (pick-and-place self-sustaining survives acceptance) | **tested** |
| **R3** | Self-declared wiring properties | Declares `unit` and `period_ns` on arrival | The brain can read the declared values directly through `object_declared()`; they are gone after departure | **tested** |
| **R4** | Fixed-point boundary | No floating point crosses the boundary | `motion[]` accepts only Q32.32; floats are allowed inside, not at the boundary | type-enforced |
| **R5** | Referenceable | Can serve as another object's `frame`, and can resolve the `frame` it receives | Resolves motion relative to the world along the frame chain; cycles and broken chains must be rejected | **tested** |
| **R6** | Self-held state, direct to the brain | State never appears in the base; only the brain can see it | The base has no interface that reads object state | type-enforced |
| **R7** | At most 6 degrees of freedom | A single wire carries at most 6 dimensions | `dof > 6` is rejected | **tested** |
| **R8** | Self-describing | Can declare its capability and hold its pose and velocity, via the direct brain channel, never through the base | Readable after declaring; `base.h` has no corresponding interface | **tested** |

| **R10** | Timed motion is not the base's job | "Run for N seconds then stop" is not the base's responsibility: it holds no timer and judges no stillness | No time-accumulation or remaining-time field exists in `base/base.c`; `make check-purity` checks it | Structural |
| **R9** | Device interface conformance | When a real device is attached it must satisfy DONOS-MO-1: commands go to `set_velocity`, the measurement is overwritten by `sample`, and the measurement must not be a copy of the command | `tests/host/conformance_mo.c` runs one device-agnostic suite over two implementations (linear dof=1, angular dof=6) | **Tested** |

R7 is not a limitation: a seven-axis arm does not "fail to conform" — it is "more than one object". It consists of several wires of at most 6 dimensions each, serving as each other's frames.

R8 is a precondition of R5: resolving `frame` requires knowing where both the object and its frame are.
### R10: why timed motion is not in the base

The original requirement said the base should let an object move, and for how
long. Read literally that means "run for N seconds then stop", and that is
**not implemented**, because:

- a base that stops an object after N seconds is **judging stillness**, which
  conflicts with stillness and motion being judged above the base;
- "how much time is left" is information and a timer is state, which conflicts
  with the base storing no information.

It has two correct homes: the **brain** re-issuing the command every 10 ms and
sending zero velocity at the deadline, or the **object** interpreting a duration
field and stopping itself (objects are allowed to hold state). Neither exists
today. `duration_ns` is a **freshness window**: it answers "is this command still
valid", not "make it move for that long".

### How R9 relates to R1-R8

R1-R8 constrain **what an object looks like to the base**: one wire, at most six
dimensions, self-describing, self-sustaining, referenceable. R9 constrains **what sits
behind the object**: whether that is a wheel, a joint or something else, any
implementation satisfying DONOS-MO-1's numeric rules and behavioural clauses can be
recognised and driven, and **the system never asks what it is**.

The standard deliberately has no device-type field. A `type = wheel / joint` field
would force the base to understand those names and turn every new device into an
upstream change, which contradicts "the base does motion control and nothing else":

- **S-2 refuse rather than clamp**: an out-of-limit command is refused outright, never
  quietly clamped to something close.
- **S-3 measured is independent**: `sample` must reflect sensor truth. This is the
  precondition for drift detection, which was identically zero while the measurement
  was a copy of the command.
- **S-4 failure is honest**: a failed read returns false and keeps the last
  measurement; "no data" is never reported as zero velocity.
- **S-5 no unit leak**: wheel radius, gear ratio, encoder counts and joint zero offset
  live in the adapter; the event has no field to put them in.

The full clause list is in `drivers/include/mo_standard.h`; the machine check is
`tests/host/conformance_mo.c`, which prints the part it cannot reach (D-3, no blocking)
instead of dressing a review obligation up as tested.


---

## 3. Capability and state

R8 requires an object to hold two kinds of information, and **neither enters the base**:

| Kind | Content | Change rate | Reference struct |
|---|---|---|---|
| **State** | position 3, orientation quaternion 4, linear velocity 3, angular velocity 3 | every cycle | `object_state_t` |
| **Capability** | degrees of freedom, per-axis limits, per-axis maximum rate, mass | essentially fixed | `object_capability_t` |

Everything is Q32.32 fixed point. Orientation is a unit quaternion `{w, x, y, z}`: no gimbal lock, and stable interpolation.

### Why 3D pose cannot enter the base

If any one of these holds, the base is no longer a stateless function:

1. **Pose is relative** — storing it means storing "relative to what", i.e. a global world model.
2. **Pose changes every cycle** — storing it means someone must keep updating it, i.e. scheduling.
3. **Pose is the basis for deciding stillness** — if the base could decide, it would usurp the brain (decision 9).

### Position does not go into the event

"Move to pose P" is done by the brain: read the object's current pose directly → compute the required rate → send a motion vector.
Putting a target pose into the event would need a field distinguishing "rate or target" — a mode field, which violates no-payload and no-mode.

> **Position is the brain's computed result, not the base's payload.**

### The standard defines the format; the base does not parse it

As with USB: the specification defines the descriptor format, the host controller does not parse it, and parsing is the driver's job.
Here the standard defines the object's self-description format, the base only carries motion events, and parsing is the brain's job.

---

## 4. Not part of the standard

- **Device classes** — the base does not know "this is a motor / this is a camera"
- **Health reports** — whether an object is good or bad is not the base's concern
- **Physical form** — a brain is also a wire, and it has no shape

---

## 5. Open questions

- [x] **Who resolves `frame`**: settled as **option A, the object resolves it** (decision 15). An object walks the frame chain and reads each frame object's motion directly — **objects may connect to each other directly**, never through the base. The base cannot see the chain; cycles and broken chains are detected and rejected on the object side. At each level the reference `object_resolve()` rotates linear velocity, angular velocity, and measured relative position, adds the positional coupling term `omega_F x r`, then adds the frame's own motion; frame-origin offsets are carried upward and an undeclared orientation counts as no rotation. Command, world resolution, and measured state do not overwrite one another (decision 22).
- [x] **`period_ns` is honoured by the execution layer** (decision 16): `object_run(now_ns, &stepped)` advances each object's motion loop at its declared period, and the base still does not schedule. Time may only move forward (a rollback returns `OBJECT_RUN_E_CLOCK_ROLLBACK` and advances nothing), scheduling additions may not wrap (`OBJECT_RUN_E_TIME_OVERFLOW`), commit is two-phase, and the first sighting schedules without integrating. Verified under a synthetic clock; at runtime it is driven by 100 Hz PIT ticks, with the caller reading `object_now_ns()` to stay on that one timeline.
- [x] **Conformance test suite**: R1–R3, R5, R7 and R8 are checked automatically by `base/verify.c`, passing on x86_64 and riscv64; R4 and R6 are enforced by the types.
- [x] **Is a capability declaration binding?** Settled as **yes** (decisions 20 and 31). A description is validated and an invalid one refused as a whole; a declared non-zero `max_rate` really does stop an over-limit command on the object side (`MOTION_E_LIMIT`); a declared travel interval `[limit_min, limit_max]` (not both zero) binds during integration — a crossing refuses the whole step and the object holds at its last in-range position (verified by `tests/host/test_quat.c`). Command, resolution, and measurement stay separated by decision 22.
- [x] **Do fields outside the declared range count as data?** Settled as **no** (decision 19). `motion[]` outside `dof` is zeroed on acceptance and declared axes outside `cap.dof` are zeroed; otherwise tail noise accumulates along the frame chain into world motion.
- [x] **Should resolve failures be distinguished by cause?** Settled as **yes** (decision 24). `object_resolve()` returns a status enum distinguishing a null pointer, a missing target, a broken frame chain, a cycle, a depth overrun and arithmetic overflow. **A cycle and a merely long chain must stay separate**: they shared one `depth >= MAX` test, so a long chain was reported as a cycle, while their fixes are entirely different. `OBJECT_RESOLVE_OK` is 0, so callers must compare explicitly.
- [x] **Must a pose be a unit quaternion?** Settled as **yes** (decision 23). A non-zero pose is validated on entry to `object_set_state()` against `|q|^2 = 1 +/- 1/1024` and an invalid one is refused as a whole; all-zero means "no pose declared" and maps to no rotation — "not reported yet" and "reported something wrong" must stay distinguishable.
