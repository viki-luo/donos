# liteOS Design Document

> Everything is motion. The base manages only motion control — stateless, and any brain can ride it.

**Languages / 语言:** [中文](design.md) · **English**

---

## 0. Positioning

liteOS is not "an operating system" but a **base**: a stateless foundation that does exactly one thing — motion control — and can be ridden by any replaceable brain.

| Layer | Role | Property |
|---|---|---|
| **Brain** | intent, policy, intelligence | hot-pluggable, replaceable at will |
| **Base** | motion control only | invariant and stateless (**"trustworthy" is a goal, not yet guaranteed by isolation**) |
| **Object** | the physical world | plug-and-play, driven by the base |

---

## 1. Ontology: everything is motion

1. Everything is motion
2. Without motion, an object does not exist
3. **Stillness is relative motion**

### Point 3 is the fulcrum of the whole design

Its consequences:

- "Still" is not a *value* of motion — it is the **result of a comparison**
- A comparison needs a **frame of reference**, and a frame is **information**
- The base stores no information ⇒ **deciding "still / moving" is not the base's job**

**Therefore: deciding belongs to the brain; applying belongs to the base.**

---

## 2. The single primitive: apply motion

Taken to its conclusion, point 1 yields:

> **The base has exactly one command: apply motion.**
> There is no "stop" command — "stillness" simply means *motion equal to that of the frame*.

| Intent | What the base actually executes |
|---|---|
| Make it move | `apply_motion(object, motion, frame, duration)` |
| Make it still | `apply_motion(object, synced-with-frame, duration)` |

**Both rows are the same operation; only the frame differs.**

The base **never distinguishes** "moving" from "still". Therefore the base contains no `MOVING` / `STILL` enum — not a single one is needed.

> The state machine disappears; only a function remains.

---

## 3. The base: a stateless motion-control function

```
base = f(self-describing motion event) → apply motion to an object
```

### 3.1 Stateless, therefore events must be self-describing

The base stores no information ⇒ it has no frame of reference available ⇒ every event must carry its own context.

> **Motion events are self-describing**: they carry target, motion content, frame, and duration.

This is the **entire secret** of a workable stateless base: it needs no memory because **every event carries its own world**.

The converse also holds: **the base can be stateless precisely because "stillness" is relative** — the frame can come from the event rather than from the base.

### 3.1.1 Semantics of `duration_ns`: a command freshness window

`duration_ns` used to mean "how long this event stays valid". That reading has two holes:

1. **It cannot be refused.** If a window only makes a command expire *later*, every late command still
   runs — a grasp commanded in a basement two hours ago is still valid when it reaches the table.
2. **It was a dead field in the implementation.** `duration_ns` never took part in any validation, refusal,
   or replacement, and `MOTION_E_DEADLINE` was consequently unreachable. A status code that can never fire is
   not a safety net; it is the illusion of one.

It is now frozen as a **command freshness window**: the event carries its own issue time, and it must be
accepted no later than `issued_at_ns + duration_ns`.

| Model | Semantics | Once the window closes |
|---|---|---|
| Remote control | keep pushing; release and it falls back | object returns to a default state |
| Pick-and-place | place the object onto a trajectory | the object keeps moving along it |
| **Freshness (current)** | **acceptable only inside the window** | **late commands are refused, not executed; an accepted trajectory still runs to completion** |

The pick-and-place model's *self-sustaining* property is kept in full: after acceptance nothing needs to be
fed in again, the object follows its trajectory on its own. What changed is only the "acceptance" step, which
now has a time gate.

| Design decision | How a freshness window satisfies it |
|---|---|
| The base is stateless | deciding needs only "now" plus two numbers in the event; the base stores no time, and the decision lives on the object side |
| Objects carry their own motion capability | closing the window does not disturb a trajectory already accepted |
| The brain is hot-pluggable | commands left behind by a retired brain are refused outright by the new execution clock |
| Stillness is relative | unchanged: "still" is still an event with `motion = 0` |

