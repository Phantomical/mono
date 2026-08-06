Diagnose the long-running and hung tests in the `check-all` sweep. Read
`/home/swlynch/projects/mono/CLAUDE.md` first — the build, the CTest harness and
the `mono/llvm/` architecture are described there and its rules are binding.
Then read `.claude/handoff/long-timeouts/README.md`, which is the full record.

This work needs a session without a subagent's 5-minute prompt-cache window:
every one of these cases means starting a suite that runs for 15-30 minutes and
watching it, or looping a test until it hangs. That is why it was handed off
rather than dispatched.

## The short version

Six tests in the sweep at `85e7ebac6ee` hit a timeout wall. They are not one
problem — the log separates them into three different shapes, and the handoff
says which is which and what the log does and does not prove.

The first thing to know is that the earlier classification of four of them was
wrong. `.claude/handoff/ctest-budgets/README.md` filed them as permanently
wedged and therefore environmental, on the premise that none had ever recorded
a completion. Three of them had; the result XMLs are still on disk with dates
and case counts. So these are intermittent or load-dependent, which makes them
worth diagnosing rather than tolerating.

## Where to start

**`bcl-corlib`.** It ran all 10784 of its tests, wrote its result XML, and then
sat doing nothing for another 15 minutes until CTest killed it at the 1800 s
wall. It is a shutdown hang, not a slow suite, and no budget change touches it.
Run it alone, wait for `Results saved as …`, then take both a native backtrace
(`gdb -p … -ex 'thread apply all bt'`) and mono's own `SIGQUIT` managed dump.
`--debug` is on the command line for every BCL suite, so the debugger agent is
a live suspect alongside the finalizer and threadpool threads.

**`runtime/pinvoke-detach-1@sgen`** is the other one with a concrete lead, and
it is small and fast enough to loop. The driver's SIGQUIT dump caught it: a
foreign pthread wedged inside a reverse-pinvoke call into managed code — i.e.
inside attaching a brand-new thread to the runtime and compiling on it — while
the main thread waited on the condvar it never signalled. The delegate variant
hung; the by-name variant in the same program passed. Suspects and the source
walk-through are in the handoff.

The remaining four are timing cases where the honest answer may turn out to be
"needs a bigger budget" or "only fails under load", and the handoff says what
measurement would settle each.

## One trap worth repeating here

`nunit-lite-console` prints its banner, runs everything silently, and prints its
summary only at the end. Two of these suites produced nothing but the banner in
30 minutes and that is **not** evidence they never started a test —
`bcl-corlib`'s block looks identical for its first 10784. Do not read silence as
position.

Also: `ctest -N` overwrites `Testing/Temporary/LastTest.log` even though it runs
nothing. The sweep log is preserved at
`.claude/worktrees/llvm-opt-flag/.claude/scratch/sweep-triage/LastTest-85e7ebac6ee.log`;
copy it out before doing anything else with that build tree.

## Deliverable

A diagnosis per case — cause, or the specific evidence that says it is a budget
rather than a bug — and a fix where one is in reach. A well-evidenced "this is
load, raise the budget to N" is a complete answer for the timing cases; a
shutdown hang is not, and should be fixed. Update
`.claude/handoff/ctest-budgets/README.md` where its classification turns out
wrong, since the tree's budgets were set from it.

Rules (no push, no PR, no `git add -A`, no `git stash`, never build while a test
runs, commit-message style) are at the end of the handoff document.
