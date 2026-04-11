# Real-Time Scheduling Theory

## Overview

Real-time systems must not only produce correct results but also meet temporal constraints. Scheduling theory provides the mathematical foundation for analysing whether a set of tasks with deadlines can be guaranteed to meet them. This file covers rate monotonic analysis, earliest deadline first, utilisation bounds, blocking, and the classic algorithms used in safety-critical embedded systems.

---

## Key Concepts

### Hard, Firm, and Soft Real-Time

**Hard real-time:** missing a deadline is a system failure. Flight control, pacemakers, airbags. The system must be provably correct under worst-case conditions.

**Firm real-time:** missing a deadline produces no value, but isn't catastrophic. Video frame deadlines, multimedia streaming.

**Soft real-time:** missing a deadline degrades quality but the result is still useful. Web servers, user interfaces.

Scheduling analysis is most important for hard real-time — you must prove, not just measure, that deadlines are met.

---

### Task Model

A **task** $\tau_i$ is characterised by:

- **$T_i$** — period (time between releases of consecutive instances).
- **$C_i$** — worst-case execution time (WCET).
- **$D_i$** — deadline (typically $D_i \leq T_i$).
- **Priority** — fixed or dynamic, depending on the scheduler.

**Utilisation:** $U_i = C_i / T_i$ — the fraction of CPU time task $i$ consumes.

**Total utilisation:** $U = \sum U_i$.

A necessary condition for schedulability: $U \leq 1$ (on a single processor). If utilisation exceeds 1, no scheduler can meet all deadlines.

---

### Rate Monotonic (RM) Scheduling

**Rate Monotonic** is a fixed-priority algorithm: tasks with shorter periods get higher priority.

**Utilisation bound (Liu and Layland, 1973):**

$$
U \leq n(2^{1/n} - 1)
$$

For n tasks, this bound approaches $\ln 2 \approx 0.693$ as n grows. If total utilisation is below this bound, RM is guaranteed to meet all deadlines.

| n   | Bound   |
| --- | ------- |
| 1   | 1.000   |
| 2   | 0.828   |
| 3   | 0.780   |
| 4   | 0.757   |
| ∞   | 0.693   |

**Properties:**

1. **Optimal among fixed-priority schedulers.** No other static priority assignment does better.
2. **Predictable.** Priority is fixed; easy to reason about.
3. **Conservative bound.** Actual schedulability is often higher; use exact analysis.

**Exact analysis — Response Time Analysis (RTA):**

Instead of the utilisation bound, compute the worst-case response time for each task:

$$
R_i = C_i + \sum_{j \in hp(i)} \left\lceil \frac{R_i}{T_j} \right\rceil C_j
$$

Where $hp(i)$ is the set of higher-priority tasks. This is a fixed-point equation — iterate until $R_i$ converges. If $R_i \leq D_i$ for all tasks, the system is schedulable.

**Example:**

Three tasks:
- $\tau_1$: C=1, T=4
- $\tau_2$: C=2, T=6
- $\tau_3$: C=3, T=8

Utilisation: $0.25 + 0.33 + 0.375 = 0.955$. Above the Liu-Layland bound for n=3 (0.780) — but RTA may show it's still schedulable.

---

### Earliest Deadline First (EDF)

**EDF** is a dynamic-priority algorithm: at any moment, the task closest to its deadline has the highest priority.

**Utilisation bound:**

$$
U \leq 1
$$

EDF is optimal — it can schedule any task set with utilisation up to 1.

**Pros:**

1. **Higher utilisation.** Up to 100%, vs ~70% for RM.
2. **Works for any deadline pattern**, not just $D = T$.

**Cons:**

1. **Dynamic priorities.** More scheduler overhead.
2. **Catastrophic overload.** When overloaded, behaviour is undefined — tasks miss deadlines cascading. RM fails more gracefully (always the longest-period tasks first).
3. **Less predictable.** Priority depends on current time and deadline.

**Used in:** real-time Linux with SCHED_DEADLINE, some aerospace systems, high-performance embedded.

---

### Deadline Monotonic (DM)

A generalisation of RM: when $D_i < T_i$ (deadline before period), use DM — tasks with shorter **deadlines** (not periods) get higher priority. DM is optimal among fixed-priority schedulers for arbitrary deadlines.

---

### Priority Inversion and Solutions

**Priority inversion:** a high-priority task is blocked by a lower-priority task that holds a resource.

**Classic scenario:**

1. Task L (low priority) holds a mutex.
2. Task H (high priority) wakes up, tries to acquire the mutex, and blocks.
3. Task M (medium priority) wakes up and preempts L.
4. M runs, L doesn't release the mutex, H stays blocked.

In effect, M has higher priority than H, transitively. The high-priority task misses its deadline.

