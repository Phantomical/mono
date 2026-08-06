# Triage of the 32 `check-all` failures at 85e7ebac6ee

The sweep ran `ctest -j18 -LE "slow|stress|acceptance"` at `85e7ebac6ee` in
`/home/swlynch/projects/mono/.claude/worktrees/llvm-opt-flag`, took 3653 s and
left 32 failures. Its `LastTest.log` is preserved at
`.claude/scratch/sweep-triage/LastTest-85e7ebac6ee.log` (the live copy in
`build/Testing/Temporary/` has since been overwritten by re-runs).

Everything re-run below was re-run at `ade7ef67037`, the branch tip at the time
of writing, after a full rebuild. Nothing between `85e7ebac6ee` and that tip
touches EH side tables, `jinfo`, `finally-range` or `mini-exceptions.c`, so the
one EH finding here is unaffected by the move.

The machine was busy throughout with other agents' builds and test runs. Every
wall-clock number below is therefore pessimistic; where a number is load-bearing
it is a ratio against the system mono measured in the same command, not an
absolute.

## Counts

| bucket | n | tests |
| --- | --- | --- |
| (a) genuine regression | 12 | 178, 241, 378, 432, 903, 1293, 3171, 3173, 3176, 3185, 3194, 3203 |
| (b) stale by construction | 2 | 456, 457 |
| (c) environmental / pre-existing floor | 6 | 183, 192, 206, 207, 208, 232 |
| (d) load artifact or flake | 8 | 187, 234, 369, 408, 1880, 1961, 2811, 3192 |
| unresolved — see the last section | 4 | 172, 188, 219, 226 |

Three of the twelve in (a) are independent defects. The other nine share one
root cause: the JIT takes so long to compile that tests with a short timing
budget lose their race. They are listed separately because each needs its own
verification, but one fix plausibly clears all nine.

---

## (a) Genuine regressions

### 1293 `runtime/finally_block_ending_in_dead_bb@sgen` — abort delivered inside a finally

The most serious finding here, and fully diagnosed.

**Repro**

```
cd build/mono/tests
MONO_PATH=<tree>/build/mcs/class/lib/net_4_x \
MONO_CFG_DIR=<tree>/build/runtime/etc \
  <tree>/build/mono/mini/mono-sgen finally_block_ending_in_dead_bb.exe
```

Fails roughly one run in seven, on **both** collectors — the sweep's
"sgen only" was luck:

| runtime | result |
| --- | --- |
| in-tree JIT, sgen, 25 idle runs | 22 pass / 3 exit 4 |
| in-tree JIT, boehm, 25 idle runs | 21 pass / 4 exit 4 |
| in-tree JIT, sgen, 24 concurrent | 22 pass / 2 exit 4 |
| in-tree JIT, boehm, 24 concurrent | 21 pass / 3 exit 4 |
| in-tree **interpreter**, 40 runs | 40 pass |
| **system mono 6.8 (classic JIT)**, 40 runs | 40 pass |

The same `.exe` is 100 % clean on the classic JIT and on our own interpreter, so
this is the LLVM back end's own EH, not a race in the test.

**What actually happens.** The test aborts a thread that is spinning inside a
`finally`:

```csharp
finally {
    res = 4;
    while (!foo);          // main sets foo right after t.Abort ()
    Thread.ResetAbort ();
    res = 0;
}
```

An abort requested while a thread is inside a finally must be *delayed* until
the finally exits. An instrumented copy of the test
(`.claude/scratch/sweep-triage/abortprobe.cs`) shows it is not: on a failing run
the thread catches a `System.Threading.ThreadAbortException` and the print
placed immediately after the spin loop never executes. The abort is delivered
while the IP is still inside the finally body, `ResetAbort` never runs, `res`
stays 4.

**Root cause.** The async stack walk that installs the guard returns **zero
frames**, so the guard is never even considered. `async_abort_critical ()`
(`mono/metadata/threads.c`) asks `mono_install_handler_block_guard ()` first;
that walks the suspended thread's stack looking for a finally it is inside. On
every failing run the walk visits no frame at all — instrumentation on the
suspend state reports `last_managed=(none)`, `running_managed=0`, the LMF still
at its root slot in `jit_tls`, and no `MonoJitInfo` for the suspended IP. With
no `ji` and no LMF the walk has nothing to start from and stops at frame zero.
The abort is then merely marked pending and delivered at the next interruption
checkpoint, which lands inside the finally: `ResetAbort` never runs and `res`
stays 4.

