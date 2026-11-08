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
- With every available slot filled and chains up to eight objects deep, both p99.9 and the maximum of a complete pass stay below 10% of a 100 Hz tick (QEMU icount baseline). **E1 + E2**
- There is no user mode, no upper-brain sandbox and no authority model; x86_64 reaches only a partial W^X at 2 MiB granularity while riscv64 reaches page-level W^X. The prototype must not be described as production-secure. **E1** (checked against `mmu failures = 0` and the self-reported lines)

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

**Evidence grades**: every conclusion in this document carries its source, so that an argument is never presented as a measurement.

| Grade | Meaning |
|---|---|
| **E1** | Measured on this machine with a reviewable log (the corresponding `make check` / `tools/bench.sh` line) |
| **E2** | Enforced by an automated gate that turns red when broken (including gates verified with mutants) |
| **E3** | Argument or code-structure statement only; not measured |

The following values are from one complete dual-architecture run. Absolute virtual-machine counts vary; the ratios are more meaningful.

| Metric | x86_64 | riscv64 |
|---|---:|---:|
| Free object slots entering benchmark | 58 | 62 |
| Accepted `base_apply` | 137.00 TSC counts/call | 1.01 time counts/call |
| Rejected `base_apply` | 25.00 TSC counts/call | 0.24 time counts/call |
| One-object frame resolution | 101.00 TSC counts/call | 0.86 time counts/call |
| Eight-object frame resolution | 15,924.09 TSC counts/call | 133.78 time counts/call |
| One 100 Hz tick period | 8,367,199 TSC counts | 90,560 time counts |
| Full-table pass p99.9 share of tick | **8.95%** | **7.66%** |
| Full-table pass maximum share of tick | **8.95%** | **7.66%** |
| Hot-plug churn | 20,000 | 20,000 |
| Benchmark failures | 0 | 0 |

All of the above are **E1 + E2** (`make bench-latency` and `make check` check them).

x86_64 has 58 free slots because PCI discovery keeps six devices online. The riscv64 test configuration has 62 free slots because two PCI devices remain online. This is not a leak. The self-test previously retained `brain2`, `hand`, and `foot`; those objects are now released when the test finishes.

### Performance interpretation

- Accepted `base_apply` is more expensive than rejection because the accepted path locates the target object and submits the event. Rejection exits during handle validation. **E1**
- Eight-object reference resolution is substantially more expensive than one-object resolution, as expected from per-level quaternion rotation and accumulation. **E1**
- Even with every available slot filled and objects placed on the deepest tested chains, the p99.9 and maximum of one complete pass stay below 10% of a 10 ms tick. **E1 + E2**
- Those numbers come from a deterministic QEMU `-icount` baseline; they are **not** chip nanoseconds or a WCET proof. Real hardware must re-run the same quantiles. **E3 (not done)**


## 4. Security assessment

### 4.1 Verified security properties

| Property | Result | Evidence |
|---|---|---|---|
| Fail-closed capacity exhaustion | Pass | Additional arrival returns `WIRE_NONE` | E1 + E2 |
| Existing wires survive a full table | Pass | Every held wire still accepts a motion event | E1 + E2 |
| Dead handles do not resurrect | Pass | Reuse changes the generation and the old handle stays invalid | E1 + E2 (incl. generation white-box test) |
| Forged handles are rejected | Pass | Six malformed and boundary shapes are rejected | E1 + E2 |
| Frame-cycle detection | Pass on object side | The base cannot see chains; `object_resolve` rejects the cycle | E1 + E2 |
| Broken frame rejection | Pass | Resolution fails after the frame object departs | E1 + E2 |
| Benchmark cleanup | Pass | Temporary objects are released after each section | E1 + E2 |
| Write to a read-only page is refused by hardware | Pass | x86_64 #PF error=0x03; riscv64 scause=15, with exact fault address and faulting PC | E1 + E2 |
| Instruction fetch from a non-executable page is refused | Pass | x86_64 #PF error=0x11; riscv64 scause=12 | E1 + E2 |
| Normal execution resumes after an expected fault | Pass | Ordinary RX code runs after both probes | E1 + E2 |
| Objects stay inside the declared travel interval | Pass | The limited axis stops exactly at +4 m and refuses the following steps | E1 + E2 |

### 4.2 Confirmed security gaps

