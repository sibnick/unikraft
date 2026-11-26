# uksched: thread placement and CPU affinity contract

This document states what the scheduler *does* with a thread's CPU, so
that applications can rely on it and nothing arrives as a surprise.
It describes the behavior of `lib/uksched` with `lib/ukschedcoop`,
which is the configuration of the multicore KVM/x86 images
(`CONFIG_HAVE_SMP=y`).

## The contract

**A thread runs on the CPU of the scheduler that created it, from
creation until an explicit affinity operation moves it. The scheduler
never rebalances.**

There is no load balancing, no idle stealing, and no periodic
re-placement. "Allowed CPUs" and "placed CPU" are different things:

- The per-thread **affinity mask** is a permission set. The pick path
  refuses to run a thread on a CPU its mask excludes.
- The **placement** is where the thread's queue lives. Nothing spreads
  threads across CPUs. A thread whose mask allows every CPU still runs
  only on its creator's CPU.

## Placement at creation

| Creation path | Initial CPU |
|---|---|
| `uk_sched_thread_add(s, t)` / `uk_sched_thread_create_*()` in image | the passed scheduler `s` — caller intent, always honored |
| posix `clone()` / `pthread_create()` (`lib/posix-process/clone.c`) | the creator's CPU (`uk_sched_current()`) |
| `ukboot` init/main | the boot CPU's scheduler |
| `uk_secondary_entry()` per-CPU init (`lib/ukboot`) | that secondary CPU's own scheduler |

An application's thread tree therefore grows on one CPU unless the
application pins. This is intentional (see *Why* below), but it has a
visible consequence for generic runtimes such as the JVM: HotSpot does
not set affinity on its own threads on Linux — it relies on the kernel
balancer. Here it must do the distribution itself.

## Explicit affinity is a hard pin

The following calls *move* a thread (Linux errnos, full tid resolution):

- `uk_sched_thread_setaffinity()` — the uksched API
- `sched_setaffinity(2)` / `sched_getaffinity(2)` — `lib/posix-process`
- `pthread_setaffinity_np()`, `pthread_attr_setaffinity_np()` and the
  `get` variants — provided by `lib/posix-thread`, because musl
  implements none of them; see `lib/posix-thread/README.md`

Mechanics, for completeness:

- A new mask that excludes the CPU the thread is queued on rehomes the
  thread to a started scheduler on an allowed, online CPU. The choice
  is the first eligible scheduler in registration order — deterministic,
  not load-aware.
- A thread *running* on a now-forbidden CPU parks itself on that CPU's
  sleep queue (hand-off) and completes its move through the
  destination CPU's lock-free inbox.
- Queued threads are never touched by a foreign CPU; cross-CPU delivery
  only ever pushes not-yet-queued threads to an inbox, and the owner
  CPU drains it in its own schedule path.
- A destination CPU that is halted takes the thread in at the next tick
  or by its capped idle halt: **placement latency is bounded by 10 ms**
  (`UK_SCHEDCOOP_IDLE_POLL_NS`). No IPI kick is sent: the current KVM
  x86 path can deliver a RUN IRQ twice for one send.
- A mask that excludes *every* started/online CPU is rejected with
  `-EINVAL`; nothing is moved.
- `sched_getaffinity(2)` reports the thread's true mask. An unpinned
  thread reports all online CPUs, which is what makes JVM CPU
  enumeration work without `-XX:ActiveProcessorCount`.

## Why no load balancing

The multicore design point is run-to-completion per core: per-core
allocator partitions (`CONFIG_LIBUKBOOT_PERCORE_HEAP`), per-core lwIP
memp pools, and one ENA MSI-X queue group per vCPU all assume that a
datapath thread stays on its core. A partition allocator has a single
owner and no lock; moving a thread that allocated from one and frees on
another corrupts it. Sticky placement makes that assumption structural
instead of depending on a balancer that applications must then fight to
opt out of. Threads that must live on a known CPU — every datapath
thread — are pinned explicitly, and the pin is honored absolutely: the
pick path never moves an allowed thread, only forbidden ones.

## Guidance for JVM-style workloads

The JVM gets one CPU by default; use the affinity calls to distribute
it:

- Workers of a per-core service: `pthread_attr_setaffinity_np()` on the
  `pthread_attr_t` before `pthread_create()`. The application-side shim
  (`lib/posix-thread/compat/pthread_affinity_musl.c`, linked with
  `-Wl,--wrap=pthread_create`) applies the attribute affinity before the
  start routine runs, so the worker is born on, or moves
  immediately-and-once to, the service core.
- Already-running threads: `sched_setaffinity(2)` with the tid.
- Expect up to 10 ms before a move lands on an idle target core.

## Known deviations from Linux

Two, stated here because both interact with placement:

1. **Affinity is not inherited.** On Linux, a child of a pinned thread
   starts with the parent's mask. Here the clone path gives the child
   the default all-online mask. The thread still *starts* on the
   parent's CPU (placement), and stays there until pinned, so in
   practice the observable difference is what `sched_getaffinity`
   reports for the child: all CPUs, not the parent's set.
2. **Placement latency after a pin**, on an otherwise idle target CPU,
   is bounded by the 10 ms idle-halt cap instead of being IPI-driven
   (see mechanics above).

## Where this lives in code

- Mask storage and the move machinery: `lib/uksched/affinity.c`
  (`uk_sched_thread_setaffinity()`, `uk_sched_thread_rehome_dequeued()`,
  `uk_sched_thread_migrate_handoff()`), `include/uk/thread.h`
  (`uk_thread_affinity_allows()`).
- Affinity-aware pick and inbox drain: `lib/ukschedcoop/schedcoop.c`.
- Tests: `lib/uksched/tests/test_affinity.c` (uktest, SMP + UP).

A future load-aware *creation* placement is under consideration; if it
lands it will apply to unaffined posix threads only, and explicit pins
will keep the absolute precedence documented here.