Dumping 32 bytes around the suspended IP identifies where the thread is stopped
(evidence in `.claude/scratch/finally-guard-range/`):

* `ff 25 xx ff ff ff` in a run of the same — a **JITLink PLT/GOT stub**
  (`jmp *disp(%rip)`), which no `MonoJitInfo` covers.
* `48 c7 c0 c8 ff ff ff / 48 8b 04 02 / c3` inside `mono-sgen` — the `ret` of a
  small unnamed native TLS getter, reached from managed code **before** the
  wrapper's prologue has pushed an LMF.

What makes that window wide enough to hit ~10 % of runs is codegen: the
translator emits a `mono_generic_class_init` icall on **every** static-field
access, so `while (!foo);` calls out through a stub into a wrapper on every
iteration. The classic JIT emits that check once per method, which is why
system mono is 40/40 clean; the interpreter generates no stubs at all. The same
icall is also what puts an interruption checkpoint *inside* the finally, which
is where the pending abort gets taken.

**Not the cause, though real.** `finally-range.cpp` used to close each block's
guard range before the block's terminator, leaving the spin loop's back-edge in
no range (`je .LBB0_3` sitting between `.Lmono_finally_end1` and
`.Lmono_finally_begin2` in a `MONO_LLVM_JIT_ASM` dump). That hole is genuine and
is fixed, but it is a couple of bytes of a loop dominated by a call, and closing
it did not move the failure rate.

**Ruled out:** the collector (fails on both), thread-start timing (an
instrumented copy that aborts only after main has observed the thread inside the
finally still fails with the same zero-frame signature), and `ResetAbort`
throwing `ThreadStateException` (the caught exception is `ThreadAbortException`).

**What it needs.** Either code with no `MonoJitInfo` that managed code can be
stopped in — JITLink's stubs, wrapper prologues before the LMF push — has to
become walkable, or the redundant class-init calls have to go so the loop
contains no call at all. Both are their own tasks.

### 903 `runtime/pinvoke-detach-1@sgen` — genuine intermittent hang

The sweep killed this at 300 s. It normally runs in 22–24 s, and a test that
takes 22 s does not need 300 s because the machine is loaded — so it was never
plausibly starvation.

**Reproduced at the tip.** 30 runs through `build/runtime/mono-wrapper`
(15 per collector, `timeout -s KILL 90`): 29 completed in 22–24 s; the 30th
(boehm, run 15) was still running at 90 s and had to be killed.

```
cd build/mono/tests
MONO_PATH=<tree>/build/mcs/class/lib/net_4_x \
MONO_CONFIG=$PWD/tests-config \
MONO_EXECUTABLE=<tree>/build/mono/mini/mono-boehm \
  timeout -s KILL 90 <tree>/build/runtime/mono-wrapper pinvoke-detach-1.exe
```

The sweep's SIGQUIT thread dump names where it is stuck:

```
"<unnamed thread>"  at <unknown> <0xffffffff>
  at (wrapper managed-to-native) Tests.mono_test_attach_invoke_block_foreign_thread (...)
  at Tests.test_0_attach_invoke_block_foreign_thread_delegate ()
```

i.e. a foreign thread attached and blocked inside the pinvoke never gets
released. Note that even the passing runs print
`abort_threads: Failed aborting id: 0x...., mono_thread_manage will ignore it`,
which is the same message that shows up in `bcl-System.Core` and
`threadpool-exceptions5` — likely the same shutdown machinery.

### 241 `bcl-corlib` — shutdown hang after the suite has finished

The sweep reports this as a 1800 s timeout, which reads like "needs a bigger
budget". It is not.

The last line the suite printed is
`Results saved as .../TestResult-net_4_x-corlib.xml`, which nunit-lite emits at
the very end of `Main`. That file's mtime is **18:10:44**; the test started at
17:55 and ctest killed it at ~18:25. So the tests themselves took about 895 s
and the process then sat for roughly **15 minutes without exiting**.

By contrast `TestResult-net_4_x-System.xml` was written 18:05:29 and
`bcl-System` was recorded as ending at 18:05 — that suite exited normally.

Raising the budget will not help this one. The 11 test failures inside the run
(`TimeZoneTest.GetUtcOffsetAtDSTBoundary` and friends) are the usual tzdata
floor and are not the reason it failed.

**Not settled:** whether the hang is in the same place as `pinvoke-detach-1`'s.
Both would need a thread dump from a hung process to compare.

