# CTest timeout budgets

Branch `ctest-budgets` in the worktree `.claude/worktrees/fix-pool-3`, based on
`llvm18-tiered-jit` at `ade7ef67037`.

Every CTest test now has a timeout. The change is CMake-only; nothing native or
managed was touched, so a reconfigure is all it needs.

## What the change is

Three parts.

**A default, set once, before `include(CTest)`** (`CMakeLists.txt`):

```cmake
set(MONO_CTEST_TIMEOUT 300 CACHE STRING "...")
set(DART_TESTING_TIMEOUT "${MONO_CTEST_TIMEOUT}" CACHE STRING "..." FORCE)
include(CTest)
```

This reaches the 533 tests that name no `TIMEOUT` of their own -- the 18 mini
regression corpora, the 348 gtest cases under `mono/llvm`, the 134 eglib cases,
the unit tests, `runtime-env-options@*` and `runtime-eglib-remap@*`. Before the
change those ran under CTest's own default of 1500s.

**It is a default and not a ceiling.** A suite that sets `TIMEOUT` keeps what it
asked for, and `MONO_CTEST_TIMEOUT` neither raises nor lowers it. This is the
one substantive disagreement with the unmerged commit `6a2044b39d0` "build: cap
every CTest test at 300s", which clamps every suite down to the same number.
Merging that as written fails `runtime/dynamic-method-churn@sgen` permanently:
it is measured at 207-412s and its budget is deliberately 900s.

**Per-test budgets for the tests that do not fit their suite** -- see the table
below. The runtime corpus already had the mechanism, `LONG <program>.exe` in
`mono_runtime_suite()` (added by `01f2ebac958`); this extends that list and adds
the same shape on the class-library side as `MONO_BCL_TESTS_LONG` in
`cmake/MonoManagedTests.cmake`. Each entry carries a note saying what makes that
test slow, the way `MONO_TESTS_BOEHM_DISABLED` does.

## Why 300s for the default

The tests that receive it are the fast ones. Across every `CTestCostData.txt` in
the tree (11 worktrees plus the main one) the slowest test with no explicit
budget is `mini-regression/objects` at **16.5s**, and the 99th percentile of the
358 that have ever been measured is **9.1s**. 300s is roughly 18x the worst of
them.

The number was not chosen to be tight. A cap that a loaded machine can trip is
worse than no cap, because CTest reports it as `***Timeout` and the next person
reads a hang where there was a slow test. 300s is far enough above the observed
worst case that tripping it means something is wedged, and near enough that it
says so within five minutes rather than twenty-five.

It also matches the budget the `mono/tests` corpus already gives each program,
so there is one number for "an ordinary test" instead of two.

## Per-test budgets

| test | was | now | why |
| --- | --- | --- | --- |
| `runtime/dynamic-method-churn` | 900 | 900 (unchanged) | 40000 JIT compiles. 207s and 203s in the completed sweep, 412s at worst across trees. Already `LONG` from `01f2ebac958`; listed here because it is the case that rules out a flat 300s cap. |
| `runtime/appdomain-threadpool-unload` | 300 | 900 | Unloads 100 domains from a PLINQ query sized to `ProcessorCount`. Its cost is set by what else is running: 137s, 140s, 227s, 266s across runs, and **killed at the 300s mark on the boehm half of the completed sweep**. |
| `runtime-interp/appdomain-threadpool-unload` | 300 | 900 | Same program under the interpreter. **Also killed at 300s in the completed sweep.** The interp suite takes its own `LONG` list because the other two are cheap there (3.9s and 11.5s) -- their cost is compilation and the interpreter does none. |
| `runtime/appdomain-unload` | 300 | 900 | Domains with deliberately slow finalizers and a 10s `BeginInvoke` in flight, so most of the time is spent waiting. 171s/176s in the sweep, 247s at worst. Has not actually failed; 82% of budget is not margin. |
| `runtime-stress/domain-stress` | 900 | 1800 | 5 threads x 100 domains, a fixed iteration count, so wall time is whatever the machine gives it. Measured at **816s of 900s** in one run and **killed at 900s on both collectors** in another. The kill is the driver's, not CTest's, so it reports as a plain `Failed` -- easy to mistake for a code bug. |
| `bcl-xunit-corlib` | 1800 | 3600 | Largest assembly in the tree by case count and the one whose cost swings most with load: 868s, 1674s, **1722s pass in the completed sweep**, 1792s pass in another -- then a timeout at 1800s. |
| `bcl-System.Xml` | 1800 | 3600 | 224s idle, 1541s loaded, and killed at 1800s on **three separate sweeps**. Most of the spread is the compute-bound XSLT and schema fixtures. |