**Real-world example: Mars Pathfinder (1997).** The rover experienced repeated watchdog resets due to priority inversion involving a high-priority bus management task blocked by a low-priority meteorological task holding a mutex, with medium-priority tasks preempting. Diagnosed in-flight; fixed by enabling priority inheritance.

**Solutions:**

**1. Priority inheritance (PIP).**

When a high-priority task blocks on a mutex held by a lower-priority task, the lower-priority task temporarily inherits the higher priority. When the mutex is released, the original priority is restored.

Benefits: fixes the Mars Pathfinder-style bug. Simple conceptually.

Drawbacks: transitive inheritance chains can be complex. Blocking time is still O(nested locks).

**2. Priority ceiling protocol (PCP).**

Each mutex has a "ceiling" equal to the highest priority of any task that might acquire it. A task can acquire a mutex only if its priority is strictly greater than the ceiling of all mutexes currently held by other tasks. This prevents deadlocks and bounds blocking time.

Variants:
- **Immediate Ceiling Priority Protocol (ICPP):** task inherits the mutex's ceiling as soon as it acquires the mutex.
- **Original Ceiling Priority Protocol (OCPP):** more conservative.

**3. Disable interrupts in the critical section.**

For very short sections, disabling interrupts prevents any preemption. Not scalable or safe for longer sections, but common for atomic sequences.

**Blocking time in analysis:**

Schedulability analysis must account for blocking time $B_i$ — the maximum time task $i$ can be blocked by lower-priority tasks due to resource access. The RTA becomes:

$$
R_i = C_i + B_i + \sum_{j \in hp(i)} \left\lceil \frac{R_i}{T_j} \right\rceil C_j
$$

---

### Jitter and Release Time

**Jitter:** variation in a task's release time. In practice, periodic tasks aren't exactly periodic — interrupts, ISRs, and clock drift introduce jitter.

**Release jitter** is treated as an addition to the response time equation. Task $i$ with release jitter $J_i$ can delay other tasks by up to $J_i$ beyond its nominal release.

**WCET issues:**

Accurately determining $C_i$ (worst-case execution time) is one of the hardest problems in real-time systems. Sources of variation:

1. **Data-dependent paths.** Different inputs execute different code.
2. **Cache and memory hierarchy.** Cache hits vs misses cause orders of magnitude difference.
3. **Pipelining and branch prediction.** CPU state affects execution time.
4. **Interrupts.** Unexpected interrupts extend execution.

**Tools for WCET estimation:**

- **Static analysis tools** (aiT, Bound-T) compute WCET from binaries with models of the hardware.
- **Measurement-based analysis.** Run many test cases; take the max plus a safety margin. Unsafe for hard real-time.
- **Hybrid approaches.** Combine static analysis with measurements.

For hard real-time in safety-critical systems (DO-178C, ISO 26262), WCET must be proven, not just observed.

---

## Interview Questions

### Q1. When should you use RM vs EDF?

**Answer:** Use **RM (Rate Monotonic)** when: (1) you want fixed priorities and simple, predictable scheduling; (2) your RTOS supports only fixed-priority scheduling (most do); (3) you can accept the ~70% utilisation bound; (4) overload behaviour matters — RM degrades gracefully (the longest-period tasks miss first). Use **EDF** when: (1) you need high utilisation (close to 100%); (2) deadlines don't match periods; (3) your scheduler supports dynamic priorities. In practice, RM is far more common in embedded systems because the utilisation margin is usually tolerable and predictable behaviour under overload is valuable.

### Q2. Explain the Liu-Layland bound.

**Answer:** The Liu-Layland bound says: for a set of n periodic tasks with deadlines equal to periods, scheduled by Rate Monotonic, if total utilisation $U \leq n(2^{1/n} - 1)$, then all deadlines are met. As n grows, the bound approaches $\ln 2 \approx 0.693$. This is a **sufficient** (but not necessary) condition — tasks above the bound may still be schedulable. Use RTA for exact analysis.

### Q3. What is priority inversion and how do priority inheritance and priority ceiling solve it?

**Answer:** Priority inversion occurs when a high-priority task is blocked by a lower-priority task holding a resource it needs, and a medium-priority task preempts the low-priority task, effectively raising its own priority above the high-priority task's. **Priority inheritance**: when a high-priority task blocks on a mutex, the holder temporarily inherits the blocked task's priority, preventing medium-priority tasks from preempting. **Priority ceiling**: each mutex has a ceiling equal to the highest priority of any task that might use it; a task can only acquire a mutex if its priority exceeds all currently-held mutex ceilings. Ceiling protocols prevent deadlocks and bound blocking time analytically — preferred in safety-critical systems.

### Q4. Walk through RTA (Response Time Analysis) for a small task set.

**Answer:**

Given:
- $\tau_1$: C=1, T=4 (highest priority)
- $\tau_2$: C=2, T=6
- $\tau_3$: C=2, T=10 (lowest priority)