### The compile-latency family

Nine failures — 378, 432, 178 and the six unhandled-exception cases — all have
the same shape: the test gives something between 100 ms and 1000 ms to happen,
the classic JIT does it in single-digit milliseconds, and this JIT does not.
Measured on the tip, alone:

| measurement | in-tree | system mono | ratio |
| --- | --- | --- | --- |
| `test-async-20.exe` wall (8195 methods compiled) | 7.95 s | 0.27 s | 29× |
| time to reach `mre.Set ()` in the linker task test | 334 ms | 3 ms | 110× |
| user CPU of the profiler's `idle-sleep` workload | 0.746 s | 0.024 s | 31× |

Each of the nine is listed with its own repro because a fix agent will want to
verify them individually, but they should be treated as one problem.

**378 `compiler-tests/23`** — `mcs/tests/test-async-20.cs`. Deterministic:
10/10 failures at the tip, 1–2 of its 4 sub-tests reporting
`FAILED (Timeout)` against a `Task.WaitAll (..., 1000)`. System mono: 3/3 clean.
The test uses `dynamic`, so it drags the whole DLR binder through the JIT.

```
mcs -debug -d:NET_4_0 -d:NET_4_5 -lib:build/mcs/class/lib/net_4_x-linux \
    -out:test-async-20.exe mcs/tests/test-async-20.cs
MONO_PATH=build/mcs/class/lib/net_4_x-linux MONO_CFG_DIR=build/runtime/etc \
  build/mono/mini/mono-sgen --debug test-async-20.exe
```

**432 `linker-mscorlib-test-task-01`** — deterministic, 10/10 at the tip, both
on the linked output and on the unlinked `.exe`; system mono 5/5 clean. The
program returns 1 when `contSuccess.Wait (100)` does not complete. A probe
(`.claude/scratch/sweep-triage/task01probe.cs`) shows the continuation *does*
complete — at ~900 ms — and that `Wait (100)` itself blocks for ~560 ms of wall
time while its own machinery is compiled.

```
cd build/mcs/tools/linker/illink-output/mscorlib-test-task-01
MONO_PATH=. MONO_CFG_DIR=<tree>/build/runtime/etc \
  <tree>/build/mono/mini/mono-sgen test-task-01.exe   # exits 1
```

**178 `bcl-xunit-Mono.Profiler.Log`** — `ProcessTimeSamplingWorks` asserts that
an idle-sleeping process produces fewer than 100 process-time samples. It
produced 447 in the sweep and 5565 standalone at the tip. Reproduces in ~10 s:

```
cd mcs/class/Mono.Profiler.Log
MONO_PATH=<tree>/build/mcs/class/lib/net_4_x-linux \
  <tree>/build/runtime/mono-wrapper --debug \
  external/xunit-binaries/xunit.console.exe \
  build/mcs/class/lib/net_4_x-linux/tests/net_4_x_Mono.Profiler.Log_xunit-test.dll \
  -noappdomain -noshadow -parallel none \
  -method 'MonoTests.Mono.Profiler.Log.ProfilerTests.ProcessTimeSamplingWorks'
```

The workload sleeps on four threads and does nothing else, so every sample is
CPU this runtime burned that the classic one did not.

**3171 / 3173 / 3176 / 3185 / 3194 / 3203 — the unhandled-exception family.**
These were on the "known flake, do not chase" list. They are not clean, and the
brief's advice to just confirm them was wrong. 15 runs of each configuration at
the tip, through `build/runtime/mono-wrapper`:

| test | env | result (expected) |
| --- | --- | --- |
| `unhandled-exception-1.exe` @sgen | `TEST_UNHANDLED_EXCEPTION_HANDLER=1` | 15/15 correct (1) |
| `unhandled-exception-1.exe` @boehm | `TEST_UNHANDLED_EXCEPTION_HANDLER=1` | **11/15** — 4 exited 0 |
| `unhandled-exception-3.exe` @sgen | no handler | **7/15** — 8 exited 0 |
| `unhandled-exception-3.exe` @boehm | no handler | 15/15 correct (255) |
| `unhandled-exception-3.exe` @sgen | handler | 15/15 correct |
| `unhandled-exception-3.exe` @boehm | handler | 15/15 correct |
| `unhandled-exception-3.exe`, **system mono**, 20 runs | no handler | 20/20 correct (255) |