> **An event has a lifetime; it only takes effect inside that lifetime. Once accepted, the object is still moving.**

The boundaries are written down, one by one:

| Case | Result |
|---|---|
| `duration_ns == 0` | `MOTION_E_INVAL` (a zero-length window is never valid) |
| `issued_at_ns + duration_ns` wraps | `MOTION_E_INVAL` (otherwise it forges a window that never expires) |
| `issued_at_ns > now` | `MOTION_E_INVAL` (an event from the future: the time source is broken) |
| `now == deadline` | accepted (the moment the window closes is still inside it) |
| `now > deadline` | `MOTION_E_DEADLINE` (refused, never executed) |

**"Now" is the submission instant, passed explicitly by the caller:**

```c
motion_status_t base_apply(u64 now_ns, const motion_event_t *ev);
motion_status_t object_submit(wire_t w, u64 now_ns, const motion_event_t *ev);
```

The base does not keep time, so the deadline test can only be made on the object side. That is not a
compromise; it follows directly from being stateless. But the "now" used by that test **must be the instant the
command reached the entry point**, not the execution layer's last scheduled beat: for commands submitted between
two `object_run()` calls, judging against a stale beat lets a long-expired command be accepted indefinitely while
a freshly issued one can be classified as "from the future". Neither the base nor the object stores that instant —
it is just an argument to this call.

### 3.1.2 The shape of a motion event

```c
/* illustrative */
#define MOTION_DOF_MAX 6

typedef struct {
    u64 wire;                    /* target: a wiring handle; 0 is invalid */
    u64 frame;                   /* frame of reference: another wiring; 0 = world */
    u64 issued_at_ns;            /* when the event was issued (caller's clock) */
    u64 duration_ns;             /* freshness window length; 0 is invalid */
    u8  dof;                     /* number of active components, <= 6 */
    s64 motion[MOTION_DOF_MAX];  /* velocity relative to frame, Q32.32 */
} motion_event_t;
```

Six fields map to the parameters of the single primitive — no more, no fewer.

**Crucially, `motion` means "motion relative to `frame`".**

So "still" needs no separate command:

| What you want | Event |
|---|---|
| Absolutely still | `motion = 0, frame = world` |
| Move with object X (relatively still) | `motion = 0, frame = X's wiring` |
| Move at velocity v | `motion = v, frame = world` |

**Stillness = `motion` of zero.** This is where "stillness is relative motion" lands in code.

Three disciplines:

1. **No opaque payload.** An event may not carry any non-motion data — "everything else" must travel the direct object-to-brain channel, never through the base. This is discipline, not omission.
2. **Fixed-point, not floating-point.** The base touches no floats: no FPU state to save, and the WCET stays statically analyzable.
3. **The frame is just another wiring.** The base therefore stores no frame definitions: `frame` is merely a wiring handle, validated on the spot.

> This section covers only the "sent to an object" direction. For the **complete bidirectional contract** (including object → base arrival / departure), see §3.6.

### 3.2 The boundary of statelessness: wiring vs information

| | **Wiring** | **Information** |
|---|---|---|
| What it is | which object is on which port | the content of the world |
| Examples | port → object channel | position, velocity, intent, images |
| Held by the base | **yes** | **no** |
| Analogy | a switchboard knows which line reaches which number | the switchboard does not know what you are saying |

> **Wiring is a precondition of execution; information is the content of the world.**
> The base may hold the former, never the latter.

### 3.3 What the base has

```
├── wiring        object-to-port correspondence (transient plumbing)
├── apply motion  the single primitive
├── arrival/departure  wiring changes (an arrival event is the only source of wiring)
├── execution transients  timers and similar during an event (not world information)
└── isolation     boundaries between objects, and between brain and objects
```

### 3.4 What the base does not have

```
world state        object state tables   query interfaces
still/moving logic frames and context    control laws and policy
logs               file systems          network stacks
driver models      user management       graphics
```

### 3.5 The base is the sole path for motion control

All motion control must pass through the base. This makes the base the sole executor of the world's motion — **while requiring it to store no world information at all**.