RTA for each:

$R_1 = C_1 = 1$ (no higher-priority tasks). Meets deadline.

$R_2 = C_2 + \lceil R_2/T_1 \rceil \cdot C_1$. Start with $R_2^0 = C_2 = 2$.
- $R_2^1 = 2 + \lceil 2/4 \rceil \cdot 1 = 2 + 1 = 3$.
- $R_2^2 = 2 + \lceil 3/4 \rceil \cdot 1 = 2 + 1 = 3$. Converged. $R_2 = 3 \leq 6$. OK.

$R_3 = C_3 + \lceil R_3/T_1 \rceil C_1 + \lceil R_3/T_2 \rceil C_2$.
- $R_3^0 = 2$.
- $R_3^1 = 2 + 1 \cdot 1 + 1 \cdot 2 = 5$.
- $R_3^2 = 2 + \lceil 5/4 \rceil \cdot 1 + \lceil 5/6 \rceil \cdot 2 = 2 + 2 + 2 = 6$.
- $R_3^3 = 2 + \lceil 6/4 \rceil \cdot 1 + \lceil 6/6 \rceil \cdot 2 = 2 + 2 + 2 = 6$. Converged. $R_3 = 6 \leq 10$. OK.

All tasks schedulable.

### Q5. What is WCET and why is it hard to determine?

**Answer:** **WCET (Worst-Case Execution Time)** is the maximum time a task can take to execute across all possible inputs and execution conditions. It's hard because: (1) **Data-dependent control flow.** Branches, loops, dispatch tables execute differently for different inputs. (2) **Cache effects.** A cache miss is often 10-100x slower than a hit. (3) **Pipelining and speculation.** Modern CPUs execute out-of-order; execution time depends on the microarchitectural state. (4) **Interrupts.** External events extend execution. (5) **Memory bus contention.** Especially in multi-core systems, DRAM access is contended. **Approaches:** (a) Static analysis tools that model the CPU. (b) Measurement-based with large safety margins (unsafe for hard real-time). (c) WCET-aware CPU architectures (PRET, time-predictable pipelines).

### Q6. Why is EDF's overload behaviour worse than RM's?

**Answer:** Under normal load, EDF is optimal. But when total utilisation exceeds 1 (overload), EDF's behaviour cascades — a missed deadline causes the next task's priority to drop (its deadline is now in the past), which causes more misses. Any task could miss any deadline. In RM, under overload, priorities are fixed, so the shortest-period (highest-priority) tasks always run. Only the longest-period tasks miss — graceful, predictable degradation. For real-world systems where transient overloads happen, RM is more robust. EDF's optimality is for the "just at or below 100%" regime.

### Q7. Explain how jitter affects schedulability analysis.

**Answer:** **Release jitter** is the variation in when a task actually becomes ready vs its nominal period. Jitter can make a task's release happen earlier (within limits). From the perspective of lower-priority tasks, an earlier release means they can be preempted sooner. In RTA, this is captured by adding $J_j$ (jitter of higher-priority task j) to each term:

$$
R_i = C_i + B_i + \sum_{j \in hp(i)} \left\lceil \frac{R_i + J_j}{T_j} \right\rceil C_j
$$

Release jitter effectively shortens the worst-case period. Large jitter degrades the achievable utilisation significantly. Hard real-time systems design to minimise jitter through hardware timers, dedicated scheduler cores, and careful interrupt management.

### Q8. What is "task phasing" and why does it matter?

**Answer:** **Phasing** refers to the offset of task releases relative to each other. In the worst case for analysis, all tasks release simultaneously at time 0 — the "critical instant". Analysing this worst case gives an upper bound on response times. If tasks are phased (released at staggered times), response times are often better than the critical-instant analysis suggests. However, it's difficult to guarantee phasing is maintained as the system evolves (e.g., after restart, when interrupts fire). Most analyses assume the critical instant and accept the pessimism.

### Q9. How does Rate Monotonic handle tasks with deadlines shorter than their periods?

**Answer:** RM assumes $D = T$. For $D < T$, use **Deadline Monotonic (DM)**: assign priorities based on deadline (shorter deadline = higher priority), not period. DM is optimal among fixed-priority schedulers for arbitrary deadlines with $D_i \leq T_i$. The utilisation bound analysis is similar but uses deadline-relative metrics. For $D_i > T_i$ (tasks can overlap their own next release), analysis is more complex — use frameworks like **arbitrary-deadline analysis** or schedulers like EDF.

### Q10. What is a sporadic task and how is it analysed?