`ctest --show-only=json-v1` before and after differs in exactly these 9 tests
(each of the runtime ones is two, `@sgen` and `@boehm`) and in nothing else.

### Deliberately not given a budget

- **`runtime/pinvoke-detach-1@sgen`** was killed at 300s in the completed sweep,
  which looks like the same failure mode. It is not: the same program runs in
  20.9s under the interpreter, 24.8s on boehm and 22-23s in every other
  recorded run. 13x its normal time is a hang, not starvation, and a longer
  budget would only make the sweep wait longer for the same failure.
- **`bcl-corlib`, `bcl-xunit-System.Core`, `bcl-Mono.Debugger.Soft`,
  `bcl-System.ServiceModel`** each stopped at exactly `1800.0` in the sweep and
  have **never** recorded a completion in any tree. A suite that finishes
  somewhere and is killed elsewhere is a budget problem; one that is always
  killed at the wall is wedged. These are in the known environmental floor.
- **`bcl-System.Web`** is the awkward one: 1583s (completing, with failures) in
  the sweep and 1801s timeouts in two others, so it is genuinely near its
  budget. It is left alone because it fails on this machine either way -- it
  wants a web stack -- and raising it buys a longer wait for the same red.
  Revisit if the environmental failures are ever fixed.
- **`gc-descriptors`** was killed at its 3600s budget in one sweep but has a
  recorded cost of 162s. Same shape as `pinvoke-detach-1`. There is a
  `.claude/worktrees/descriptor-slow` worktree investigating it; leave it there.

## Which timings are whose

- The strongest set is a **complete `check-all` sweep that ran to completion in
  the `llvm-opt-flag` worktree** -- 3298 tests, 3652s wall at `-j18`. Its
  per-test times were read out of that worktree's
  `build/Testing/Temporary/LastTest.log`; a copy is in
  `.claude/scratch/ctest-budgets/sweep-LastTest.log` and its ctest summary in
  `sweep-ctest.out`. Every number above described as "in the completed sweep"
  comes from there. Not mine, and not taken under my load.
- The **historical spread** (the "at worst across trees" numbers) is the maximum
  of `Testing/Temporary/CTestCostData.txt` over all 12 build trees on the
  machine. That file is a running mean, so it *understates* peaks -- a single
  bad run is smoothed by the runs around it. Every worst case quoted here is
  therefore a lower bound on the true worst case, which is the right direction
  for choosing a budget.
- **I took no timing runs of my own for choosing the numbers.** The only thing I
  ran was verification. The machine was carrying other agents' builds and a long
  sweep throughout (15-minute load average 25 at the start, 5-14 later), so any
  wall clock I did produce is pessimistic -- fine for confirming a test still
  fits its budget, not something to quote as a clean measurement.

## Verified

- The default is enforced, and does not clamp: a throwaway project with
  `MONO_CTEST_TIMEOUT=5` kills a `sleep 30` test that names no budget
  (`***Timeout 5.00 sec`) while a `sleep 8` test carrying `TIMEOUT 60` passes.
  That is the whole mechanism, both directions, in ten seconds.
- `ctest -N` before and after: **3303 tests, listing byte-identical** (`diff`
  clean, not just the total).