### 3.6 The bidirectional interface contract

All data crossing the base boundary, in one place. **There are only three routes, and no fourth.**

| Direction | Interface | Content | Is it state? |
|---|---|---|---|
| object → base | `base_arrive()` | nothing (the arrival itself) | no, an arrival notice |
| object → base | `base_depart(wire_t)` | the handle | no, a departure notice |
| brain → base | `base_apply(motion_event_t)` | see below | no, a command |
| base → object | `object_submit(wire, motion_event_t)` | the same struct | no, a command |

**There is no "query" row.** The base never asks an object anything: no `get_state()`, no `poll()`, no heartbeat.

#### Everything sent to an object

```c
typedef struct {
        wire_t          wire;          /* target wire */
        wire_t          frame;         /* reference frame; 0 = world */
        uint64_t        issued_at_ns;  /* when issued (caller's clock) */
        uint64_t        duration_ns;   /* freshness window length */
        uint8_t         dof;           /* active components, <= 6 */
        motion_fixed_t  motion[6];     /* velocity relative to frame, Q32.32 */
} motion_event_t;
```

What deliberately **does not exist**:

```
command codes      MOVING / STILL enums      a mode field
any distinction between position and velocity (both are motion components)
payload            priority                  callbacks
```

#### Everything received from an object

**Nothing.** `base_arrive()` takes no arguments: the arrival itself is the whole content.

Self-declared properties such as unit and control period are kept by the object and read directly by the brain through `object_declared()`, never through the base (decision 17). About a wire the base knows only one thing: whether it is alive right now.

So an object has exactly **two forms of existence** in the whole system: **alive** (handle valid and generation matching) and **non-existent** (a dead handle). Binary, with no third.

#### Where the object's state lives

In the object itself; the base does not even know it exists:

```
motion[6]   frame   duration_ns   carry[3]   dof   event_count
```

### 3.7 Why "zero states"

If the base were to store "this object is now moving or still", it would also have to hold the **frame of reference** in order to compare — and a frame is **information**. The moment the base stores it, statelessness collapses.

> Stillness is relative motion. **Deciding belongs to the brain; applying belongs to the base.**

So "moving / still" is not a value in the base; it is whether `motion[]` is zero relative to `frame` — **computed each time, never stored**.

### 3.8 Where "objects report their state" goes

Objects do report their state, but **not to the base** — **to the brain** (directly, see §7).

The reference implementation is `object_motion(wire, i)`: a newly attached brain reads the situation directly. That is the interface behind "objects are the memory", and it corresponds to decision 8 and the third point of the ontology.

---

## 4. The brain

- **Hot-pluggable and seamless**
- Relates to the base on **one matter only**: motion control
- Responsible for **deciding**: still or moving (because that needs a frame, and a frame is information)
- Responsible for **intent and policy**: why move, and how
- Everything else: **direct** to objects

### A brain is just an ordinary wire

In the base's eyes there is **no such category as "brain"**. A brain is simply the wire that happens to be driving the base at the moment:

- A brain attaches with `base_arrive`, exactly like any object — **there is no "brain slot"**
- Replacing a brain = the old brain wire `depart`s and the new one `arrive`s — the **same mechanism** as object hot-plug
- The base is **symmetric**: no wire is special, and there is no "only the brain may command" check
- Brains can be stacked on brains arbitrarily; where the stack ends is decided at runtime by the open system, not by the base
- **"Thinking" and "moving" are the same thing to the base**: both are just a `motion` vector on some wire. The difference is only in how the object itself interprets those components — a brain reads them as cognition, a limb as displacement. The base interprets nothing

> **Everything is a motion object; the base is the only thing that is not one.**
> The base is not a thing in the world — it is the world's motion-control function.

### What makes brain replacement seamless

The base is stateless, so **the brain's context is not in the base** — it is in the **objects**.

A new brain connects and reads the situation directly from the objects. This works precisely because "objects connect directly to the brain".