The exception is not lost. `unhandled-exception-3` gives the threadpool worker
exactly 1000 ms after the `finally` runs to finish unwinding and abort the
process, then exits 0 itself. A probe that extends that to 30 s
(`.claude/scratch/sweep-triage/ue3probe.cs`) aborts with 255 in 8/8 runs, always
within the first second — i.e. the runtime does get there, but it needs most or
all of the test's whole budget, so main wins the race about half the time. On
system mono the abort is effectively instant.

Repro for the worst configuration:

```
cd build/mono/tests
MONO_PATH=<tree>/build/mcs/class/lib/net_4_x MONO_CONFIG=$PWD/tests-config \
MONO_EXECUTABLE=<tree>/build/mono/mini/mono-sgen \
  <tree>/build/runtime/mono-wrapper unhandled-exception-3.exe; echo $?
# want 255; exits 0 about half the time
```

---

## (b) Stale by construction

### 456 `symbolicate-with_aot`, 457 `symbolicate-with_aot_msym`

Confirmed from the log rather than assumed. Both die on:

```
--aot: ahead-of-time compilation is not supported by this runtime.
CMake Error at mcs/tools/mono-symbolicate/symbolicate-test.cmake:28 (message):
  failed (1): ... mono-wrapper;-O=-inline;--aot;.../StackTraceDumper.exe
```

The AOT compiler is out of scope and refuses immediately, so these two can never
pass again. They should be dropped from `mcs/tools/mono-symbolicate/`'s CTest
registration. The four sibling `symbolicate-*` cases that do not pass `--aot`
pass and should stay.

---

## (c) Environmental / pre-existing floor

Each of these was read from the assertion text in the log, not guessed from the
suite name.

### 192 `bcl-System.Data.Linq` — a worktree-path-length artifact, and a nice one

One failure, `DataContextTest.Ctor_FileOrServerOrConnectionIsFilename`:

```
System.ArgumentException : The value's length for key 'data source' exceeds
it's limit of '128'.
```

The test passes `typeof (DataContextTest).Assembly.Location` as the connection
string. In this worktree that path is **133** characters; in the main tree it is
**101**. The limit is 128. It fails here purely because the worktree lives under
`.claude/worktrees/llvm-opt-flag/` and would pass in
`/home/swlynch/projects/mono/build`. Nothing to fix in the runtime.

### 183 `bcl-System`

6401 run, 7 failures. Five are `System.Net.WebException : Error:
NameResolutionFailure` / `No such host is known` — no DNS. The other two are
timing rather than network: `TestTimeoutPropertyWithServerThatExistsAndResponds\
ButTooLate` ("Timeout exception ... was at least half-second late") and
`UploadDataAsyncCancelEvent`. Those two are the same 500 ms-budget shape as the
(a) latency family and may well clear with it; they are not on their own worth a
task.

### 206 `bcl-System.Messaging`

58 failures, all
`RabbitMQ.Client.Exceptions.BrokerUnreachableException : None of the specified
endpoints were reachable`. No broker on this machine.

### 207 `bcl-System.Net.Http`, 208 `bcl-System.Net.Http.WebRequest`

One failure each, both `HttpClientTest.Proxy_Disabled`, both `No such host is
known` / `NameResolutionFailure`. DNS.

### 232 `bcl-System.Windows.Forms`

```
Gtk not found (missing LD_LIBRARY_PATH to libgtk-x11-2.0.so.0?)
X connection to :0 broken (explicit kill or server shutdown).
```

The X server went away mid-run. No test failure was recorded at all — the
process died.

---

## (d) Load artifact or flake

### 1880 `runtime/block_guard_restore_alignment_on_exit@boehm`

Flagged in the brief as one of the two high-suspicion EH cases. It is not the
same problem as 1293.

The failure output is `What now?` and nothing else, and exit code 1 — meaning
`res` never left its initial value, i.e. `Func` never ran at all. That is only
reachable if `t.Abort ()` landed before the thread began executing the delegate,
which needs the thread to take longer than the test's 100 ms head start to
start.

**Not reproduced:** 74 runs at the tip — 25 idle per collector and 24
concurrent per collector — all passed, on both collectors. The concurrent runs
were specifically an attempt to induce the start delay and did not.

Classified (d) on the failure mode plus 74 clean runs, but stated plainly: the
sweep's failure was not reproduced, so this rests on the mechanism argument
rather than on a demonstration.

### 408 `compiler-tests/53` — a SIGSEGV that did not come back

