# The long-timeout investigation

Six tests in the `check-all` sweep at `85e7ebac6ee` either ran to their timeout
wall or hung far past their normal cost. Diagnosing any one of them means
starting a suite that takes 15-30 minutes and watching it, which is why this is
a handoff rather than a subagent task: a subagent fighting a 5-minute
prompt-cache window cannot sit with a 30-minute suite.

Nothing here is fixed. What is here is the evidence that was extractable from
the sweep log without re-running anything, and — more usefully — a correction
to how these six were classified the first time round.

## The correction that matters

`.claude/handoff/ctest-budgets/README.md` (task #70) put four of these in the
"deliberately not given a budget" bucket on the grounds that they

> each stopped at exactly `1800.0` in the sweep and have **never** recorded a
> completion in any tree. A suite that finishes somewhere and is killed
> elsewhere is a budget problem; one that is always killed at the wall is
> wedged.

The reasoning is right and the premise is wrong. Three of the four *have*
completed, and the result XMLs are still on disk:

| Suite | Completed run | Cases |
|---|---|---|
| `bcl-System.Xml` | `build/…/tests/TestResult-net_4_x-System.Xml.xml`, 2026-08-05 19:36 | 2024, 0 errors, 0 failures |
| `bcl-System.ServiceModel` | `.claude/worktrees/fix-pool-4/…`, 2026-08-05 16:25 | 990, 0 errors, 0 failures |
| `bcl-Mono.Debugger.Soft` | `.claude/worktrees/llvm-opt-flag/…`, 2026-08-06 14:53 | 121, 2 errors, 11 failures |

So they are not permanently wedged, and "environmental floor" is not the right
box for them. Whatever this is, it is intermittent or load-dependent. That
moves them from *ignore* to *worth a diagnosis*, which is what this document is
for.

`bcl-corlib` is a different story again — see below; it is the strongest lead
of the six.

## What the sweep log actually shows

Source: `.claude/worktrees/llvm-opt-flag/.claude/scratch/sweep-triage/LastTest-85e7ebac6ee.log`
(40 MB, 3223 test blocks). **Copy it somewhere safe before touching that build
tree** — `ctest -N` overwrites `LastTest.log` even though it runs nothing, and
that is how the original was already lost once. `extract-times.py` in this
directory pulls the per-test wall times out of it;
`sweep-85e7ebac6ee-slowest.txt` is the slowest 60 from that run.

The sweep ran at `-j18`. `bcl-Mono.Debugger.Soft` started 17:26 and was killed
17:56; `bcl-corlib`, `bcl-System.Xml`, `bcl-System.ServiceModel` and
`bcl-xunit-System.Core` all started 17:55 and were all killed at 18:25. Four
1800-second suites therefore overlapped completely, and `pinvoke-detach-1`
started at 18:10, in the middle of that. Load is a real confound for the
timing-based cases; it is *not* an explanation for `bcl-corlib`.

### 1. `bcl-corlib` — completes, then hangs at exit (1800.04 s)

The strongest finding, and the one that would never have been found by raising
the budget. The suite's output block ends with:

```
Tests run: 10784, Passed: 10673, Errors: 2, Failures: 9, Inconclusive: 1
  Not run: 99, Invalid: 0, Ignored: 99, Skipped: 0
...
Results saved as .../TestResult-net_4_x-corlib.xml.
```

and the XML on disk is timestamped 18:10. The suite started at 17:55, so it
**finished its 10784 tests in about 15 minutes and then sat for another 15
doing nothing until CTest killed it at 1800 s.** This is a shutdown hang, not a
slow suite. A bigger budget makes the sweep wait longer for the same red.

Adjacent prior art: task #79 was a debugger-agent shutdown hang, and
`runtime-stress` has a known shutdown SIGSEGV. Shutdown is a soft spot in this
runtime right now. This is the case to start with.

### 2. `runtime/pinvoke-detach-1@sgen` — a real hang, in a named test (310.74 s)

Killed by the driver at 300 s. It runs in 20.9 s under the interpreter, 24.8 s
on boehm and 22-23 s in every other recorded run, so 13x is a hang and not
starvation — and unlike the others, the driver's SIGQUIT dump tells us exactly
where:

```
"<unnamed thread>"  at <unknown> <0xffffffff>
  at (wrapper managed-to-native) Tests.mono_test_attach_invoke_block_foreign_thread (...)
  at Tests.test_0_attach_invoke_block_foreign_thread_delegate () <0x0003e>
```

That is the main thread, blocked inside the P/Invoke. Read
`mono/tests/libtest.c:8518` and `:8539` together: the helper spawns a foreign
(never-attached) pthread, then waits on a condvar for it to signal *after* it
has called into the runtime. The foreign thread's job is

```c
nm->del ();                      /* reverse-pinvoke into managed code */
pthread_cond_signal (&nm->coord_cond);
pthread_mutex_lock (&nm->deadlock_mutex);   /* blocks forever, by design */
```

The main thread never got the signal, so **the foreign thread wedged inside
`nm->del ()`** — that is, inside attaching a brand-new thread to the runtime
and running the reverse-pinvoke wrapper for a delegate, which under this JIT
means compiling on a freshly attached thread.

Two details worth carrying:

- The `_delegate` variant is the one that hung. The sibling
  `test_0_attach_invoke_block_foreign_thread`, which goes through
  `test_invoke_by_name` instead of a delegate, runs earlier in the same program
  and passed.
- This sweep predates the delegate `invoke_impl` work (`aa63342d628`, task
  #88), so #88 did not cause it — but #88 changed the delegate call path, so
  re-measure at the current tip before concluding anything.

Suspects, in the order worth checking: a backend lock held across an ORC lookup
(lookups drain materialization units inline — see
`orc-lookups-drain-mus-inline` in memory), the attach racing a GC while another
thread is mid-compile, or something in the reverse-pinvoke wrapper's own
compilation.

### 3. `bcl-Mono.Debugger.Soft` (1800.03 s) and `bcl-System.ServiceModel` (1800.01 s) — no output past the banner

Both blocks are 15 lines: the NUnitLite banner, the runtime version, then
nothing for 30 minutes.

**Do not read that as "wedged before running a single test."** I nearly did.
`nunit-lite-console` in this tree prints its banner, runs everything silently,
and prints the summary and failure details only at the end — `bcl-corlib`'s
block has exactly the same shape for its first 10784 tests. Silence proves only
that the suite never reached its summary. It says nothing about where it was.

Getting real information out of these two means running them alone and taking a
stack when they stall. `bcl-Mono.Debugger.Soft` is the more interesting of the
two: it drives the soft debugger, which is under active change (#78, #80), and
its completed run above shows 2 errors + 11 failures rather than a clean pass,
so it is not healthy even when it finishes.

### 4. `bcl-System.Xml` (1800.02 s) and `bcl-xunit-System.Core` (1800.09 s) — still producing output at the wall

These two were demonstrably still doing work when they were killed:
`bcl-System.Xml`'s tail is `<DoxCompoundKind>class</DoxCompoundKind>` fixture
spam, and `bcl-xunit-System.Core`'s tail is a live stream of xunit `[SKIP]`
lines (the xunit console, unlike nunit-lite, reports per test).

These are the two most likely to be genuine budget cases rather than hangs —
`bcl-System.Xml` has been measured at 224 s idle against 1541 s loaded, a 7x
swing with machine load. `bcl-xunit-System.Core` has no recorded completion
anywhere, so its honest cost is still unknown.

## What to do

Roughly in value order. Each is independent; there is no need to do them all.

1. **`bcl-corlib`'s shutdown hang.** Run the suite alone, watch for the
   `Results saved as …` line, and once it appears take a stack:

   ```
   gdb -p $(pgrep -f 'nunit-lite-console.*corlib') -batch -ex 'thread apply all bt'
   ```

   Mono also dumps managed stacks for all threads on `SIGQUIT`, which is worth
   having alongside the native backtrace. Expect the interesting thread to be a
   finalizer, a threadpool worker, or the debugger agent — `--debug` is on the
   command line for every BCL suite.

2. **`pinvoke-detach-1`.** Small, fast, self-contained, and the stack above says
   where to look. Run it in a loop until it hangs, then attach. This one may
   well be reproducible in minutes rather than half an hour.

3. **`bcl-Mono.Debugger.Soft` and `bcl-System.ServiceModel`** alone on an idle
   machine. If either completes, the finding is "load-dependent" and the next
   question is whether it is a budget problem or a hang that only a loaded
   machine triggers. If either stalls, attach.

4. **`bcl-System.Xml` and `bcl-xunit-System.Core`** alone, for a clean cost
   number. If either lands above ~1500 s idle then the 1800 s budget is itself
   too tight and `MONO_BCL_TESTS_LONG` should carry it; if it lands near 224 s
   then the sweep number was load and the wall was a coincidence of scheduling.

## How to run one BCL suite by hand

Take the command straight out of the sweep log rather than reconstructing it —
`extract-times.py` shows the block structure, and each block's `Command:` and
`Directory:` lines are verbatim. The shape is:

```
cd <tree>/mcs/class/<Assembly>
<tree>/build/runtime/mono-wrapper --debug \
  <tree>/build/mcs/class/lib/net_4_x-linux/tests/runner/<Assembly>/nunit-lite-console.exe \
  <tree>/build/mcs/class/lib/net_4_x-linux/tests/net_4_x_<Assembly>_test.dll -exclude=...
```

`mono-wrapper` sets the environment up for you; a bare `mono-sgen` from a
worktree needs `MONO_CFG_DIR=$PWD/build/runtime/etc` or anything touching the
filesystem dies with `DllNotFoundException: System.Native`.

Or by CTest, which applies the real budget:

```
ctest --test-dir build -R '^bcl-corlib$' --output-on-failure
```

## Traps in this area

- **`ctest -N` overwrites `Testing/Temporary/LastTest.log`** even though it runs
  nothing. Copy the log out before doing anything else with a build tree whose
  sweep you care about.
- **A driver timeout does not report as a timeout.** `mono/tests` programs are
  killed by `MonoRunTest.cmake` at `MONO_TEST_TIMEOUT` and by CTest 60 s later,
  so a program that runs out of budget shows as `***Failed` at `timeout+10`,
  not `***Timeout`. `pinvoke-detach-1` at 310.74 s against a 300 s driver budget
  is a timeout wearing a different label.
- **`MONO_CTEST_TIMEOUT` is a default, not a ceiling.** A suite that names its
  own `TIMEOUT` keeps it. Raising the global variable does not move `bcl-*`.
- Read gate counts from the `100% tests passed, 0 tests failed out of N` line,
  **never** from ctest's "Label Time Summary" — that counts tests carrying a
  label, not tests that ran.
- **nunit-lite output is banner → silence → summary.** Silence is not evidence
  of no progress (see §3).
- Load is a confound worth controlling: four 1800 s suites ran concurrently in
  the sweep. Anything measured alone on an idle machine is a different
  experiment from anything measured in a `-j18` sweep, and both are useful, but
  they are not comparable.

## Rules

- **Never `git push`, never open a PR** unless explicitly asked. Committing
  locally is the deliverable.
- Stage by explicit path. **Never `git add -A`** — there are pre-existing
  untracked `mcs/class/corlib/crash_*` files that belong to the user.
- **Do not use `git stash`.** `refs/stash` lives in the common git directory, so
  every pool worktree shares one stack and concurrent agents interleave on it
  silently. Use `git diff HEAD > /tmp/mine.patch` and `git apply -R`.
- Never build while a test is running in the same tree — relinking `mono-sgen`
  SIGSEGVs the running suite.
- Scope any `pkill` to your own PIDs; other work may be live in sibling
  worktrees.
- Commit style: `mono/mini: ` / `mono/llvm: ` / `docs: ` prefix, lowercase
  imperative summary, blank line, prose body wrapped at 76 columns explaining
  *why*, blank line, then
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`. No references to
  plans, tasks, briefs or handoffs in the message.

## What an independent triage added

A separate pass over the same sweep (`.claude/plans/sweep-triage-85e7ebac6ee.md`)
reached the same two conclusions about `bcl-corlib` and `pinvoke-detach-1`
without seeing this document, and turned up four things worth carrying:

- **`pinvoke-detach-1` reproduces.** 30 runs at the tip: 29 finished in 22-24 s,
  one was still alive at 90 s. So it is a hang with a rate of roughly 1 in 30
  and not a once-off, and it can be provoked in minutes rather than by waiting
  for a sweep. This is the cheapest of the six to work on.
- **`bcl-corlib`'s timings, independently.** Tests took ~895 s, the XML was
  written at 18:10:44, CTest killed it around 18:25 — the process sat about 15
  minutes without exiting.
- **Neither `bcl-Mono.Debugger.Soft` nor `bcl-System.ServiceModel` wrote a
  result XML in that sweep** — ServiceModel's does not exist in that tree at
  all. Consistent with a hang, but still an inference from silence, and the
  triage was as careful as this document to say so. Do not just raise their
  budgets.
- **`bcl-xunit-System.Core` has one genuine divergence hiding in an
  environmental floor.** Its 42 recorded FAILs are all `useInterpreter: True`
  LINQ conversions, and most give the same wrong answer on system mono. The
  exception is `Expression.Convert (float.MaxValue, int)`, which yields 0 here
  and `-2147483648` on system mono. The triage could not separate runtime from
  class library because a corlib version mismatch blocks the A/B. That is a
  correctness question, not a timeout one, but it lives in the same suite.
- **`bcl-System.Web`'s 396 failures are one message repeated** —
  `RemotingException: No receiver for uri`, naming a single object, and the
  fixture passes standalone. The proposed mechanism is that `MyHost` is a
  `MarshalByRefObject` on the default five-minute lease while the suite ran for
  26 minutes. Unproven, and worth one experiment: if it holds, the suite's
  failures are a duration artifact rather than a bug, which changes what a
  bigger budget would even mean for it.

## Related

- `.claude/handoff/ctest-budgets/README.md` — task #70, the timeout budgets that
  are now in the tree, and the source of the classification corrected above.
- `.claude/handoff/filter-frames/README.md` — task #78, the other handed-off
  investigation; `bcl-Mono.Debugger.Soft` overlaps its territory.
- `.claude/worktrees/descriptor-slow` — a separate investigation into
  `gc-descriptors`, which shows the same "killed at its budget once, 162 s
  normally" shape as `pinvoke-detach-1`.