> **Seamlessness of the brain rests on objects being the memory.**

---

## 5. Objects

- Plug-and-play, hot-swappable
- Motion is driven by the base
- All other information connects directly to the brain
- Carry their own motion capability (their own drive and closed loop)
- **Do not define correctness**: they are simply driven, and they report upward

---

## 6. Two kinds of hot-plug: similarities and differences

**There is exactly one similarity**: both are hot-plug — attaching and leaving require no downtime. Everything else differs.

| | Object hot-plug | Brain hot-plug |
|---|---|---|
| Meaning of "seamless" | **the body is not interrupted** | **the mind is not interrupted** |
| Guaranteed by | the base (recognize and take control) | the base (keep running) |
| Where state lives | in the object itself | in the objects (the brain reads it) |
| Consequence of failure | physical accident | **goal:** becomes dumb, not dangerous (no privilege isolation yet, see below) |
| Technical difficulty | physical continuity | cognitive continuity (shared language) |
| Defines correctness? | no | yes |
| What is replaced | the limbs of the world | the values of the world |

> **Object seamlessness is continuity of body; brain seamlessness is continuity of mind.**
> The former rests on the autonomy of objects, the latter on objects as memory.

> **Status correction (see [assessment.en.md](assessment.en.md) §4.2):**
> "becomes dumb, not dangerous" is a **goal**, not a current fact. There is no
> privilege isolation today — on x86_64 every page is writable and executable
> and `CR0.WP=0`; on riscv64 the system runs in Bare mode with `satp=0`. While a
> brain holds the same privilege as the base, a faulty brain can go dumb *and*
> corrupt the base. That sentence becomes true only after execution isolation
> exists.

---

## 7. Channel model: motion is the only narrow waist

```
              brain (hot-pluggable)
              ╱                    ╲
   motion control (sole path,        all other information
   self-describing)                  (direct)
            ╱                          ╲
           ▼                            ▼
    ┌─────────────┐                 ┌────────┐
    │    base     │  apply motion   │ object │
    │  stateless  ├────────────────▶│        │
    └─────────────┘                 └────────┘
```

Just as IP is the narrow waist of the network: everything passes over it, yet applications do not communicate through it.

| Channel | Content | Passes through the base |
|---|---|---|
| Motion control | self-describing motion events | **required** |
| Everything else | sensing, intent, images, data | **never** |

---

## 8. Confirmed design decisions

