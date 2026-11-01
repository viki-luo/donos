# liteOS Security, Reliability, and Performance Assessment

> Subject: current x86_64 and riscv64 prototype  
> Environment: QEMU, 100 Hz execution tick  
> Verdict: **the current semantic and boundary tests pass and performance headroom is substantial, but production-grade security isolation does not yet exist.**

## 1. Executive summary

The current liteOS prototype demonstrates that its core model is executable: the base tracks wire validity and forwards one motion primitive, while objects retain their own state, resolve reference frames, and are advanced by an external execution layer. x86_64 and riscv64 use the same `base/` implementation and run the same semantic and stress tests.

What the evidence supports:

- Semantic verification reports `failures = 0` on both architectures.
- Performance and stress verification reports `bench failures = 0` on both architectures.
- Generation counters advance through 20,000 plug/unplug cycles and the earliest handle remains dead.
- Capacity exhaustion returns `WIRE_NONE` without damaging existing wires.
- Dead handles and six forged-handle shapes are rejected.
- With every available object slot filled and reference chains up to eight objects deep, one complete execution pass consumes about 1% to 2% of a 100 Hz tick period.
- There is currently no user mode, W^X, enforced NX isolation, or sandbox for an upper brain. The prototype must not be described as production-secure.

## 2. Scope and method

### 2.1 Semantic verification

`base/verify.c` covers:

- The single motion primitive and its error paths.
- A brain and ordinary objects using the same wire model.
- Object self-description and object-held state.
- Frame-chain resolution, broken chains, and cycle rejection.
- Quaternion rotation of linear and angular motion.
- Execution at different declared `period_ns` values.
- A late run stepping once rather than catching up.
- Conformance requirements R1–R3, with R4 and R6 enforced by type boundaries.

### 2.2 Stress verification

`base/bench.c` verifies:

- Fail-closed behavior at wire-table and object-table capacity.
- Existing wires continue working when the table is full.
- Reused slots produce handles with a new generation.
- Departed handles remain dead.
- Six malformed or forged handle shapes never become valid wires.
- Generation advancement over 20,000 plug/unplug cycles.

### 2.3 Performance measurement

Measurements execute inside the target kernel:

- x86_64 uses the TSC.
- riscv64 uses the 10 MHz `time` CSR, with 100 ns resolution.
- Absolute counter values depend on QEMU and the host and are not silicon nanosecond claims.
- `worst run share of a tick` measures numerator and denominator with the same counter. It is the most defensible performance metric in this report.

Reproduce with:

```sh
make ARCH=x86_64
make ARCH=riscv64
tools/bench.sh
```

## 3. Final measurement results

The following values are from one complete dual-architecture run. Absolute virtual-machine counts vary; the ratios are more meaningful.

| Metric | x86_64 | riscv64 |
|---|---:|---:|
| Free object slots entering benchmark | 58 | 62 |
| Accepted `base_apply` | 482.48 TSC counts/call | 1.80 time counts/call |
| Rejected `base_apply` | 101.17 TSC counts/call | 0.96 time counts/call |
| One-object frame resolution | 437.89 TSC counts/call | 1.19 time counts/call |
| Eight-object frame resolution | 6666.44 TSC counts/call | 29.24 time counts/call |
| Worst complete execution pass | 351222 TSC counts | 1410 time counts |
| One 100 Hz tick period | 28108316 TSC counts | 89634 time counts |
| Worst pass share of tick | **1.24%** | **1.57%** |
| Hot-plug churn | 20,000 | 20,000 |
| Benchmark failures | 0 | 0 |

x86_64 has 58 free slots because PCI discovery keeps six devices online. The riscv64 test configuration has 62 free slots because two PCI devices remain online. This is not a leak. The self-test previously retained `brain2`, `hand`, and `foot`; those objects are now released when the test finishes.

### Performance interpretation

- Accepted `base_apply` is more expensive than rejection because the accepted path locates the target object and submits the event. Rejection exits during handle validation.
- Eight-object reference resolution is substantially more expensive than one-object resolution, as expected from per-level quaternion rotation and accumulation.
- Even with every available slot filled and objects placed on the deepest tested chains, one complete pass consumes only about 1% to 2% of a 10 ms tick.
- This establishes strong headroom for the current 100 Hz prototype workload. It does not establish equivalent results on real silicon, at higher control rates, with more objects, under SMP, or under production traffic.

## 4. Security assessment

### 4.1 Verified security properties

| Property | Result | Evidence |
|---|---|---|
| Fail-closed capacity exhaustion | Pass | Additional arrival returns `WIRE_NONE` |
| Existing wires survive a full table | Pass | Every held wire still accepts a motion event |
| Dead handles do not resurrect | Pass | Reuse changes the generation and the old handle stays invalid |
| Forged handles are rejected | Pass | Six malformed and boundary shapes are rejected |
| Frame-cycle detection | Pass on object side | The base cannot see chains; `object_resolve` rejects the cycle |
| Broken frame rejection | Pass | Resolution fails after the frame object departs |
| Benchmark cleanup | Pass | Temporary objects are released after each section |

### 4.2 Confirmed security gaps

#### x86_64

The kernel reports:

```text
cr0 = 0x0000000080000011  (PG=1 WP=0)
cr4 = 0x0000000000000020  (PAE=1)
cr3 = 0x0000000000070000
pd[0] = 0x00000000000000e3  (P=1 RW=1 US=0 NX=0)
pd[1] = 0x0000000000200083  (P=1 RW=1 US=0 NX=0)
```

Paging is active, but `CR0.WP` is clear. Mappings are writable and executable, with no user mapping and no NX isolation. Kernel code, data, and an upper brain have no MMU sandbox boundary.

#### riscv64

The kernel reports:

```text
satp = 0x0000000000000000  (MODE=0 Bare)
```

S-mode performs no address translation and has no page-level permission boundary. The upper brain has the same privilege as the base.

#### Common architectural gaps

- No user-mode process or privilege separation.
- No W^X, NX enforcement, or read-only kernel-data protection.
- No upper-brain sandbox; a faulty or malicious brain can corrupt the base and other objects.
- The base deliberately grants no authority distinctions: a caller holding a valid handle can, in principle, command any live wire.
- Limits and maximum rates declared by objects are not enforced by the base.
- `base/` and `object.c` contain no locks and make no SMP, preemption, or interrupt-context reentrancy guarantee.
- The build uses `-fno-stack-protector` and has no stack guard page.
- Each slot has a 32-bit generation; roughly `2^32` reuses of one slot create a theoretical ABA risk.
- There is no fuzzing, formal proof, static-analysis gate, or hardware fault-injection campaign yet.

The security verdict must therefore be phrased as:

> liteOS has demonstrated sound rejection behavior at handle and capacity boundaries, but it has not established execution isolation, authority isolation, or a memory-safety boundary.

## 5. Reliability assessment

### Demonstrated

- Both architectures execute the same base semantics.
- Semantic and stress suites finish with zero failures.
- A late run does not apply multiple catch-up steps in one call.
- Position remains consistent with actual execution count. For example, motion 3 over 99 steps produces position 297.
- riscv64 programs its timer through SBI TIME, avoiding direct S-mode access to the PMP-protected CLINT.
- Interrupt handlers only count ticks; complex object execution remains in the main loop.
- Temporary self-test and benchmark objects are released.

### An important observed behavior

`tick_count` and object step counts may differ by a small amount. Decision 16 explicitly states that one late `object_run(now_ns)` advances once and reschedules from the current time rather than replaying missed periods. This avoids a burst of stale actions after a delay. The live demo prints both ticks and steps so the behavior remains observable.

### Not yet demonstrated

- Resource stability during long-duration continuous operation.
- Recovery from power loss, device flapping, bus errors, and interrupt storms.
- SMP or multi-hart concurrency safety.
- Driver fault isolation and recovery from an upper-brain crash.
- Multi-level PCI bridges and runtime riscv64 PCIe hot-plug.
- Real-hardware cache, memory-ordering, and timing behavior.

## 6. Is it “extremely fast”?

For the currently defined prototype workload, 100 Hz execution, at most 64 objects, and reference chains up to eight objects deep, the defensible statement is:

> The liteOS motion execution layer has substantial headroom. Its measured worst pass consumes about 1% to 2% of a tick period.

The following statement is not supported:

> liteOS has been proven extremely fast for every processor and production workload.

The current tests run in QEMU with a fixed workload and do not include a real control loop, SMP, cache pressure, network interrupt storms, or silicon measurements.

## 7. Required work before production security

1. **Establish execution isolation**
   - On x86_64, set `CR0.WP` and map code RX, read-only data R, and data/stack RW+NX.
   - On riscv64, enable Sv39 and create page-level privilege boundaries.
   - Run upper brains at lower privilege or in separate address spaces.

2. **Add authority without contaminating the motion ontology**
   - Keep authorization in wiring management or an upper policy layer, not in `motion_event_t`.
   - Use unforgeable capabilities or per-connection allowed-target sets.

3. **Enforce physical limits**
   - Let objects or a safety-monitor brain enforce limits, maximum rate, and acceleration from `object_capability_t`.
   - Keep the base limited to motion and free of world state.

4. **Define concurrency and recovery**
   - Declare the single-core assumption or add locking and memory-order rules for wire and object tables.
   - Add brain-crash, device-departure, interrupt-storm, and repeated-hot-plug tests.

5. **Create verification gates**
   - Build and run both architectures plus `tools/bench.sh` in CI.
   - Add clang static analysis, host-side UBSan tests, fuzzing, and soak testing.

## 8. Final verdict

| Dimension | Verdict |
|---|---|
| Core-model feasibility | **Supported by the current prototype** |
| Cross-architecture consistency | **Pass** |
| Handle and capacity reliability | **Pass under current stress tests** |
| 100 Hz prototype performance | **Substantial headroom** |
| Production memory security | **Fail, isolation not implemented** |
| Isolation of a faulty or malicious brain | **Fail, no sandbox or authority model** |
| SMP and long-duration reliability | **Not yet verified** |

The accurate final statement is:

> liteOS has moved from a concept to an executable, measurable, and reproducible dual-architecture prototype. Its motion-control model, handle boundaries, and performance at the current scale have supporting evidence. Security isolation remains the next major engineering phase, not a property the prototype already possesses.