#### x86_64 (currently installed)
- EFER.NXE is set. **The first 2 MiB page has been split into 4 KiB pages**: `.text` = R|X, `.rodata` = R, `.data`/`.bss`/stack = R|W, and every other address is unmapped (an access faults). All 511 2 MiB pages above it carry NX.
- CR0.WP is set: even kernel-mode writes must honour the writable bit. `.text` and `.rodata` are now genuinely read-only, so WP **really intercepts** (a write to `.text` gives #PF `0x03` at CR2=0x101000).
- **Full W^X now holds**: no page is ever writable and executable at once. Evidence: a write to `.text` is refused (error=0x03) and an instruction fetch from `.data` is refused (error=0x11), and normal execution resumes after both.

#### riscv64 (currently installed)
```text
satp = 0x8000000000080217  (MODE=8 Sv39)
```
- Sv39 with 4 KiB pages is enabled: .text R|X, .rodata R, .data/.bss/stack R|W; UART (0x10000000) and PCIe ECAM (0x30000000) each map one 2 MiB megapage R|W.
- Still no U-mode: S-mode can rewrite satp, so this blocks wrong-address accesses; it does not stop a malicious kernel from rebuilding its tables.
- PMP and paging are independent checks: a PMP pass does not replace paging; devices must sit inside the table.

#### Common architectural gaps

- No user-mode process or privilege separation.
- x86_64 is not full W^X (2 MiB granularity); riscv64 reaches W^X but has no U-mode.
- No upper-brain sandbox; a faulty or malicious brain can corrupt the base and other objects.
- The base deliberately grants no authority distinctions: a caller holding a valid handle can, in principle, command any live wire.
- Limits and maximum rates declared by objects are not enforced by the base.
- `base/` and `object.c` contain no locks and make no SMP, preemption, or interrupt-context reentrancy guarantee.
- The build uses `-fno-stack-protector` and has no stack guard page.
- Each slot has a 32-bit generation; an exhausted generation retires the slot permanently rather than wrapping (a deployment-level refusal, not an infinite budget).
- A deterministic fuzz corpus (`tests/host/fuzz_base.c`) and a generation-exhaustion white-box test (`tests/host/test_wire.c`) exist, but there is no formal proof or hardware fault-injection campaign yet.

The security verdict must therefore be phrased as:

> liteOS has demonstrated sound rejection behavior at handle and capacity boundaries and page-level permissions are now enabled on both architectures (x86_64 NX+WP to the extent the current granularity allows; riscv64 Sv39 reaching W^X). It still has no user mode and no brain sandbox, and no authority model; production memory security is not yet achieved.

## 5. Reliability assessment

### Demonstrated

- Both architectures execute the same base semantics.
- Semantic and stress suites finish with zero failures, with gate markers for `failures / bench failures / brain failures / monitor failures / mmu failures`.
- A late run does not apply multiple catch-up steps in one call.
- Position remains consistent with actual execution count. For example, motion 3 over 99 steps produces position 297.
- riscv64 programs its timer through SBI TIME, avoiding direct S-mode access to the PMP-protected CLINT. The timer interrupt only counts ticks; complex object execution remains in the main loop.
- Temporary self-test and benchmark objects are released.
- Real MMU fault injection is verified on both architectures: x86_64 read-only writes produce #PF error 0x03 and NX fetches produce #PF 0x11 with matching RIP/CR2; riscv64 read-only stores produce scause 15 and RW/NX fetches scause 12. Both recover to ordinary RX execution after the expected faults.
- The deterministic fuzz corpus (with falsifiable mutants measured) and the generation-exhaustion white-box test are both in the CI gate.

### An important observed behavior

`tick_count` and object step counts may differ by a small amount. Decision 16 explicitly states that one late `object_run(now_ns)` advances once and reschedules from the current time rather than replaying missed periods. This avoids a burst of stale actions after a delay. The live demo prints both ticks and steps so the behavior remains observable.

### Not yet demonstrated

- Resource stability during long-duration continuous operation.
- Recovery from power loss, device flapping, bus errors, and interrupt storms.
- SMP or multi-hart concurrency safety.
- Driver fault isolation and recovery from an upper-brain crash.
- Multi-level PCI bridges and runtime riscv64 PCIe hot-plug.
- Real-hardware cache, memory-ordering, and timing behavior.

## 6. Is it “small”?

The size gate uses allocated ELF sections rather than the host `objcopy --strip-debug` file length. The installed GNU `objcopy` handles x86_64 but does not recognise the riscv64 ELF, so stripped-file comparison would silently apply different definitions to the two architectures.

| metric | x86_64 | riscv64 |
|---|---:|---:|
| `.text` | 57,663 B | 45,056 B |
| `.rodata` | 12,472 B | 16,384 B |
| `.data` under each linker layout | 122,880 B | 69,664 B |
| ELF allocated load size | 193,035 B | 131,104 B |
| page tables | 12,288 B, three fixed low pages | 16,384 B, four Sv39 tables already inside `.data` |
| reserved static stack | 65,536 B | 69,600 B |
| static-RAM accounting | 135,168 B | 139,264 B |

Automated budgets are: x86_64 load size at most 224 KiB, static RAM at most 192 KiB and stack at most 64 KiB; riscv64 load size at most 160 KiB, static RAM at most 192 KiB and stack at most 72 KiB. `make check-size` fails on any overrun.

“Static RAM” is a reproducible link-time budget. It excludes device MMIO, QEMU/OpenSBI firmware and any future runtime dynamic peak. There is currently no dynamic allocator; adding one requires a separate runtime high-water gate.

> Under this explicit budget definition, both architectures qualify as lightweight prototypes. This is not an absolute small-size claim for every deployment.

## 7. Is it “extremely fast”?

The automated gate runs QEMU with `-icount shift=0,align=off,sleep=off`, using deterministic instruction advancement as a reproducible regression baseline. Ordinary QEMU runs produced host-scheduling outliers above one tick at p99.9 and above three ticks at max, so they cannot serve as a function-level budget gate. The following values are icount regression numbers, not physical nanoseconds or a WCET proof.

| full-table `object_run` metric | x86_64 (58 objects) | riscv64 (62 objects) |
|---|---:|---:|
| average | 732,413 ticks, 9.07% of one tick | 6,785 ticks, 7.74% |
| p50 | 743,669 ticks | 6,888 ticks |
| p99 | 745,286 ticks | 6,901 ticks |
| p99.9 | 745,286 ticks, **9.23%** | 6,901 ticks, **7.87%** |
| maximum | 745,286 ticks, **9.23%** | 6,901 ticks, **7.87%** |
| histogram | 99 samples in the 2^18 bucket, 1,901 in 2^19 | all 2,000 samples in 2^12 |

The automated budgets are p99.9 below 50% of one 100 Hz tick and maximum below 75%; both architectures currently pass. Target interrupts are briefly disabled around the samples so this measures `object_run` itself. Interrupt-exposed end-to-end latency requires a separate storm scenario later in the plan.

For the currently defined prototype workload, the defensible statement is:

> liteOS meets the current function-level tail-latency budgets on the deterministic QEMU regression baseline; real hardware and end-to-end real-time behavior are not yet accepted.

The following statement is not supported:

> liteOS has been proven extremely fast for every processor and production workload.

The current tests run in QEMU with a fixed workload and do not include a real control loop, SMP, cache pressure, network interrupt storms, or silicon measurements.

## 8. Required work before production security

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

## 9. Final verdict

| Dimension | Verdict | Evidence grade |
|---|---|---|
| Core-model feasibility | Supported by the current prototype | E1 + E2 |
| Cross-architecture consistency | Pass (one `base/`, one gate suite) | E2 |
| Handle and capacity reliability | Pass under stress and generation white-box tests | E1 + E2 |
| 100 Hz prototype performance (function-level tail) | Budgets met: p99.9 / max about 8.95% (x86_64), 7.66% (riscv64) | E1 + E2 |
| Real-hardware real-time behaviour (WCET, end-to-end latency) | **Not verified** | E3 |
| Long-duration run (1,000 ticks, exact accounting) | Pass | E1 + E2 |
| Interrupt storm (8 kHz burst, then recovery) | Pass (criterion proven falsifiable with a mutant) | E1 + E2 |
| Page-level permissions (real violating access on both architectures) | Pass (x86_64 0x03/0x11, riscv64 15/12) | E1 + E2 |
| Full W^X (no page both writable and executable) | **Pass**: proven by real violating accesses on both architectures | E1 + E2 |
| User mode and upper-brain sandbox | **Fail, no sandbox and no authority model** | E3 |
| Multi-core concurrency | **Unsupported**, explicitly declared and gated | E2 |
| Release metadata completeness | **Incomplete**: still needs the real repository owner and security contact | E3 |

The accurate final statement is:

> liteOS has moved from a concept to an executable, measurable, and reproducible dual-architecture prototype. Its motion-control model, handle boundaries, function-level tail latency, long-duration behaviour and interrupt-storm recovery all have reproducible measurements or gate evidence, and page-level permissions have been confirmed by real violating accesses on both architectures. **Security isolation, user mode, a brain sandbox, multi-core concurrency and real-hardware real-time behaviour are still absent** — they are not properties the prototype already possesses.

**No row in this table may claim "pass" without an evidence grade**; any new conclusion must state its source grade, or it is a documentation defect.