1. The base is itself a brain, doing motion control and nothing else
2. The base is **stateless**; it stores no information
3. The base holds **wiring** (plumbing), never **information** (content)
4. The base is the **sole path** for motion control
5. Motion events are **self-describing** (carrying frame and duration)
6. The single primitive is **apply motion**; "still" is motion taken from the frame
7. The brain is hot-pluggable and seamless; it relates to the base only on motion control
8. All other information flows **directly** between objects and the brain
9. Deciding still / moving belongs to the brain, not the base
10. `duration_ns` is a **command freshness window**: the event carries its issue time `issued_at_ns` and may be accepted only within `[issued_at_ns, issued_at_ns + duration_ns]`; a late event is refused with `MOTION_E_DEADLINE` and never executed. **After acceptance** the object keeps moving along that event — pick-and-place's self-sustaining property lives on past the window, which constrains only how late a command may arrive
11. Motion events are **fixed-point, payload-free, and self-describing**: `{wire, frame, issued_at_ns, duration_ns, dof, motion[]}`
12. `motion` is **motion relative to `frame`**; "still" is simply `motion = 0`
13. **Everything is a motion object**; a brain is just an ordinary wire with **no "brain slot"**. The base is symmetric, grants no authority and judges nothing; brains can be stacked arbitrarily. **The base is the only non-object** — not a thing in the world, but the world's motion-control function
14. **Wire handles stay stable across slot reuse**: encoded as `(generation << 16) | slot` with a monotonic generation. A departed handle never comes back to life — otherwise, after a hot-plug, an old brain would command whatever new object took the same slot
15. **Objects resolve their own frame (option A)**: an object walks the frame chain and reads each frame object's motion directly — **objects may connect to each other directly**, never through the base. The base only checks that the `frame` wire is valid right now; it cannot see the chain and therefore cannot detect cycles. Cycles and broken chains are detected and rejected on the object side
16. **The execution model lives outside the base**: an execution layer advances each object's motion loop at the `period_ns` the object declared on arrival; time is supplied by the caller (the architecture clock), and the base neither schedules nor keeps time. Objects with no declared period (`period_ns = 0`) are never advanced; a late run does not catch up but reschedules from the current time. Four time rules: **(a) time only moves forward** — a `now_ns` below the last accepted instant returns `OBJECT_RUN_E_CLOCK_ROLLBACK` and advances nothing; **(b) scheduling additions never wrap** — both `now_ns + period_ns` and `next_due_ns + period_ns` are checked and report `OBJECT_RUN_E_TIME_OVERFLOW`; **(c) two-phase commit** — the first pass only computes deadlines and the second writes state, so a refusal can never leave half the table advanced; **(d) the first sighting only schedules** — an object is not integrated the first time the execution layer sees it, because stepping at plug-in would count time that never elapsed. A caller's hardware tick counter is not the execution timeline: read `object_now_ns()` and add the measured delta
17. **About a wire the base knows only whether it is alive**: `base_arrive()` takes no arguments, and each wiring-table slot holds only a generation and an in-use flag. Self-declared properties such as unit and control period are kept by the object and read directly by the brain through `object_declared()`, never through the base
18. **Motion units are frozen**: `motion[0..2]` are linear velocity in m/s and `motion[3..5]` angular velocity in rad/s, all Q32.32. An object integrates velocity into pose over its declared `period_ns`: `position += v * period_ns`. Integration uses **remainder carry**, not re-rounding every step — per-step truncation errs in one constant direction, accumulates linearly with step count, and so effectively lets the period rewrite the velocity. Every fixed-point add, subtract, multiply, rotation, frame-chain accumulation and position update must pass through checked arithmetic; an out-of-range result fails explicitly and never relies on signed C wraparound. Device-native units (degrees, encoder ticks, millimetres) are converted by the object's adapter layer and never enter the base's channel; "the same event produces the same world velocity on objects with different periods" is therefore a verifiable property, not a convention
19. **What is not declared does not enter the model**: `dof` is the authoritative boundary of a command. An event has components only in `motion[0..dof-1]`, and the components outside `dof` are zeroed on acceptance; a capability declaration is effective only on axes `i < cap.dof`, and the rest are zeroed. The reason is the same in both cases: the tail is typically the previous command's leftovers or uninitialised stack content, and the frame chain accumulates all six axes — so a `dof = 1` object could add angular velocity it was never commanded into a child's world motion. **What was not declared is not data, it is noise**, and the model must not read it
20. **To declare is to commit**: a declared capability is validated, and an invalid description is **refused as a whole** (`dof > 6`, `limit_min > limit_max` on an active axis, `max_rate < 0`, `mass < 0`) leaving no partial record; a declared non-zero `max_rate` really does stop an over-limit command on the object side (`MOTION_E_LIMIT`), with `INT64_MIN` handled separately. **To declare and not enforce is a dead field** — which is exactly what `duration_ns` used to be. The travel limits (`limit_min`/`limit_max`) are structurally validated but not yet enforced during integration; they constrain the **measured** pose, so they belong with the separation of commanded, resolved and measured motion
21. **Base purity is statically assertable**: `base/base.c` must not mention `cap`, `limit_min`, `limit_max`, `max_rate`, `mass`, `orientation`, `position`, `described` or `still`, and `make check-purity` asserts exactly that. The moment the base understands those names it starts to understand the world, and "stateless" stops being true — so this invariant is not a slogan but a gate that fails
22. **Command, resolution, and measurement must remain separate**: `cmd_motion` is the object's commanded velocity relative to `frame`; `state.position`, `state.linear`, and `state.angular` are the object's measured/self-held state relative to `frame`; only `object_resolve()` provides the world view. The executor integrates local command into local pose and never writes a world-resolution result back. Full rigid-body twist composition follows `v' = Rv + v_F + omega_F x Rr` and `omega' = Romega + omega_F` at each level, using measured relative position for the coupling term; an external pose write clears carry from the old trajectory. Query names obey the same boundary: `object_commanded_is_zero()` says only that the command relative to the frame is zero, not that the object is still in the world
23. **A pose must really be a rotation**: a non-zero `orientation` is validated on entry to `object_set_state()` against `|q|^2 = 1 +/- 1/1024`, and an invalid pose is refused as a whole, writing no field; all-zero means "no pose declared" and maps to no rotation. This is not pedantry: rotating by `2q` doubles every vector, and the error disguises itself as precision loss, which is far more dangerous than an explicit refusal. **"Not reported yet" is not "reported something wrong"**, and the two must stay distinguishable
24. **A resolve failure must have a cause**: `object_resolve()` returns `object_resolve_status_t`, distinguishing a null pointer, a missing target, a broken frame chain, a cycle, a depth overrun and checked arithmetic overflow. A cycle and a merely long chain need different fixes, yet they used to share one `depth >= MAX` test, so a long chain was reported as a cycle. Note that `OBJECT_RESOLVE_OK` is 0: using the result as a boolean silently makes success false, so callers must compare explicitly
25. **Generation exhaustion = slot retirement, not wraparound**: a handle places a 32-bit generation in bits 16..47, so only `1..0xFFFFFFFF` are representable. One more increment wraps to 0 and is forced back to 1 — the "first-use" handle comes alive and points at a stranger; at 1 kHz hot-plug this happens in about 50 days. The fix is not a wider encoding but **issuing no more handles**: a slot that can no longer mint a distinct handle retires permanently. Retirement is wiring information ("which port still works"), not world information
26. **The moment a wire becomes visible is the moment it is ready**: `object_plug()` fills every field except the wire first and writes the wire and `in_use` last — the wire is the publish point. The old code published the wire before initialising, and a command accepted in that window was judged by the **previous occupant's** frame, period and rate limit; that is not "the hand is not ready", it is "the command entered the wrong frame". `object_unplug()` makes the object invisible before returning the wire, and the order must not be reversed either