**Answer:** A **sporadic task** releases jobs at irregular intervals, but with a known minimum inter-arrival time $T_i$. Unlike periodic tasks, the gap between releases can be longer, but never shorter. For schedulability purposes, sporadic tasks are treated identically to periodic tasks with period $T_i$ — the worst case is consecutive releases separated by exactly $T_i$. Used for event-triggered processing: network packets, button presses, sensor events with rate limits.

### Q11. What is a "resource-aware" extension to schedulability analysis?

**Answer:** Classical analysis assumes tasks only compete for the CPU. Real systems also have: (1) **Memory accesses.** Multiple tasks contending for DRAM. (2) **Shared caches.** One task's data evicts another's. (3) **Buses and peripherals.** I/O contention. **Resource-aware analysis** models these contentions explicitly, often by computing additional "blocking terms" for each shared resource. For multicore systems, this becomes essential — the assumption that CPU time is the only resource is untenable. Standards like AUTOSAR and ARINC 653 have frameworks for multi-resource real-time analysis.

### Q12. What is ARINC 653 and how does it relate to real-time scheduling?

**Answer:** **ARINC 653** is a standard for partitioned real-time operating systems in avionics. It defines:

1. **Time partitioning.** Each application has dedicated time windows in a cyclic schedule. No partition can steal time from another. Mitigates jitter and enables deterministic scheduling.
2. **Space partitioning.** Memory is isolated between partitions. Failures are contained.
3. **Hierarchical scheduling.** Within each partition, a local scheduler (often fixed-priority) handles tasks.

Used in Airbus A380, Boeing 787, and other certified avionics. The partitioning model enables strict **temporal and spatial isolation**, essential for mixed-criticality systems where a non-critical task cannot affect a critical one.

### Q13. How does multicore change real-time scheduling?

**Answer:** Single-core real-time theory doesn't directly extend. Multicore issues:

1. **Partitioned vs. global scheduling.** Partitioned: each task is pinned to a core; use single-core analysis per core. Simple but may waste capacity. Global: tasks migrate freely; higher utilisation but harder analysis.
2. **Cache effects.** Task migration invalidates cache — migration cost is real.
3. **Shared resources.** LLC, memory controllers, interconnects all become contended.
4. **Interference analysis.** The worst-case effect of one core on another must be modelled.

**Dhall's effect:** some task sets are schedulable on single core but not on multicore, despite capacity. Demonstrates that multicore scheduling is inherently harder.

**Industry trend:** conservative partitioning with ample slack. High-integrity avionics typically use one partition per core with strict isolation.

### Q14. What is "mixed-criticality" and why does it matter?

**Answer:** **Mixed-criticality** systems run tasks of different criticalities on the same hardware — e.g., flight-critical controllers alongside infotainment. Key problem: how to ensure high-criticality tasks meet deadlines even in the presence of lower-criticality tasks.

**Vestal's model:** each task has multiple WCETs — one for each criticality level. In "normal" mode, low-criticality tasks run with their normal WCETs. If a critical task overruns its normal WCET, the system switches to "high" mode where low-criticality tasks are suspended or given reduced budgets.

Why it matters:

1. **Cost.** One platform for multiple functions is cheaper than separate ones.
2. **Certification.** High-criticality code is expensive to certify; mixing allows isolation without full re-certification of low-criticality code.
3. **Research area.** Active academic work, some industrial adoption (automotive).

### Q15. How do you verify schedulability in a certified real-time system?

**Answer:** For safety-critical systems (DO-178C, ISO 26262), you must **prove** schedulability with evidence:

1. **Static analysis of WCET.** Tools like aiT, Bound-T analyse the binary against a hardware model. Produces a safe upper bound.
2. **RTA or EDF analysis.** Plug WCETs into the equations; verify all deadlines met.
3. **Stress testing.** Run the system under worst-case synthetic loads; measure response times.
4. **Independent review.** A separate team reviews the analysis and assumptions.
5. **Documentation.** Every assumption, tool version, and result is recorded for audit.

For ASIL-D (ISO 26262 highest) or DO-178C Level A, the effort is substantial. Timing analysis is often the hardest part of certification — more than functional correctness.

---

## Interview Tips

- **Be concrete.** Real-time questions often come with task sets. Show you can compute utilisation, apply Liu-Layland, and do RTA.
- **Know the limits.** The Liu-Layland bound is a sufficient condition, not a necessary one. Many task sets above the bound are still schedulable.
- **Priority inversion is a classic.** Be ready to describe priority inheritance and the Mars Pathfinder anecdote.
- **Modern challenges.** Multicore, mixed-criticality, WCET determination on modern CPUs — these are hot topics.
- **Know your RTOS.** FreeRTOS, Zephyr, VxWorks, QNX — each has different scheduler policies and inheritance support.

Real-time scheduling theory is the analytical foundation for safety-critical embedded software. Engineers working in aerospace, automotive, and medical devices are expected to reason about it rigorously. Understanding it is a mark of seriousness in the embedded domain.
