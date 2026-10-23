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
├── attach/detach wiring changes
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

---

## 4. The brain

- **Hot-pluggable and seamless**
- Relates to the base on **one matter only**: motion control
- Responsible for **deciding**: still or moving (because that needs a frame, and a frame is information)
- Responsible for **intent and policy**: why move, and how
- Everything else: **direct** to objects

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

---

## 9. Open questions

- [ ] The exact shape of a motion event
- [ ] Scale and update semantics of the wiring table
- [ ] First runnable target (QEMU prototype / real hardware)
- [ ] Naming