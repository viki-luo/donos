# liteOS Design Document

> Everything is motion. The base manages only motion control — stateless, and any brain can ride it.

**Languages / 语言:** [中文](design.md) · **English**

---

## 0. Positioning

liteOS is not "an operating system" but a **base**: a stateless foundation that does exactly one thing — motion control — and can be ridden by any replaceable brain.

| Layer | Role | Property |
|---|---|---|
| **Brain** | intent, policy, intelligence | hot-pluggable, replaceable at will |
| **Base** | motion control only | invariant, trustworthy, stateless |
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

### 3.1.1 Semantics of `duration_ns`: the pick-and-place model

`duration_ns` is the **validity period of the event** (pick-and-place model), not "how long to keep pushing" (remote-control model).

| Model | Semantics | After the event ends |
|---|---|---|
| Remote control | keep pushing; release and it falls back | object returns to a default state |
| **Pick-and-place** | **place the object onto a trajectory** | **the object keeps moving along it** |

**The pick-and-place model is adopted.** It is consistent with every confirmed design decision:

| Design decision | How pick-and-place satisfies it |
|---|---|
| The base is stateless | after the event ends, the base maintains nothing |
| Objects carry their own motion capability | the object continues along its trajectory by itself |
| The brain is hot-pluggable | when the brain disconnects, the world does not freeze; it finishes its events |

> **An event has a lifetime; after that lifetime, the object is still moving.**

### 3.1.2 The shape of a motion event

```c
/* illustrative */
#define MOTION_DOF_MAX 6

typedef struct {
    u64 wire;                    /* target: a wiring handle; 0 is invalid */
    u64 frame;                   /* frame of reference: another wiring; 0 = world */
    u64 duration_ns;             /* validity period (pick-and-place) */
    u8  dof;                     /* number of active components, <= 6 */
    s64 motion[MOTION_DOF_MAX];  /* motion relative to frame, fixed-point */
} motion_event_t;
```

Five fields map to the parameters of the single primitive — no more, no fewer.

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
| object → base | `base_arrive(wire_desc_t)` | `unit`, `period_ns` | no, wiring metadata, once at arrival |
| object → base | `base_depart(wire_t)` | the handle | no, a departure notice |
| brain → base | `base_apply(motion_event_t)` | see below | no, a command |
| base → object | `object_submit(wire, motion_event_t)` | the same struct | no, a command |

**There is no "query" row.** The base never asks an object anything: no `get_state()`, no `poll()`, no heartbeat.

#### Everything sent to an object

```c
typedef struct {
        wire_t          wire;          /* target wire */
        wire_t          frame;         /* reference frame; 0 = world */
        uint64_t        duration_ns;   /* event validity (pick-and-place model) */
        uint8_t         dof;           /* active components, <= 6 */
        motion_fixed_t  motion[6];     /* motion relative to frame, Q32.32 */
} motion_event_t;
```

What deliberately **does not exist**:

```
command codes      MOVING / STILL enums      a mode field
any distinction between position and velocity (both are motion components)
payload            priority                  callbacks
```

#### Everything received from an object

```c
typedef struct {
        wire_t   id;          /* filled in by the base */
        uint32_t unit;        /* unit semantics: 0=none 1=deg 2=mm 3=rad ... */
        uint64_t period_ns;   /* the control period the object reports for itself */
        bool     online;      /* set by the base */
} wire_desc_t;
```

Only `unit` and `period_ns` genuinely come from the object — properties of the wiring, not "where it is now or whether it is moving".

So an object has exactly **two forms of existence** in the whole system: **alive** (handle valid and generation matching) and **non-existent** (a dead handle). Binary, with no third.

#### Where the object's state lives

In the object itself; the base does not even know it exists:

```
motion[6]   frame   duration_ns   dof   event_count
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
| Consequence of failure | physical accident | becomes dumb, not dangerous |
| Technical difficulty | physical continuity | cognitive continuity (shared language) |
| Defines correctness? | no | yes |
| What is replaced | the limbs of the world | the values of the world |

> **Object seamlessness is continuity of body; brain seamlessness is continuity of mind.**
> The former rests on the autonomy of objects, the latter on objects as memory.

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
10. `duration_ns` uses the **pick-and-place model**: the event's validity period; afterwards the object keeps moving along its trajectory
11. Motion events are **fixed-point, payload-free, and self-describing**: `{wire, frame, duration_ns, dof, motion[]}`
12. `motion` is **motion relative to `frame`**; "still" is simply `motion = 0`
13. **Everything is a motion object**; a brain is just an ordinary wire with **no "brain slot"**. The base is symmetric, grants no authority and judges nothing; brains can be stacked arbitrarily. **The base is the only non-object** — not a thing in the world, but the world's motion-control function
14. **Wire handles stay stable across slot reuse**: encoded as `(generation << 16) | slot` with a monotonic generation. A departed handle never comes back to life — otherwise, after a hot-plug, an old brain would command whatever new object took the same slot
15. **Objects resolve their own frame (option A)**: an object walks the frame chain and reads each frame object's motion directly — **objects may connect to each other directly**, never through the base. The base only checks that the `frame` wire is valid right now; it cannot see the chain and therefore cannot detect cycles. Cycles and broken chains are detected and rejected on the object side
16. **The execution model lives outside the base**: an execution layer advances each object's motion loop at the `period_ns` the object declared on arrival; time is supplied by the caller (the architecture clock), and the base neither schedules nor keeps time. Objects with no declared period (`period_ns = 0`) are never advanced; a late run does not catch up but reschedules from the current time

---

## 9. Open questions

- [x] First runnable target: x86_64 + QEMU, serial self-test and bus discovery
- [ ] Scale and update semantics of the wiring table
- [x] **Execution model**: settled as decision 16; reference `object_run()` (verified under a synthetic clock)
- [x] Execution layer driven by a real clock: 100 Hz PIT ticks drive `object_run` from the main loop; the interrupt handler only counts ticks
- [x] Runtime hot-plug: the main loop rescans the PCI bus every 100 ms; new devices arrive and vanished ones depart; removal is completed through the QEMU PIIX4 ACPI hotplug registers (0xAE00) (verified by `tools/hotplug-test.sh`)
- [ ] Interrupt-driven hot-plug (parse ACPI tables, handle GPE) instead of polling
- [x] A second processor architecture: riscv64 (QEMU virt + OpenSBI, `make ARCH=riscv64`), with `base/` unchanged and all verification passing
- [ ] riscv64 timer interrupts and PCI / virtio bus discovery (currently polls the time CSR, no bus discovery)
- [ ] Landing the upper brain and brain hot-swap
- [ ] Naming