The sweep recorded `Subprocess aborted` with

```
Got a SIGSEGV while executing native code.
```

after 74 s, and `build/mcs/tests/net_4_x-53.log` is 0 bytes, so nothing had been
written yet. Re-run at the tip **5 times**: passed every time, in 33–58 s.

A native crash is the most serious kind of failure in this list and one clean
set of re-runs is not proof it is gone. Recorded as (d) because it did not
reproduce, but flagged: if it appears again in any later sweep it should be
treated as (a) immediately, and it is worth running the shard under a core-dump
setting next time rather than re-running it bare.

### 369 `compiler-tests/14`

`mcs/tests/test-async-10.cs` failed with "Wrong return code: 1", which is
exactly the first `Task.WaitAll (new[] { t1 }, 1000)` in that program timing
out. At the tip, alone, it passes **20/20** and the whole program takes 1.43 s
wall.

So it is (d) — but the margin is thin, and it is the same underlying problem as
378. If the latency work in (a) lands, this stops being marginal.

### 187 `bcl-System.Core`

One failure out of 1596:
`ReaderWriterLockSlimTests.EnterWriteLockWhileInUpgradeAndOtherWaiting`,
`Expected: True / But was: False` — a lock-handoff timing assertion. At the tip
the whole `ReaderWriterLockSlimTests` fixture passes 36/36 in 4 s, and the single
failing test passes 10/10.

```
cd mcs/class/System.Core
MONO_PATH=<tree>/build/mcs/class/lib/net_4_x-linux \
  <tree>/build/runtime/mono-wrapper --debug \
  build/mcs/class/lib/net_4_x-linux/tests/runner/System.Core/nunit-lite-console.exe \
  build/mcs/class/lib/net_4_x-linux/tests/net_4_x_System.Core_test.dll \
  -exclude=NotWorking,CAS \
  -test:MonoTests.System.Threading.ReaderWriterLockSlimTests
```

### 3192 `runtime-unhandled-exception-255-without-managed-handler/threadpool-exceptions5@boehm`

Failed with exit 2, which in that program means `mre.WaitOne (10000)` expired —
a 10 s budget, an order of magnitude looser than its siblings'. 16 runs at the
tip (8 per collector) all returned the expected 255. (d), load.

Note it is not in the (a) latency group precisely because its budget is 10 s,
not 1 s.

### 1961 `runtime/appdomain-threadpool-unload@boehm`, 2811 `runtime-interp/appdomain-threadpool-unload@sgen`

Both were **killed at 300 s** by the `mono/tests` driver budget
(`TEST_DRIVER_TIMEOUT_SEC=300`), not failed. Both had made progress — the log
shows dots and then a SIGQUIT thread dump with live `Thread Pool Worker` frames
in `System.Runtime.Serialization`. Measured elsewhere at 137/140/227/266 s, so
300 s is simply too tight for them under a loaded 18-way run. Their budgets have
since been raised to 900 s on the tip. Not re-run here (each costs minutes).

"Exceeds its budget under load" rather than "flaky": there is a fixed, already
applied fix.

### 234 `bcl-System.Xml`

Killed at 1800 s, but with output still streaming at the kill — it was making
progress, not stuck. Measured elsewhere at 224 s idle and 1541 s loaded, and its
budget has since been raised to 3600 s. Not re-run (30 min).

---

## Unresolved

These four could not be settled without a run of 30 minutes or more, which was
out of scope. Stating what is known and what is not.

### 172 `bcl-Mono.Debugger.Soft`, 219 `bcl-System.ServiceModel` — never observed to complete anywhere

Both stopped at exactly 1800.0 s. Both printed the NUnitLite banner and then
**not one further character** — nunit-lite prints a `.` per test, and there are
zero dots in thirty minutes. Neither has a recorded completion in any build tree
on this machine.

Corroborating: `build/mcs/class/lib/net_4_x-linux/tests/TestResult-net_4_x-System.ServiceModel.xml`
does not exist at all, and `TestResult-net_4_x-Mono.Debugger.Soft.xml` exists but
is dated 15:05, hours before this sweep's 17:26 start — so this run wrote
nothing.

A suite that produces zero output for 1800 s is much more consistent with a hang
before or during the first test than with being merely enormous, but that is an
inference from silence. **Needs a long run (or a thread dump from the hung
process) to confirm.** Do not raise their budgets on the assumption that they
are just slow.

