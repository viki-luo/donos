# 变更记录 / Changelog

本项目所有重要变更都将记录于此。
所有变更遵循 [Keep a Changelog](https://keepachangelog.com/) 格式，
版本号遵循 [语义化版本 / Semantic Versioning](https://semver.org/)。

*All notable changes to this project are documented here, following [Keep a Changelog](https://keepachangelog.com/) and [Semantic Versioning](https://semver.org/).*

---

## [Unreleased] / 未发布

### Changed / 变更 — **破坏性：事件结构与 `motion[]` 语义**

- **`motion[]` 的量纲冻结**（设计决定 18）：此前 `motion` 的含义随物件而定，
  `object.c` 按「每个控制周期的位移」解释，文档多处按「速度」解释，两者
  只在周期恰为 1 秒时一致。现冻结为 `motion[0..2]` = 线速度（m/s）、
  `motion[3..5]` = 角速度（rad/s），全部 Q32.32；物件按自述 `period_ns`
  积分：`position += v * period_ns`。设备原生单位由物件适配层换算。
  **旧代码里 `motion = 3` 会导致物件每周期走 3 米，迁移时必须乘上 `1 << 32`**
  / **`motion[]` units are frozen** (decision 18): the meaning of `motion` used
  to be per-object; `object.c` read it as "displacement per control period"
  while several docs read it as "velocity", and the two agreed only when the
  period happened to be one second. It is now linear velocity in m/s
  (`motion[0..2]`) and angular velocity in rad/s (`motion[3..5]`), Q32.32, and
  an object integrates over its declared `period_ns`
- **`duration_ns` 改为命令新鲜度窗口**（设计决定 10 修订）：此前它只是
  「事件有效时长」的说明性字段，**从未被任何代码读取** —— `MOTION_E_DEADLINE`
  是一个永远不可能返回的状态码。现在事件自带 `issued_at_ns`，只在
  `[issued_at_ns, issued_at_ns + duration_ns]` 内可被接受，过期命令以
  `MOTION_E_DEADLINE` 拒绝且绝不执行；拾取器模型的「接受后自持」不变。
  零长度窗口、端点回绕、未来事件一律 `MOTION_E_INVAL`
  / **`duration_ns` becomes a command freshness window** (decision 10, revised):
  it used to be documentation only — **no code read it**, so `MOTION_E_DEADLINE`
  was a status code that could never be returned. An event now carries
  `issued_at_ns` and is acceptable only within
  `[issued_at_ns, issued_at_ns + duration_ns]`; a late command is refused with
  `MOTION_E_DEADLINE` and never executed, while pick-and-place
  self-sustaining motion after acceptance is unchanged. Zero-length windows,
  wrapping endpoints and events from the future are all `MOTION_E_INVAL`
- **`motion_event_t` 增加 `issued_at_ns` 字段**：结构体成员增至 6 个
  / **`motion_event_t` gains `issued_at_ns`**: six members instead of five
- **一个接口签名变更**：`void object_set_state(...)` → `bool object_set_state(...)`。
  姿态在入口校验，非法姿态整份拒绝，调用者必须能知道
  / **one signature change**: `void object_set_state(...)` becomes
  `bool object_set_state(...)`. The pose is validated on entry and an invalid
  pose is refused as a whole, so the caller has to be able to learn that
- **一个解算接口的返回类型变更**：`bool object_resolve(...)` →
  `object_resolve_status_t object_resolve(...)`。断链、成环、链过深、算术溢出
  从此可以区分；注意 `OBJECT_RESOLVE_OK` 为 0，因此**不能**再把返回值直接
  当真值使用
  / **one return-type change**: `bool object_resolve(...)` becomes
  `object_resolve_status_t object_resolve(...)`. A broken frame, a cycle, an
  over-long chain and arithmetic overflow are now distinguishable; note that
  `OBJECT_RESOLVE_OK` is 0, so the result must **not** be used as a boolean
- **一个查询接口重命名**：`object_is_still(wire)` →
  `object_commanded_is_zero(wire)`。旧名字错误暗示它判断世界中的实际静止；
  实际上它只读取相对 `frame` 的当前命令。参照系本身运动时，命令为零的物件
  在世界中仍然运动
  / **one query API rename**: `object_is_still(wire)` becomes
  `object_commanded_is_zero(wire)`. The old name incorrectly implied measured
  stillness in the world; the query only reads the current command relative to
  `frame`. An object with a zero command still moves in the world when its frame
  moves
- **一个接口签名变更**：`void object_describe(...)` →
  `bool object_describe(...)`。申报可能被校验拒绝，调用者必须能知道 ——
  静默拒绝正是死字段的来源
  / **one signature change**: `void object_describe(...)` becomes
  `bool object_describe(...)`. A description can now be refused by validation
  and the caller has to be able to learn that — a silent refusal is exactly how
  dead fields are born
- **两个接口签名变更**：`base_apply(const motion_event_t *)` →
  `base_apply(u64 now_ns, const motion_event_t *)`；
  `u32 object_run(u64 now_ns)` → `object_run_status_t object_run(u64 now_ns, u32 *stepped)`。
  调用者必须提供提交时刻，并检查执行层是否接受了这个时刻；新增只读
  `u64 object_now_ns(void)` 供调用者在同一条时间轴上取「现在」
  / **two signature changes**: `base_apply(const motion_event_t *)` becomes
  `base_apply(u64 now_ns, const motion_event_t *)`, and
  `u32 object_run(u64 now_ns)` becomes
  `object_run_status_t object_run(u64 now_ns, u32 *stepped)`. Callers must
  supply the submission instant and check whether the execution layer accepted
  it; a read-only `u64 object_now_ns(void)` lets a caller read "now" on that
  same timeline

### Fixed / 修复

- **世代号回绕让旧句柄复活**：接线表每个槽位的世代号到达 `0xFFFFFFFF` 后，上一次实现的「回绕成 0 就改回 1」会造出与**第一次使用**完全相同的句柄，旧大脑缓存的接线从此指向一个陌生物件；1 kHz 热插拔下约 50 天就会发生。现在世代号耗尽的槽位**永久退役**，分配器不再从它发号，句柄绝不复用
  / **generation wraparound resurrected old handles**: once a wiring-table slot's generation reached `0xFFFFFFFF`, the old "wrap to 0, force back to 1" logic minted a handle identical to the **first-ever** use, so a brain caching that wire suddenly addressed a stranger; at 1 kHz hot-plug this takes about 50 days. A slot whose generation is exhausted now **retires permanently**, the allocator issues no further handles from it, and a handle is never reused
- **热插拔时命令可能进了错误的参照系**：`object_plug()` 先把接线发布出去再初始化其余字段，而 `find_by_wire()` 只认接线字段 —— 窗口里命令会被接受，却按**上一个占用者**的 frame、周期与速率上限判定。现在接线是最后一个被写入的字段，可见即就绪；`object_unplug()` 也先置不可见再交还接线
  / **a command could enter the wrong frame during hot-plug**: `object_plug()` published the wire before initialising the rest, and `find_by_wire()` matches on the wire fields alone — a command accepted in that window was judged by the **previous occupant's** frame, period and rate limit. The wire is now the last field written, visible-means-ready; `object_unplug()` likewise makes the object invisible before returning the wire
- **非单位四元数被当作旋转使用**：`object_set_state()` 接受任意姿态，
  `rotate3()` 只把**全零**四元数当作「无旋转」。于是 (2,0,0,0) 或 (1,1,0,0)
  这样的输入会被当成合法旋转，把向量缩放两倍或 √2 倍 —— 错误表现成精度问题。
  现在姿态在入口按 `|q|² = 1 ± 1/1024` 校验，非法姿态**整份拒绝**、一个字段
  都不写入；全零仍表示「未申报姿态」，按无旋转处理，因为「还没上报」不是
  「报了错的」
  / **non-unit quaternions were used as rotations**: `object_set_state()`
  accepted any orientation and `rotate3()` treated only the **all-zero**
  quaternion as "no rotation". An input like (2,0,0,0) or (1,1,0,0) was taken
  as a valid rotation and scaled vectors by 2 or by sqrt(2) — an error that
  disguises itself as precision loss. A pose is now validated on entry against
  `|q|^2 = 1 +/- 1/1024` and an invalid one is **refused as a whole**, writing no
  field at all; all-zero still means "no pose declared" and maps to no rotation,
  because "not reported yet" is not "reported something wrong"
- **成环与「链过深」共用同一个失败**：`object_resolve()` 只用
  `depth >= OBJECT_FRAME_DEPTH_MAX` 一个判定，于是一条合法但较长的链会被
  报成环，而真实成环与「需要缩短链」两种现场无法区分。现在记下走过的接线，
  重复出现才是 `OBJECT_RESOLVE_E_CYCLE`；超过深度上限是
  `OBJECT_RESOLVE_E_DEPTH`；断链、目标不存在、空指针、算术溢出各有独立状态
  / **cycles and "chain too deep" shared one failure**: `object_resolve()` used
  a single `depth >= OBJECT_FRAME_DEPTH_MAX` test, so a legal but long chain was
  reported as a cycle, and the two situations needed different fixes but looked
  identical. Visited wires are now recorded and only a repeat reports
  `OBJECT_RESOLVE_E_CYCLE`; exceeding the limit reports
  `OBJECT_RESOLVE_E_DEPTH`; a broken frame, a missing target, a null pointer and
  arithmetic overflow each have their own status
- **命令、世界解算与实测状态混为一体**：执行层此前把
  `object_resolve()` 得到的世界速度积分进相对 `frame` 的 `state.position`，
  参照系自身的位移会凭空变成物件自己的相对位移；`state.linear` 与
  `state.angular` 又从未由执行层更新。现在内部命令明确保存为 `cmd_motion`，
  执行层只积分本物件相对 `frame` 的命令速度并更新实测速度字段，世界视角只由
  `object_resolve()` 返回，不写回本地状态
  / **command, world resolution, and measured state were conflated**: the
  executor integrated the world velocity returned by `object_resolve()` into a
  `state.position` relative to `frame`, causing the frame's displacement to
  become the object's own relative displacement; `state.linear` and
  `state.angular` were never updated. Commands are now held explicitly as
  `cmd_motion`; the executor integrates only the object's command relative to
  `frame` and updates measured velocity fields, while the world view is returned
  only by `object_resolve()` and never written back into local state
- **刚体速度解算遗漏 `ω × r`**：旧实现只旋转并相加线速度和角速度，挂在
  自转参照系半径 `r` 处的物件不会获得切向速度。现在逐级旋转线速度、角速度
  与实测相对位置，并以 checked Q32.32 算术加入 `ω_F × r`；参照系原点位移
  也随链上移，避免更高层围绕错误原点旋转
  / **rigid-body twist resolution omitted `omega x r`**: the old code only
  rotated and added linear and angular velocities, so an object at radius `r`
  on a rotating frame acquired no tangential velocity. Resolution now rotates
  linear velocity, angular velocity, and measured relative position at each
  level, adds `omega_F x r` with checked Q32.32 arithmetic, and carries frame
  origins upward so higher levels rotate about the correct origin
- **`object_set_state()` 保留旧轨迹的积分余数**：外部改写位姿后，下一条轨迹
  会继承改写前尚未补齐的 sub-LSB 零头，使相同新位姿和相同新命令因历史不同
  得出不同结果。现在改写状态时清空三个 `carry` 分量
  / **`object_set_state()` kept the previous trajectory's integration carry**:
  after an external pose write, the next trajectory inherited sub-LSB remainder
  from the old one, making identical new pose and command depend on history.
  All three carry components are now cleared when state is written
- **事件的 `dof` 尾部会进入模型并顺参照链累加**：`object_submit()` 照抄
  全部 6 个 `motion[]` 分量，而事件只声明了前 `dof` 个 —— 尾部可能是上一条
  命令的残值或未初始化的栈内容。`object_resolve()` 又按 6 轴累加参照链，
  于是一个 `dof = 1` 的物件能把自己「没被命令的角速度」加进子物件的世界
  运动。现在接受时把 `dof` 之外的分量归零：声明之外的字段不进入模型
  / **the `dof` tail entered the model and accumulated along the frame chain**:
  `object_submit()` copied all six `motion[]` components while an event declares
  only the first `dof`. The tail could hold the previous command's leftovers or
  uninitialised stack content, and `object_resolve()` accumulated all six axes
  along the chain — so a `dof = 1` object could add angular velocity it was
  never commanded into a child's world motion. The components outside `dof` are
  now zeroed on acceptance: what was not declared does not enter the model
- **能力申报来者不拒，且申报过的 `max_rate` 从不被检查**：`object_describe()`
  接受 `dof > MOTION_DOF_MAX`、区间反向（`limit_min > limit_max`）、负速率与
  负质量；`object_capability()` 随后把它们原样返回。申报的速率上限在任何
  代码路径上都不产生效果，等于又一处死字段。现在申报会被校验，非法申报
  **整体拒绝且不留半份记录**（`object_capability()` 继续返回 `false`）；
  `dof` 之外的轴清零；命令超出已申报的非零 `max_rate` 时返回新增的
  `MOTION_E_LIMIT`。幅值判定对 `INT64_MIN` 单独处理，避免取负溢出绕过闸门
  / **capability declarations were accepted unchecked, and a declared
  `max_rate` was never enforced**: `object_describe()` accepted
  `dof > MOTION_DOF_MAX`, inverted intervals, and negative rates or mass, and
  `object_capability()` handed them straight back. A declared rate limit had no
  effect on any code path — another dead field. Descriptions are now validated
  and an invalid one is **refused as a whole, leaving no partial record**
  (`object_capability()` keeps returning `false`); axes outside `dof` are
  zeroed; and a command exceeding a declared non-zero `max_rate` returns the new
  `MOTION_E_LIMIT`. The magnitude test handles `INT64_MIN` separately so that
  negating it cannot slip past the gate
- **Q32.32 算术未定义行为**：速度积分此前直接计算有符号 64 位
  `velocity * period_ns + carry`。例如 Q32.32 的 3 m/s 乘 1 秒周期得到
  `12,884,901,888,000,000,000`，超过 `INT64_MAX`，会进入 C 有符号溢出
  未定义行为。新增 `base/fixed.c` checked 算术层；速度积分先把速度分成
  整数项与绝对值小于 `1e9` 的余数项，分别计算后合并，不使用 128 位除法，
  也不引入 `__divti3`。加、减、乘二、Q32.32 乘法、参照链累加、四元数
  旋转与位置累计全部改为显式报告溢出；单步三个位置轴先在临时值中完成，
  任一轴失败时不提交半步状态
  / **undefined behavior in Q32.32 arithmetic**: velocity integration directly
  evaluated signed 64-bit `velocity * period_ns + carry`. At 3 m/s in Q32.32
  and a one-second period, the product is `12,884,901,888,000,000,000`, beyond
  `INT64_MAX`, invoking signed-overflow UB. A checked arithmetic layer now
  decomposes velocity into a whole term and a remainder below `1e9`, avoiding
  both signed overflow and 128-bit division helpers. Checked add, subtract,
  double, Q32.32 multiply, frame accumulation, quaternion rotation and position
  accumulation now fail explicitly; all position axes commit atomically only
  after the complete step succeeds
- **宿主边界测试与 UBSan 陷阱门禁**：新增 `make host-test` 与
  `make sanitize`，覆盖 `INT64_MIN/MAX`、3 m/s × 1 s、carry 边界、周期上界、
  正负 Q32.32 乘法和 1 ms/3 ms 余数进位等价性
  / **host boundary tests and UBSan trap gate**: `make host-test` and
  `make sanitize` cover `INT64_MIN/MAX`, 3 m/s × 1 s, carry boundaries, the
  period limit, signed Q32.32 multiplication, and 1 ms/3 ms remainder-carry
  equivalence
- **`make check` 的失败门禁一直是空转**：自检行 `failures = N` 与压力测试行
  `bench failures = N` 两处匹配用的都是无限定子串，`failures = 1` 会命中
  `bench failures = 0` 里包含的 `failures = 0`，于是 **自检有任何失败，整个门禁
  都报通过**；反过来压力测试那条规则永远不可能单独失败。现改为行首锚定
  （`^  failures = 0$` / `^  bench failures = 0$`），并在失败时把命中的
  `FAIL` 行打印出来，而不是只说一句「失败」
  / **`make check`'s failure gate never actually fired**: both the self-test
  line `failures = N` and the stress line `bench failures = N` were matched with
  unanchored substrings, so `failures = 1` matched the `failures = 0` inside
  `bench failures = 0` — any number of self-test failures still reported a
  pass, and the stress-test rule could never fail on its own. Both patterns are
  now line-anchored, and a failure prints the `FAIL` lines it found
- **速度积分单向漂移**：每步 `v * period_ns / 1e9` 各自截断到最低位，
  误差恒为同号，随步数线性累积 —— 3 m/s、100 Hz 走一秒后位置是
  `2.99999993` 而不是 `3`，`>> 32` 取整印出来是 `2` m。改为**余数进位**：
  不足一个最低位的零头留在物件侧的 `carry[3]` 里，下一步补上，
  因此不同周期的积分路径给出逐比特相同的结果
  / **unidirectional drift in velocity integration**: each step truncated
  `v * period_ns` to the least significant bit independently, so the error kept
  its sign and grew linearly with step count — 3 m/s at 100 Hz for one second
  landed on `2.99999993` m, and the truncating readout printed `2`. Integration
  now uses **remainder carry**: the sub-bit remainder lives on the object side
  in `carry[3]` and is applied to the next step, so paths with different periods
  give bit-identical results
- **`object_plug()` 拒绝超长周期**：`OBJECT_PERIOD_NS_MAX`（1 s）为执行模型
  给出明确上界；算术安全不再依赖该上界，而由 checked fixed-point 层独立保证
  / **`object_plug()` refuses over-long periods**: `OBJECT_PERIOD_NS_MAX`
  (1 s) is an explicit execution-model bound; arithmetic safety no longer
  relies on it and is enforced independently by the checked fixed-point layer
- **新鲜度判定用的是上一次 `object_run` 的旧节拍**：`object_submit()` 读的
  「现在」是执行时钟 `exec_now_ns`，只在 `object_run()` 里更新。两次运行之间
  提交的命令，无论实际迟到多久，看到的都是同一个过期时刻 —— 一个早该过期的
  命令可以无限期地被接受，而一个刚刚发出的命令却可能被判为「发出于未来」。
  现在提交时刻由调用者显式给出：`base_apply(now_ns, ev)` /
  `object_submit(w, now_ns, ev)`；基座不保存它，物件也不缓存它
  / **freshness judged against a stale execution beat**: `object_submit()`
  read `exec_now_ns`, which only `object_run()` ever updated — so every command
  submitted between two runs was judged against the same obsolete instant.
  A command could be accepted long after it should have expired. The submission
  instant is now an explicit argument: `base_apply(now_ns, ev)` and
  `object_submit(w, now_ns, ev)`; neither the base nor the object stores it
- **执行时钟可回退**：`object_run(now_ns)` 此前不检查 `now_ns` 是否小于
  上一次的值，回退时每个物件仍会各走一步，时间上「倒退」了而
  运动上「前进」了。现返回 `object_run_status_t`，回退返回
  `OBJECT_RUN_E_CLOCK_ROLLBACK` 且**不推进任何物件**
  / **the execution clock could run backwards**: `object_run(now_ns)` never
  compared `now_ns` with the previous instant, so a rollback still stepped
  every object — time went back, motion went forward. It now returns
  `object_run_status_t` and refuses a rollback with
  `OBJECT_RUN_E_CLOCK_ROLLBACK` without advancing any object
- **调度时间加法回绕**：`now_ns + period_ns` 与 `next_due_ns + period_ns`
  直接相加，接近 `UINT64_MAX` 时会回绕成一个很小的值，下一次到期就落在过去，
  运动环退化为「每拍都跑」。现全部经 `time_add_checked()`，回绕返回
  `OBJECT_RUN_E_TIME_OVERFLOW`，并以**两遍制**提交：第一遍只算排期，
  第二遍才写状态，因此失败不会留下前半张表已推进的状态
  / **scheduling additions wrapped around**: `now_ns + period_ns` and
  `next_due_ns + period_ns` were plain additions; near `UINT64_MAX` they
  wrapped to a tiny value, putting the next due instant in the past and
  degenerate a motion loop into "step on every call". Both now go through
  `time_add_checked()` and report `OBJECT_RUN_E_TIME_OVERFLOW`. Commit is
  two-phase — the first pass only computes deadlines, the second writes state —
  so a refusal cannot leave half the table advanced
- **物件首次被看见时凭空积分一个周期**：`next_due_ns == 0` 的物件被当作
  「已到期」，插上就立刻走一步，位置里因此多出一段从未经过的时间（约
  100 Hz 下 3 m/s 即 3 cm 的免费位移）。首次现在只排期，不积分
  / **an object was integrated for free the first time it was seen**: a
  `next_due_ns == 0` object counted as due and stepped immediately at plug-in.
  The first sighting now only schedules; it does not integrate
- **实机循环混用两条时间轴**：`kmain.c` 把 `arch_ticks() * NS_PER_TICK`
  直接喂给执行层，而自检/基准已把执行时钟推到 500 ms 之后。ARCH 节拍计数从
  0 起，两个时间轴一错位就是一次**真实的时间回退**，执行层正确地拒绝它，
  物件从此不再前进。新增只读 `object_now_ns()`，实机循环改为在其上加
  本循环实测的节拍增量，提交时刻同样取自执行层时间轴
  / **two different timelines mixed in the live loop**: `kmain.c` fed
  `arch_ticks() * NS_PER_TICK` straight to the execution layer, while
  verification and benchmarks had already pushed the execution clock past
  500 ms. The ARCH tick counter starts at zero, so the mismatch is a **real
  time rollback**; the new guard correctly refused it, and nothing moved
  again. A read-only `object_now_ns()` was added and the live loop now advances
  the execution clock by the measured tick delta; submission instants come from
  that same timeline

### Added / 新增

- **两架构真实 MMU 违规访问故障注入**：x86_64 的只读写入触发 error=0x03、NX 页取指得到 0x11，CR2 与保存 RIP 精确匹配后恢复；riscv64 的 R-only 写入得到 scause=15，RW/NX 页取指得到 scause=12，按精确 sepc/stval 恢复。页表位断言不再被冒充为硬件拦截证据
  / **real MMU fault injection on both architectures**: x86_64 verifies #PF 0x03 for read-only writes and 0x11 for NX instruction fetches with exact CR2/RIP matching; riscv64 verifies store fault 15 and instruction page fault 12 with exact stval/SEPC. Both recover to normal execution

- **体积与静态内存预算门禁**：`tools/size.sh` 以 ELF 已分配段统一计算两个架构的加载体积，并分别统计静态 RAM、静态栈与页表占用；`make check-size` 对 x86_64 执行 224 KiB 加载体积、192 KiB 静态 RAM、64 KiB 栈预算，对 riscv64 执行 160 KiB、192 KiB、72 KiB 预算。避免依赖当前无法识别 riscv64 ELF 的宿主 `objcopy`
  / **image and static-memory budget gate**: `tools/size.sh` uses allocated ELF sections for one cross-architecture load-size definition and separately accounts for static RAM, reserved stack and page tables. `make check-size` enforces x86_64 budgets of 224 KiB load size, 192 KiB static RAM and 64 KiB stack, and riscv64 budgets of 160 KiB, 192 KiB and 72 KiB, without depending on the host `objcopy` that cannot recognise the riscv64 ELF
- **可重复的尾延迟门禁**：满表 `object_run` 逐次记录 2,000 个样本，输出平均值、p50、p99、p99.9、最大值与 2 的幂直方图。`tools/bench.sh` 使用 QEMU `-icount shift=0,align=off,sleep=off` 建立可重复的指令级回归基线；门禁要求 p99.9 小于一个 100 Hz 节拍的 50%，最大值小于 75%。普通 QEMU 的宿主调度离群值仍作为诊断信息，但不能冒充函数 WCET；真实硬件仍需单独验收
  / **reproducible tail-latency gate**: a full-table `object_run` records 2,000 individual samples and reports average, p50, p99, p99.9, maximum and a power-of-two histogram. `tools/bench.sh` uses QEMU `-icount shift=0,align=off,sleep=off` for a reproducible instruction-level regression baseline; the gate requires p99.9 below 50% of one 100 Hz tick and maximum below 75%. Host-scheduling outliers from ordinary QEMU remain diagnostic evidence but are not presented as function WCET; real hardware still requires separate acceptance
- **MMU 隔离的第一步**（双架构均带 `mmu failures = 0` 门禁标记）：x86_64 置位 EFER.NXE，把 2 MiB 以上的 511 个大页设为非可执行，并置位 CR0.WP（2 MiB 粒度限制下代码与数据仍同页，不是完整 W^X）；riscv64 建立 Sv39 页表，`.text` R|X、`.rodata` R、`.data`/`.bss`/栈 R|W，并映射 UART 与 PCIe 两个设备大页。仍无 U 态、无大脑沙箱
  / **first step of MMU isolation** (both architectures, `mmu failures = 0` gate marker): x86_64 sets EFER.NXE, marks all 511 2 MiB pages above the kernel page non-executable, and sets CR0.WP (code and data still share page 0 at this granularity, so it is not full W^X); riscv64 builds Sv39 tables with .text R|X, .rodata R, .data/.bss/stack R|W and maps the UART and PCIe device windows. Still no user mode and no brain sandbox
- **基座之外的安全监视器**（`monitor/`，双架构自检 + 宿主边界测试）：`monitor_poll()` 不再调用 `object_run()`，调用者先推进执行层，再把 `object_run_status_t` 交给监视器观察。监视器只读现场、产出事件，从不调用 `base_apply`、从不写物件状态。目标侧与宿主侧测试共同核对轮询前后物件状态、执行时钟和步数完全不变；新增 `INT64_MIN` 实测速率测试，越界比较不再通过取绝对值引入有符号溢出。接线非预期消失只报告一次即注销
  / **out-of-base safety monitor** (per-architecture self-tests plus a host boundary test): `monitor_poll()` no longer calls `object_run()`; the caller advances execution and passes its `object_run_status_t` to the observer. The monitor reads the scene and emits events without calling `base_apply` or writing object state. Target and host tests require object state, the execution clock and the step count to remain unchanged across a poll. An `INT64_MIN` measured-rate case proves that the bound comparison no longer introduces signed overflow through absolute-value negation. Unexpected wire loss is reported once and then deregistered.
- **上层大脑与大脑热切换**（`brain/`，双架构各 14 条自检 + 实机演示）：一个大脑只是「一条普通接线 + 决策周期 + 只读目标与当前时刻的决策函数」，`base/base.c` 里连 `brain` 这个词都被 `make check-purity` 禁止。两个控制律不同的大脑 `cruise`/`shuttle` 在运行 2 秒时热切换：新大脑读物件自持的实测位置做出反向决策（1.989 m → 1.490 m，恰为 -0.5 m/s × 1 s），**没有任何交接物**
  / **upper brains and brain hot swap** (`brain/`, 14 assertions per architecture plus a live demonstration): a brain is just "an ordinary wire plus a decision period plus a decision function that reads only the target and the current instant", and the word `brain` is now forbidden inside `base/base.c` by `make check-purity`. Two brains with different control laws, `cruise` and `shuttle`, swap at two seconds: the new brain reads the object's own measured position and reverses (1.989 m to 1.490 m, exactly -0.5 m/s over 1 s), with **nothing handed over**
- **确定性模糊语料**（`tests/host/fuzz_base.c`，`make fuzz`，默认 20 万轮）：固定种子、可复现，覆盖 8 类操作（插拔、命令、命令尾部、姿态、能力申报、调度、解算），不变量由影子表独立对账。**可证伪性已实测**：把两处已修复缺陷放回去，语料分别报出数千次与上千次失败；真实代码 20 万轮零失败。诚实边界写在 Flaky 之外：`object_plug()` 的发布顺序窗口在单线程下是等价变异体，语料抓不到，也不声称抓到
  / **deterministic fuzz corpus** (`tests/host/fuzz_base.c`, `make fuzz`, 200k rounds by default): fixed seed, reproducible, covering 8 operation classes (plug, unplug, command, command tail, pose, capability, scheduling, resolve), with invariants cross-checked against a shadow table. **Falsifiability was measured**: putting two fixed defects back produced thousands and over a thousand failures respectively, while the real code passes 200k rounds. The honest limit is recorded too: the `object_plug()` publish-order window is an equivalent mutant under single-threaded execution, so the corpus cannot reach it and does not claim to
- **接线世代纪律宿主白盒测试**（`tests/host/test_wire.c`，20 条）：直接包含 `base/base.c` 把接线表推到世代耗尽的边界，不再需要跑四十亿次热插拔；覆盖世代递增、复用不复活、耗尽即退役、全表耗尽报满、高地位别名拒绝、世代/槽位伪造拒绝、越界槽位拒绝、表满等
  / **wiring-generations host white-box test** (`tests/host/test_wire.c`, 20 assertions): includes `base/base.c` directly to push the wiring table to the generation-exhaustion edge, avoiding four billion hot-plugs; covers increasing generations, reuse without resurrection, retirement on exhaustion, exhaustion reporting, high-bit alias rejection, generation/slot forgery rejection, out-of-range rejection, and a full table
- **热插拔发布顺序验证段**（`base/verify.c` 第十三部分，双架构各 9 条）：旧占用者的命令数、位姿、能力申报、事件数与步数都不会泄漏到复用槽位的新占用者，新占用者带着自己声明的 unit 与周期出现
  / **hot-plug publish-order section** (`base/verify.c`, part 13, 9 assertions per architecture): a reused slot's new occupant inherits none of the previous occupant's command, pose, capability, event count or step count, and appears with its own declared unit and period
- **姿态合法性与解算成因验证段**（`base/verify.c` 第十二部分，双架构各 12 条）：
  全零姿态被接受为「未申报」、(2,0,0,0) 被拒、(1,1,0,0) 被拒、`INT64_MAX`
  分量被拒且不触发溢出、被拒姿态不扰动已有状态、合法单位四元数被接受并可读回、
  死接线报 `NO_OBJECT`、空指针报 `INVAL`、离开的参照系报 `BROKEN_FRAME`、
  两物件互指报 `CYCLE`、合法但过长的链报 `DEPTH` 而**不是** `CYCLE`
  / **pose validity and resolve-failure section** (`base/verify.c`, part 12,
  12 assertions per architecture): an all-zero pose is accepted as undeclared,
  (2,0,0,0) refused, (1,1,0,0) refused, an `INT64_MAX` component refused without
  overflow, a refused pose leaving the previous state untouched, a proper unit
  quaternion accepted and read back, a dead wire reporting `NO_OBJECT`, a null
  pointer reporting `INVAL`, a departed frame reporting `BROKEN_FRAME`, two
  objects pointing at each other reporting `CYCLE`, and an over-long acyclic
  chain reporting `DEPTH` rather than `CYCLE`
- **命令 / 解算 / 实测分离验证段**（`base/verify.c` 第十一部分，双架构）：
  验证转动参照系的位置耦合项 `ω × r`、世界角速度继承、实测半径改变切向速度、
  相对静止物件不累计自己的位姿但仍解算出参照系的世界速度、执行层更新
  `state.linear` / `state.angular`，以及外部改写位姿后旧积分余数不会泄漏进新轨迹
  / **commanded / resolved / measured separation section** (`base/verify.c`,
  part 11, on both architectures): verifies the rotating-frame coupling term
  `omega x r`, inherited world angular velocity, measured radius controlling
  tangential speed, a relatively still object keeping zero local pose while
  resolving to its frame's world velocity, executor updates to `state.linear`
  and `state.angular`, and stale integration carry being cleared after an
  externally written pose
- **新鲜度验证段**（`base/verify.c`，第七部分）：零长度窗口拒绝、过期事件
  返回 `MOTION_E_DEADLINE`、端点回绕拒绝、边界 `now == deadline` 接受、
  未来事件拒绝 —— `MOTION_E_DEADLINE` 由此第一次成为可达路径。该段只使用
  自己的局部提交时刻，**不再依赖「前面的测试把执行时钟推到了约 106 ms」**，
  因此结论不随测试顺序漂移
  / **freshness verification section** (`base/verify.c`, part 7): zero-length
  window refused, expired event returns `MOTION_E_DEADLINE`, wrapping endpoint
  refused, boundary `now == deadline` accepted, event from the future refused —
  `MOTION_E_DEADLINE` is reachable for the first time. The section uses only its
  own local submission instants and **no longer depends on earlier tests having
  pushed the execution clock to about 106 ms**, so the verdict does not drift
  with test order
- **执行时钟安全验证段**（`base/verify.c`，第九部分）：正向推进被接受、
  回退被拒且不推进任何物件、同一时刻不算回退、插上后首次 `object_run`
  只排期不步进、下一周期边界恰好一步、空步数指针被拒、排期加法回绕被拒、
  被拒之后时钟仍可继续使用。全部在**两个架构**上实测
  / **execution-clock safety section** (`base/verify.c`, part 9): forward
  advance accepted, rollback refused without advancing anything, the same
  instant is not a rollback, the first `object_run` after plug-in only
  schedules, the next period boundary performs exactly one step, a null step
  counter is refused, a wrapping scheduling addition is refused, and the clock
  remains usable afterwards. Measured on **both architectures**
- **能力申报与 `dof` 尾部验证段**（`base/verify.c`，第十部分，两个架构各 21 条
  断言）：尾部归零、尾部不得顺参照链泄漏进子物件的世界运动、`dof` 越界拒绝、
  区间反向拒绝、负速率拒绝、负质量拒绝、被拒后仍为「未申报」、合法申报被接受、
  未激活轴清零、超速率命令返回 `MOTION_E_LIMIT`、恰好等于上限被接受、
  负方向同一判定、`INT64_MIN` 不能绕过闸门、被拒命令不扰动已接受运动、
  未申报能力的物件不设限
  / **capability and `dof`-tail verification section** (`base/verify.c`,
  part 10, 21 assertions on each architecture): the tail is normalised to zero,
  the tail cannot leak through the frame chain into a child's world motion,
  out-of-range `dof` refused, inverted interval refused, negative rate refused,
  negative mass refused, the object stays undeclared after refusals, a
  well-formed description is accepted, inactive axes are zeroed, an over-rate
  command is refused with `MOTION_E_LIMIT`, exactly-at-limit accepted, the same
  in the negative direction, `INT64_MIN` cannot bypass the gate, a refused
  command leaves the accepted motion untouched, and an object that declared
  nothing is not rate-limited
- **基座纯净性门禁 `make check-purity`**（纳入 `check-release`）：用符号名匹配
  断言 `base/base.c` 里不出现 `cap`、`limit_min`、`limit_max`、`max_rate`、
  `mass`、`orientation`、`position`、`described`、`still` —— 基座一旦开始
  理解这些名字，就不再是无状态函数了。该门禁已实测会**真的失败**
  （向副本注入 `object_capability_t cap` 即被捕获），不是空跑
  / **base-purity gate `make check-purity`** (part of `check-release`): symbol
  matching asserts that `base/base.c` never mentions `cap`, `limit_min`,
  `limit_max`, `max_rate`, `mass`, `orientation`, `position`, `described` or
  `still` — as soon as the base understands those names it is no longer a
  stateless function. The gate was shown to **actually fail** by injecting
  `object_capability_t cap` into a copy, so it is not a no-op

### Changed / 变更

- **句柄高位别名**：`WIRE_GEN` 右移后截断为 32 位，此前未校验高 16 位，
  对任意有效句柄置位高位即可得到一个同时有效、却与原文不同的别名。
  新增 `WIRE_CANONICAL_MASK`，`slot_of()` 拒绝非规范句柄；压力测试的
  伪造形状由 6 种增至 7 种，覆盖「活句柄 OR 高位」这一情形
  / **high-bit handle aliasing**: `WIRE_GEN` truncated after the shift and the
  top 16 bits were never checked, so ORing a high bit into any live handle
  produced a second handle that was also accepted. `WIRE_CANONICAL_MASK` now
  makes `slot_of()` reject non-canonical handles; the stress test covers seven
  forged shapes instead of six
- **CI 与 `make check` 的运行时符号门禁此前是空跑**：它禁止
  `__udivdi3`/`__umoddi3`，理由是「变量除数会产生 libcall」。实测在两个
  目标上都不成立 —— `rv64imac` 带 M 扩展、`x86_64` 有 `divq`，变量除数都
  编译成硬件指令，该规则从未触发。改为断言事实：裸机链接没有合成
  运行时库，镜像中**不得存在任何 undefined 符号**（`nm --undefined-only`）
  / **CI and `make check` division gate was a no-op**: it banned
  `__udivdi3`/`__umoddi3` on the theory that a variable divisor emits a libcall.
  That is false on both targets (`rv64imac` has the M extension, `x86_64` has
  `divq`), so it never fired. Replaced by the statement that actually holds: a
  bare link has no synthetic runtime library, so the image must carry **no
  undefined symbols** at all

### Added / 新增

- **GitHub 社区标准文件**：`SECURITY.md`（安全策略与**当前已实测的安全缺口**）、
  `SUPPORT.md`（求助渠道与项目边界）、根目录 `CONTRIBUTING.md`（指向完整指南）、
  `CITATION.cff`、`docs/README.md`（文档索引与术语对照）、`CODEOWNERS`、
  `.editorconfig`、Issue 模板 `config.yml`（禁用空白 Issue，指向安全与讨论）
  / **GitHub community standards**: `SECURITY.md` (policy plus **measured current
  gaps**), `SUPPORT.md` (help channels and project scope), root `CONTRIBUTING.md`
  (pointer), `CITATION.cff`, `docs/README.md` (index and glossary), `CODEOWNERS`,
  `.editorconfig`, and an issue-template `config.yml`
- **CI**：`.github/workflows/build.yml` 在两架构上构建并禁止 64 位除法运行时符号；
  `.github/workflows/verify.yml` 在 QEMU 中启动内核，断言自检零失败、压力测试零失败、
  每一项测量都出现、保护状态被自报；`.github/dependabot.yml` 只维护 Actions
  / **CI**: `build.yml` builds both architectures and forbids 64-bit division
  runtime symbols; `verify.yml` boots under QEMU and asserts zero self-test
  failures, zero stress failures, presence of every measurement, and a printed
  protection report; `dependabot.yml` maintains Actions only
- **`make check`**：本地与 CI 运行同一套门禁，断言跨架构一致性而非绝对性能数字
  / **`make check`**: the same gate locally and in CI, asserting cross-architecture
  consistency rather than absolute performance numbers
- 完整设计文档（中英双语）/ Full design document (bilingual)
- 贡献指南与行为准则 / Contributing guide and code of conduct
- Issue 与 PR 模板 / Issue and PR templates
- 引导路径文档（中英双语）/ Boot path documentation (bilingual),
  covering the PVH 32-bit entry, the 32→64 transition, and the
  file-backed writable segment requirement
- 基座实现：`base_init` / `base_apply` / `base_arrive` / `base_depart`，
  以及物件侧的 `object_init` / `object_plug` / `object_unplug` /
  `object_submit` / `object_is_still`
  / Base implementation and the object-side counterpart
- 启动自检：11 项断言覆盖运动语义、无效接线、无效参照系与越界自由度
  / Boot self-test: 11 assertions covering motion semantics, invalid
  wires, invalid frames, and out-of-range degrees of freedom
- 公共短类型头 `base/include/types.h`，让架构层不必依赖 `base.h`
  / A shared typedef header so the architecture layer need not depend on `base.h`
- 架构层：串口控制台、端口 I/O、IDT、8259 PIC 重映射、PIT 周期中断与开中断
  / Architecture layer: serial console, port I/O, IDT, 8259 PIC remap, PIT
  periodic interrupt and enabling interrupts
- **异常处理**：CPU 异常不再变成三重故障后静默停住，而是打印向量号、
  错误码与出错指令地址（`make FAULT_TEST=1` 可验证）
  / **Exception handling**: a CPU exception now prints its vector, error code
  and faulting instruction address instead of silently triple-faulting
  (`make FAULT_TEST=1` verifies this)
- **总线发现**：PCI 扫描找到真实设备，接线由此自动产生。此前接线靠测试
  代码手写 `object_plug` 造出来；现在来自硬件扫描。发现的 6 个设备
  （IDE、VGA、e1000 等）在 QEMU 上验证通过
  / **Bus discovery**: a PCI scan finds real devices and wiring arises from it.
  Earlier wiring was created by hand-written `object_plug` calls in tests; now
  it comes from scanning hardware. Six devices (IDE, VGA, e1000, ...) verified on QEMU

- **统一模型验证**（`base/verify.c`）：用同一套原语建一个「大脑」和两个
  「手脚」，验证基座无法区分它们 —— 大脑没有特权槽、换大脑与物件热插拔
  同机制、同一接线既是大脑又是可被驱动的物件
  / **Unified-model verification** (`base/verify.c`): a brain and two limbs built
  with the same primitive, showing the base cannot tell them apart — no
  privileged slot, brain swap uses the same mechanism as object hot-plug, and
  the same wire is both a brain and a drivable object
- `object_motion()`：读取物件某个自由度的运动量，供「大脑直接读现场」使用
  / `object_motion()`: read one degree of freedom of an object, so a newly
  attached brain can read the situation directly
- **双向接口契约**（设计文档 §3.6–3.8）：集中写清跨越基座边界的全部数据 ——
  发给物件的只有 `motion_event_t`，从物件只收 `unit` / `period_ns`，
  没有查询、没有状态上报；并说明「零个状态」为何是设计而非缺失
  / **Bidirectional interface contract** (design §3.6–3.8): all data crossing the
  base boundary in one place — only `motion_event_t` is sent to an object, only
  `unit` / `period_ns` are received from it, with no query and no state report;
  and why "zero states" is design rather than omission
- **物件符合性标准**（`docs/conformance.md`，中英双语）：R1–R8 八条要求及各自的
  验证状态；能力与状态的区分；3D 位姿为何不进基座
  / **Object conformance standard** (`docs/conformance.md`, bilingual): requirements
  R1–R8 with their verification status, the capability/state split, and why 3D
  pose stays out of the base
- **R8 可自述**：物件侧新增 `object_state_t`（位置、姿态四元数、线/角速度）与
  `object_capability_t`（自由度、限位、最大速率、质量），以及
  `object_describe` / `object_capability` / `object_set_state` / `object_state`；
  `base/verify.c` 增加 6 项断言。基座接口不变
  / **R8 self-description**: the object side gains `object_state_t` and
  `object_capability_t` plus four accessors; `base/verify.c` adds 6 assertions.
  The base interface is unchanged
- **参照系由物件自己解（设计决定 15，方案 A）**：新增 `object_resolve()`，沿参照链
  直读参照系物件的运动，物件之间直连、不经过基座；成环与断链由物件侧拒绝。
  `base/verify.c` 增加 6 项断言。参考实现只做平移合成，尚不按参照系姿态旋转
  / **Objects resolve their own frame (decision 15, option A)**: new
  `object_resolve()` walks the frame chain reading each frame object's motion
  directly, object to object, never through the base; cycles and broken chains
  are rejected on the object side. `base/verify.c` adds 6 assertions. The
  reference only composes translation and does not yet rotate by orientation
- **执行模型（设计决定 16）**：新增 `object_run(now_ns)` 与 `object_step_count()`，
  按各物件到达时申报的 `period_ns` 推进其运动环，把解算出的运动积分进位置；
  时间由调用者给出，基座不调度、不计时。`base/verify.c` 增加 6 项断言（合成时钟）
  / **Execution model (decision 16)**: new `object_run(now_ns)` and
  `object_step_count()` advance each object's motion loop at the `period_ns` it
  declared on arrival, integrating resolved motion into position; time is
  supplied by the caller and the base neither schedules nor keeps time.
  `base/verify.c` adds 6 assertions (synthetic clock)
- **执行层接入真实时钟**：PIT 100 Hz 节拍在 `kmain.c` 主循环中驱动 `object_run`，
  中断处理只累加节拍；启动后物件随真实时间持续运动，内核不再停在 `[idle] halting`
  / **Execution layer on a real clock**: 100 Hz PIT ticks drive `object_run` from
  the main loop in `kmain.c`, while the interrupt handler only counts ticks;
  objects now keep moving in real time and the kernel no longer stops at
  `[idle] halting`
- **运行时热插拔**：新增 `pci_poll()`，主循环每 100 ms 重扫 PCI 总线并与上次
  结果比对，新设备到达（`object_plug`）、消失设备离开（`object_unplug`）；
  拔出请求通过 QEMU PIIX4 ACPI 热插拔寄存器（0xAE00）完成弹出。
  新增 `tools/hotplug-test.sh`，运行中插入并拔出 e1000 网卡验证
  / **Runtime hot-plug**: new `pci_poll()` rescans the PCI bus every 100 ms from
  the main loop and diffs against the previous scan — new devices arrive, vanished
  ones depart; removal requests are completed through the QEMU PIIX4 ACPI hotplug
  registers (0xAE00). New `tools/hotplug-test.sh` adds and removes an e1000 NIC
  while the kernel runs
- **第二个处理器架构 riscv64**（`make ARCH=riscv64`，QEMU virt + OpenSBI）：
  新增 `arch/riscv64/`（入口、链接脚本、16550 串口、time CSR 驱动执行层）。
  `base/` 未改一行，全部验证在 riscv64 上通过；Makefile 的架构相关选项集中到一处
  / **Second processor architecture, riscv64** (`make ARCH=riscv64`, QEMU virt +
  OpenSBI): new `arch/riscv64/` (entry, linker script, 16550 UART, time CSR
  driving the execution layer). `base/` is unchanged and all verification passes
  on riscv64; architecture-specific Makefile options are gathered in one place
- **参照系姿态旋转**：`object_resolve()` 每升一级先按参照系的姿态四元数旋转运动
  （线 0..2、角 3..5），再叠加参照系自身的运动；定点乘法用 128 位中间积。
  `base/verify.c` 增加 4 项断言（不转、转 180°、转 90°、相对旋转参照系静止），
  x86_64 与 riscv64 均通过
  / **Frame orientation**: at each level `object_resolve()` first rotates the
  motion (linear 0..2, angular 3..5) by the frame's orientation quaternion, then
  adds the frame's own motion; fixed-point multiplication uses a 128-bit
  intermediate. `base/verify.c` adds 4 assertions, passing on x86_64 and riscv64
- **符合性标准 R1–R3 自动检验**：`base/verify.c` 增加 6 项断言（零运动即静止、
  事件到期后继续运动、申报属性可读且离开后失效）；新增 `object_declared()`，
  物件侧保存到达时申报的 `unit` / `period_ns` 供大脑直读。R1–R8 现均已检验或由类型保证
  / **Automated R1–R3 conformance checks**: `base/verify.c` adds 6 assertions
  (zero motion means still, motion continues after the event expires, declared
  properties are readable and gone after departure); new `object_declared()`
  keeps the arrival-time `unit` / `period_ns` on the object side for the brain.
  R1–R8 are now all tested or type-enforced
- **riscv64 总线发现**：PCI 驱动移到两种架构共用的 `drivers/pci.c`；配置空间访问与
  热插拔弹出改由架构层的 `arch_pci_read32()` / `arch_pci_eject_pending()` 提供
  （x86_64 端口 I/O，riscv64 ECAM 0x30000000）。QEMU virt 上发现主桥、e1000、virtio-net
  / **riscv64 bus discovery**: the PCI driver moves to `drivers/pci.c`, shared by both
  architectures; configuration-space access and hotplug eject are supplied by the
  architecture layer through `arch_pci_read32()` / `arch_pci_eject_pending()`
  (x86_64 port I/O, riscv64 ECAM at 0x30000000). On QEMU virt it finds the host
  bridge, an e1000 and a virtio-net device
- **riscv64 定时器中断**：新增 `arch/riscv64/trap.c` 与 `traps.S`（`stvec` 直接模式，
  保存全部寄存器后调用 `trap_dispatch`）。定时器经 SBI TIME 扩展（EID 0x54494D45）
  编程 —— OpenSBI 用 PMP 把 CLINT（0x02000000）设为 S 态无访问权限，直读写分别得到
  scause 5 与 scause 7，实测 pmpcfg0 确认。主循环 `wfi` 等中断；迟到运行按
  设计决定 16 只推进一次、不补跑，因此节拍与步数可能相差一小步，串口会同时
  打印二者并说明原因
  / **riscv64 timer interrupts**: new `arch/riscv64/trap.c` and `traps.S` (`stvec`
  in direct mode; all registers are saved before calling `trap_dispatch`). The timer
  is programmed through the SBI TIME extension (EID 0x54494D45) — OpenSBI marks the
  CLINT (0x02000000) as S-mode no-access through PMP, so a direct read or write
  raises scause 5 or scause 7 respectively, confirmed by reading pmpcfg0. The main
  loop waits in `wfi`; under decision 16 a late run steps once rather than catching
  up, so ticks and steps may differ slightly and both are printed

- **安全、可靠性与性能评估**：新增目标内核基准与压力测试，覆盖合法/拒绝路径、
  1 层与 8 层参照链、全部可用槽位的最坏推进、表满故障关闭、伪造句柄拒绝和
  20,000 次插拔循环；新增 `tools/bench.sh` 与中英文评估报告。QEMU 实测最坏推进
  约占一个 100 Hz 节拍的 1% 至 2%
  / **Security, reliability, and performance assessment**: new in-kernel benchmarks
  and stress tests cover accepted/rejected paths, one- and eight-object frame chains,
  a worst pass over every available slot, fail-closed capacity exhaustion, forged
  handles, and 20,000 plug/unplug cycles. `tools/bench.sh` and bilingual assessment
  reports reproduce the results; the measured worst pass consumes about 1% to 2% of
  a 100 Hz tick under QEMU
- **保护状态自报**：x86_64 打印 CR0/CR4/CR3 与页目录权限；riscv64 打印 satp。
  当前证据明确显示 x86_64 为可写可执行恒等映射且 `CR0.WP=0`，riscv64 为 Bare
  模式，因此尚无生产级内存或上层大脑隔离
  / **Protection-state self-reporting**: x86_64 prints CR0/CR4/CR3 and page-directory
  permissions, while riscv64 prints satp. The evidence shows writable/executable
  identity mappings with `CR0.WP=0` on x86_64 and Bare mode on riscv64, so production
  memory isolation and upper-brain sandboxing are not yet present

### Changed / 变更

- **基座不再接收任何自述属性（设计决定 17）**：删除 `wire_desc_t`，`base_arrive()`
  改为无参数；接线表每槽只剩世代号与在用标记。`unit` / `period_ns` 只保存在物件侧，
  大脑经 `object_declared()` 直读。`object_plug()` 签名不变
  / **The base no longer receives any self-declared properties (decision 17)**:
  `wire_desc_t` is removed and `base_arrive()` takes no arguments; each wiring-table
  slot holds only a generation and an in-use flag. `unit` / `period_ns` live only on
  the object side, read by the brain through `object_declared()`. `object_plug()`
  keeps its signature
- **确立统一模型：一切皆是运动物件**。大脑不再是一个独立类别，而是「恰好正在
  驱动基座」的普通接线 —— 没有「大脑槽」，换大脑与物件热插拔是同一个机制，
  基座对称且不授权；**基座是唯一的非物件**（纯函数）
  / **Unified model established: everything is a motion object.** A brain is no
  longer a separate category but simply the ordinary wire that happens to be
  driving the base — there is no "brain slot", replacing a brain is the same
  mechanism as object hot-plug, and the base is symmetric and grants no
  authority; **the base is the only non-object** (a pure function)
- `base_attach` → `base_arrive`，`base_detach` → `base_depart`：
  接线不再由代码「接入」，而是由总线驱动上报「到达」——明确了接线
  来自发现事件，是唯一来源
  / `base_attach` → `base_arrive` and `base_detach` → `base_depart`: wiring is
  no longer "attached" by code but reported as an arrival by a bus driver —
  making discovery the single source of wiring
- 设计模型从「进程/文件」式内核彻底转向「一切皆运动」基座模型
  / The design model moved completely from a process/file-style kernel to the "everything is motion" base model
- 基座定位重新确立为**无状态**：不保存世界信息，只持有接线
  / The base is re-established as **stateless**: it stores no world information, only wiring

### Fixed / 修复

- **统一模型自检遗留三个物件**：`brain2`、`hand`、`foot` 在测试结束后未归还，
  会静默减少后续可用容量。现自检结束时全部拔出，基准入口容量只反映真实 PCI
  设备占用
  / **The unified-model self-test leaked three objects**: `brain2`, `hand`, and
  `foot` remained plugged after the test, silently reducing later capacity. They are
  now released, so benchmark entry capacity reflects only real PCI devices
- **接线句柄复用会让死句柄复活**。此前 `wire_t` 就是槽位下标，旧大脑
  离开后新物件立刻复用同一槽位，于是「已离开的句柄」重新变得有效，
  旧大脑会对着陌生物件下指令。在热插拔系统里这是事故级缺陷。
  现句柄编码为 `(世代号 << 16) | 槽位`，世代单调递增，离开的句柄
  永远只是个死句柄，不会再指向任何东西。
  / **Wire handle reuse resurrected dead handles.** `wire_t` used to be a slot
  index, so a departed brain's slot was immediately reused and the "dead"
  handle became valid again — the old brain would then command a stranger. In
  a hot-plug system that is an accident-class defect. Handles now encode
  `(generation << 16) | slot` with a monotonic generation, so a departed
  handle stays dead forever.

- **引导入口必为 32 位代码**。`XEN_ELFNOTE_PHYS32_ENTRY` 的 `PHYS32`
  即「32 位入口」，QEMU 进入时 CPU 处于 32 位保护模式。此前入口按
  64 位编写，`REX` 前缀被当成 `dec`，`lea` 退化为「取位移」，导致
  `rsp`/`cr3` 指向错误地址、内核在 `base_init` 处静默停住。
  现改为 32 位 stub 完成 PAE → CR3 → LME → PG → 远跳转的切换。
  / **The boot entry must be 32-bit code.** `PHYS32` in
  `XEN_ELFNOTE_PHYS32_ENTRY` means a 32-bit entry point; QEMU enters in
  32-bit protected mode. The entry was previously written as 64-bit
  code, so `REX` prefixes were decoded as `dec` and `lea` degenerated
  into "take the displacement", leaving `rsp`/`cr3` pointing at the
  wrong addresses and the kernel silently stalling in `base_init`. It
  is now a 32-bit stub doing PAE → CR3 → LME → PG → far jump.
- **可写段必须文件承载**。纯 `NOBITS` 的 `.bss` 自成一 ELF 段时
  `FileSiz = 0`，QEMU 的 PVH 加载器不会映射它，写它直接页错误。
  现把 `.bss` 与栈并入文件承载的 `.data` 段。
  / **The writable segment must be file-backed.** A purely `NOBITS`
  `.bss` forming its own ELF segment has `FileSiz = 0`; QEMU's PVH
  loader never maps it, so writing to it faults. `.bss` and the stack
  are now folded into the file-backed `.data` segment.
- **栈不得与数据同址**。此前栈顶与接线表起点重合，`base_init` 的返回
  地址入栈即被清零。现栈排在可写段最末尾、`__bss_end` 之后。
  / **The stack must not share memory with data.** The top of the stack
  used to coincide with the start of the wire table, so `base_init`'s
  return address was zeroed as soon as it was pushed. The stack now
  sits at the very end of the writable segment, after `__bss_end`.

### Removed / 移除

- 命令态 / 自报态双表模型（与无状态基座冲突）/ Dual-table model (conflicts with the stateless base)
- 时间轮记账与「到点置静止」/ Time-wheel ledger and "set to still at expiry"
- 运动学拓扑、双重 deadline、故障检测（均属大脑职责）
  / Kinematic topology, dual deadlines, fault detection (all belong to the brain)
- 旧的内核骨架（`kernel/core/sched.c`、`ipc.c`、`cap.c`）标记为废弃
  / Legacy kernel skeleton marked deprecated

### Deprecated / 废弃

- `kernel/include/hal.h`、`cap.h`、`ipc.h`、`mm.h`、`sched.h` — 沿用通用微内核假设，待重写
  / Based on generic microkernel assumptions; to be rewritten

---

## 版本号规则 / Versioning

| 版本 / Version | 含义 / Meaning |
|---|---|
| `0.x` | 设计阶段，接口不稳定 / Design stage, unstable interface |
| `1.0.0` | 基座语义冻结、首个可运行目标 / Base semantics frozen, first runnable target |