- `ctest -L regression -j18` -- `100% tests passed, 0 tests failed out of 18`
- `ctest -R test-llvm -j18` -- `100% tests passed, 0 tests failed out of 348`
- `cmake --build build --target check` -- `100% tests passed, 0 tests failed out of 528`
- `ctest -R 'runtime/dynamic-method-churn@' -j2` -- `100% tests passed, 0 tests
  failed out of 2`; `@boehm` 84.2s, `@sgen` 88.3s on the final tree (and 103.3s
  / 107.0s on an earlier one, under more load). This is the one constraint the
  task named explicitly. Note how far those are from the 207s the same test took
  inside a full sweep, and the 412s it has reached: on its own it is nowhere
  near any budget, which is exactly why a flat 300s cap looks safe right up
  until the machine is busy.
- The 900s budget reaches the driver, not just CTest. With the test running,
  `ps` shows `TEST_DRIVER_TIMEOUT_SEC=900`, `-DMONO_TEST_TIMEOUT=900` and
  `timeout -s QUIT --kill-after=10 900` in the process tree. Both halves of the
  pair have to move together or CTest kills the program before `MonoRunTest`
  can SIGQUIT it for a thread dump.

## Not verified

Nothing here needs a long run to *choose*; these would only confirm numbers the
sweep already establishes.

- `runtime/appdomain-threadpool-unload@boehm` and
  `runtime-interp/appdomain-threadpool-unload@sgen` under a full `-L runtime` /
  `-L interp` sweep, which is where they were killed. Alone on a quiet machine
  they finish in well under 300s and prove nothing; the failure only appears
  with the machine saturated. `cmake --build build --target check-all` is the
  real reproduction, ~30 min.
- `runtime-stress/domain-stress@{sgen,boehm}` -- `ctest -R domain-stress -j2`,
  ~15 min each. Expect them to pass at somewhere between 800s and 1200s now.
  If either lands above ~1500s the 1800s budget is itself too tight and the
  right answer is to look at why, not to raise it again.
- `bcl-xunit-corlib` and `bcl-System.Xml` -- `ctest -R 'bcl-xunit-corlib|bcl-System.Xml'`,
  30-60 min. Both are expected to *fail on their assertions* on this machine
  (ICU/tzdata floor); what the run confirms is that they now finish and report
  those failures rather than being killed at the wall.

## Traps

- **`ctest -N` overwrites `Testing/Temporary/LastTest.log`** even though it runs
  nothing. Copy the log before listing, or the timings are gone. `CTestCostData.txt`
  survives `-N`, but do not rely on that.
- **`CTestCostData.txt` is a running mean, not a maximum.** Ranking tests by it
  alone hides exactly the outliers a timeout budget is about. Cross-check
  against a real sweep log.
- **A driver timeout does not report as a timeout.** `mono/tests` programs are
  killed by `MonoRunTest.cmake` at `MONO_TEST_TIMEOUT` and by CTest 60s later,
  so a program that runs out of budget shows as `***Failed` at `timeout+10`,
  not `***Timeout`. `runtime-stress/domain-stress` failing at "910.13 sec"
  against a 900s driver budget is a timeout wearing a different label. Look at
  the number, not the word.
- **Read counts from `100% tests passed, 0 tests failed out of N`.** CTest's
  "Label Time Summary" counts tests *carrying* a label, not tests that ran.
- **An empty CMake variable in `set_tests_properties` is a configure error, not
  a default.** Moving the `set(_timeout ...)` block in `_mono_add_managed_test`
  and dropping it by accident produced `set_tests_properties called with
  incorrect number of arguments` once per class-library suite -- forty
  identical errors naming a line that looked fine. Re-run `cmake -S . -B build`
  and read the *first* error after any edit to a test-registration function;
  the earlier `--show-only=json-v1` check had passed against the previous
  configure and happily reported the tree as correct.
- `refs/stash` is shared across all pool worktrees; use `git diff` + `git apply -R`
  for an A/B, never `git stash`.