`Mono.Debugger.Soft` spawns a debuggee runtime and attaches the soft debugger to
it; `System.ServiceModel` opens network listeners. Both have obvious places to
hang.

### 188 `bcl-xunit-System.Core` — never completes, and the failures it did record are ambiguous

Killed at 1800 s, but unlike the two above it was progressing — 42 `[FAIL]`
lines were recorded before the kill. Every one of them is in
`System.Linq.Expressions.Tests` with `useInterpreter: True`, and 38 of the 42 are
out-of-range floating-point conversions, e.g.

```
ConvertFloatToUShortTest(useInterpreter: True)
  Expected: 65535   Actual: 0
ConvertFloatToIntTest(useInterpreter: True)
  Expected: -2147483648   Actual: 0
```

Partially investigated with a standalone probe
(`.claude/scratch/sweep-triage/linqconv.cs`):

* Most of these (float→ushort, double→ulong, double→byte) give **0 on system
  mono too**, so those are the historical mono-vs-corefx expectation gap, not
  ours.
* `Expression.Convert (float.MaxValue, typeof (int))` is different: system mono
  returns `-2147483648`, this tree returns **0**, both with `Compile (true)` and
  `Compile (false)`.
* A direct `conv.i4` is *not* affected — `.claude/scratch/sweep-triage/convprobe.cs`
  gives `-2147483648` on our JIT, our interpreter and system mono alike. (One
  unrelated divergence surfaced there: `(ulong) double.MaxValue` is
  `9223372036854775808` on our JIT and `0` on both the interpreter and system
  mono.)

I could not separate runtime from class library on that one: system mono 6.8
ships a different `System.Linq.Expressions`, and pointing it at our
`net_4_x-linux` fails with "Corlib not in sync with this runtime". So the
`Expression.Convert` divergence may be ours or may be a class-library difference.
**Needs a long run and a runtime-only A/B to settle**, plus a count of how many
of the 42 also fail on the classic JIT.

### 226 `bcl-System.Web` — one cascading root cause, first failure unknown

396 of the 421 recorded failures are the identical
`System.Runtime.Remoting.RemotingException : No receiver for uri
<guid>/<id>.rem`, all naming the *same* uri. That is one broken object, not 396
independent failures.

`MonoTests.System.Web.Configuration.GlobalizationSectionTest`, one of the
affected fixtures, passes 4/4 standalone at the tip (86 s):

```
cd mcs/class/System.Web
MONO_PATH=<tree>/build/mcs/class/lib/net_4_x-linux \
  <tree>/build/runtime/mono-wrapper --debug \
  build/mcs/class/lib/net_4_x-linux/tests/runner/System.Web/nunit-lite-console.exe \
  build/mcs/class/lib/net_4_x-linux/tests/net_4_x_System.Web_test.dll \
  -exclude=NotWorking,CAS \
  -test:MonoTests.System.Web.Configuration.GlobalizationSectionTest
```

The most likely mechanism, unconfirmed: the suite's cross-AppDomain host,
`mcs/class/System.Web/Test/mainsoft/NunitWeb/NunitWeb/MyHost.cs`, is a
`MarshalByRefObject` that does not override `InitializeLifetimeService`, so it
gets the default **five-minute** remoting lease. The sweep's run took **26
minutes**. Once the lease expires every later test that touches the host gets
exactly this exception. If that is right, it is the same slowness story as the
(a) latency family rather than a remoting bug — but I have not shown it.

**Needs a long run** with `-labels` to identify the first failing test and
whether the cutover happens around the five-minute mark.

---

## Deferred as too slow to check

For the record, so nobody assumes these were verified:

* Every BCL suite was classified from the sweep's own output. Only three single
  tests were re-run out of them (`Mono.Profiler.Log.ProcessTimeSamplingWorks`,
  `ReaderWriterLockSlimTests`, `System.Web GlobalizationSectionTest`); no BCL
  suite was re-run in full.
* 172, 188, 219, 226, 241 would each need 30+ minutes to re-run, and 241's
  shutdown hang would need a thread dump taken from the hung process.
* 1961, 2811 and 234 were not re-timed; their timings come from other agents'
  measurements and their budgets have already been raised on the tip.
* Nothing was bisected. The `.mono_guards` finding in 1293 is diagnosed from the
  emitted code and the pass source rather than from a bisect, which seemed the
  better use of the time; the offending line is
  `mono/llvm/passes/finally-range.cpp:239` whatever commit introduced it.
