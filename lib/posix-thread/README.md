# posix-thread: pthread CPU affinity for musl applications

musl libc implements **none** of the non-portable pthread affinity
calls (`pthread_setaffinity_np`, `pthread_getaffinity_np`,
`pthread_attr_setaffinity_np`, `pthread_attr_getaffinity_np`). This
library fills that gap on top of the per-thread CPU affinity provided by
`lib/uksched` and the `sched_setaffinity(2)`/`sched_getaffinity(2)`
syscalls implemented by `lib/posix-process`.

## Two integration styles

### 1. In-kernel helpers (`CONFIG_LIBPOSIX_THREAD=y`)

`pthread_affinity.c` exposes kernel-side helpers (exported as
`uk_pthread_*`):

- `uk_pthread_setaffinity_np()` / `uk_pthread_getaffinity_np()` resolve a
  musl `pthread_t` (the thread control block address) by scanning the
  scheduler thread lists for the recorded TLS pointer (`uk_thread.tlsp`),
  verifying musl's self-pointer back-reference (x86_64 `TLS_ABOVE_TP`:
  `*(void **)(tp - 16) == tp - 16`), then apply/read the affinity on the
  resolved `uk_thread`.
- `uk_pthread_attr_*()` store the attribute cpuset in a small side
  registry keyed by the attribute object address, because musl's
  `pthread_attr_t` has no room for a cpuset.

Use these from other in-tree libraries or platform glue.

### 2. Application-side shim (musl static binaries, e.g. app-elfloader images)

A musl-linked application cannot call kernel functions directly, so
compile `compat/pthread_affinity_musl.c` into the application itself.
It provides the four `pthread_*_np` calls with musl-compatible return
codes (0 / positive errno) on top of plain libc `sched_setaffinity(2)` /
`sched_getaffinity(2)` syscalls, which the unikernel implements with
full tid resolution.

Integration into the app build:

1. Compile `compat/pthread_affinity_musl.c` into the image. Because it
   provides symbols musl does not define, they resolve automatically.
2. To make `pthread_attr_setaffinity_np()` actually take effect, link
   the app with `-Wl,--wrap=pthread_create`: every `pthread_create()`
   call then lands in the shim, which records the new thread's tid
   (published from inside the thread via `SYS_gettid`) and applies the
   attribute cpuset before the application's start routine runs.

```make
APP_LDGROUNDS_GCCLIBS := -Wl,--wrap=pthread_create
```

(or the equivalent in your app build system, e.g. Kraftfile
`targets[].kconfig` + `ldflags`).

## Behavior and caveats

- `pthread_t -> tid` mapping: the shim's `pthread_create` wrapper is the
  only path that registers handles. Threads created before the shim is
  linked in, or through paths that bypass `pthread_create`, resolve to
  `ESRCH`.
- The wrapper waits (bounded, yielding) for the new thread's tid to be
  published before resolving handles for it; on a cooperative scheduler
  the child publishes it at its first scheduling point.
- Slots are reclaimed when a wrapped thread's start routine returns;
  threads that never return keep their slot (bounded by
  `MAX_TRACKED_THREADS`, 512).
- Attribute entries are consumed on use; a destroyed-but-unused
  attribute leaves a stale entry until its address is reused. This
  matches fresh-object semantics for the recycled address.
- cpuset size: the shim requires `cpusetsize == sizeof(cpu_set_t)` for
  the attribute calls (musl apps always pass that); the thread calls
  accept any non-zero size and the kernel applies Linux `cpusetsize`
  semantics (short read/write, tail-bit validation).
- The kernel-side resolution assumes the musl x86_64 TLS layout; on
  other architectures `uk_pthread_resolve()` returns `NULL` and callers
  get `ESRCH`.

## Related

The placement semantics the affinity calls interact with — sticky
creation placement, pins as hard affinity, the 10 ms placement bound on
idle targets — are documented in `lib/uksched/README.md`.