---

## 9. Open questions

- [x] First runnable target: x86_64 + QEMU, serial self-test and bus discovery
- [ ] Scale and update semantics of the wiring table
- [x] **Execution model**: settled as decision 16; reference `object_run()` (verified under a synthetic clock)
- [x] Execution layer driven by a real clock: 100 Hz PIT ticks drive `object_run` from the main loop; the interrupt handler only counts ticks
- [x] Runtime hot-plug: the main loop rescans the PCI bus every 100 ms; new devices arrive and vanished ones depart; removal is completed through the QEMU PIIX4 ACPI hotplug registers (0xAE00) (verified by `tools/hotplug-test.sh`)
- [ ] Interrupt-driven hot-plug (parse ACPI tables, handle GPE) instead of polling
- [x] A second processor architecture: riscv64 (QEMU virt + OpenSBI, `make ARCH=riscv64`), with `base/` unchanged and all verification passing
- [x] riscv64 bus discovery: shares `drivers/pci.c` with x86_64; configuration space is accessed through ECAM (0x30000000)
- [x] riscv64 timer interrupts: CLINT is programmed through the SBI TIME extension (EID 0x54494D45) and S-mode counts ticks in the `stvec` trap handler; the main loop waits in `wfi`, matching the semantics of x86_64's PIT
- [ ] riscv64 runtime hot-plug (the virt root bus does not support eject)
- [ ] Landing the upper brain and brain hot-swap
- [ ] Enforce `limit_min` / `limit_max` during integration (currently structural validation only): command, resolution, and measurement are now separated, but the object-side policy for crossing a travel limit (clamp, refuse the whole step, or report a fault) is still open
- [ ] Naming