# Where a compile's time goes, at b461931af78

Everything below was measured on `b461931af78` plus the `MONO_LLVM_JIT_TIMING`
patch that reports it, on this WSL2 box, with sibling agents putting the load
average somewhere between 2 and 5. Absolute milliseconds drift with that load;
the *shares* and the *paired* A/B deltas held across every run. Figures in this
area go stale quickly — re-measure before trusting them against a later tip.

The headline: **86% of a compile is LLVM, and 70% of the total is a fixed
per-method cost that does not depend on how much code the method has.** The
CIL→IR front end is 5.5%. Nothing on mono's side of the boundary is worth
optimizing.

## The tool

`MONO_LLVM_JIT_TIMING=1` prints a phase table to stderr at exit. Phases nest;
the self column is the one to read, and it is a percentage of the whole so it
sums to a hundred.

`perf` is **not usable on this machine** — there is no `linux-tools` package for
`6.6.75.1-microsoft-standard-WSL2+` and `perf record` exits 2. Neither is
`--llvm-opt=-time-passes`: the legacy pass manager's timers are not thread-safe
and two concurrent compiles trip `Cannot start a running timer`, and even
single-threaded they never print, because nothing in this runtime calls
`llvm_shutdown ()`. In-process accounting is the only instrument here.

## The distribution

`mcs/tests/test-async-20.cs`, which drags the whole DLR binder through the JIT:
4157 methods translated over 4315 compile requests, 4134 of them distinct — so
there is no duplicate-compile bug hiding in this. Median of three quiet runs:

| phase | total ms | self ms | self % | what it is |
| --- | --- | --- | --- | --- |
| compile | 7009 | 117 | 1.6 | the whole request; self is the fresh `LLVMContext` and `Module` |
| metadata | 11 | 11 | 0.2 | loading the header |
| translate | 394 | 394 | 5.5 | **CIL→IR, the whole front end** |
| resolve | 62 | 62 | 0.9 | laying out callees' classes, publishing their stubs |
| orc | 6429 | 838 | 11.5 | self is `addIRModule` + `lookup` + JITLink |
| dylib | 88 | 88 | 1.2 | the per-compile JITDylib |
| pipeline | 2250 | 2250 | 30.9 | the tier-0 O1 function simplification pipeline |
| codegen | 3142 | 113 | 1.5 | self is the object buffer copy |
| cgsetup | 464 | 464 | 6.4 | building codegen's pass pipeline, streamer, printer |
| cgrun | 2532 | 2532 | 34.9 | running them |
| dwarf | 126 | 126 | 1.8 | reading `.debug_line` back out of the object |
| jinfo | 47 | 47 | 0.7 | side tables → `MonoJitInfo` |

LLVM (pipeline + cgsetup + cgrun + orc self + dylib + dwarf) is **86.7%**.
Everything mono owns — metadata, translation, resolution, jinfo, module setup —
is under 10%, and more than half of that is the translator.

## It is a floor, not a slope

Fitting per-method compile time against IL size over all 4157 methods:

```
us = 1120 + 7.36 * IL_bytes            (all)
us = 916  + 11.69 * IL_bytes           (IL <= 250 bytes, 94% of methods)
```

The constant term accounts for **70% of all compile time spent**. By bucket:

| IL bytes | methods | sum ms | % of time | median us | us per IL byte |
| --- | --- | --- | --- | --- | --- |
| 0–10 | 1123 | 1073 | 16.1 | 916 | 139.0 |
| 11–25 | 1015 | 1128 | 16.9 | 1058 | 68.8 |
| 26–50 | 800 | 1113 | 16.7 | 1331 | 36.5 |
| 51–100 | 582 | 1024 | 15.4 | 1625 | 25.5 |
| 101–250 | 407 | 1080 | 16.2 | 2493 | 17.7 |
| 251–500 | 167 | 707 | 10.6 | 3794 | 12.1 |
| 501–1000 | 50 | 371 | 5.6 | 7170 | 10.5 |
| 1001+ | 13 | 171 | 2.6 | 12157 | 7.2 |

Half the compiles are 25 IL bytes or less. A property getter — three CIL
instructions — costs **900 µs**, and this is what it is made of:

| phase | median us | share of the ≤10-byte bucket |
| --- | --- | --- |
| cgrun | 342 | 38.3% |
| ORC self, plus module and context setup | 188 | 20.8% |
| pipeline | 179 | 19.5% |
| cgsetup | 99 | 11.2% |
| translate | 36 | 4.1% |
| dwarf | 25 | 2.8% |
| dylib | 19 | 2.1% |
| metadata + resolve + jinfo | 7 | 1.2% |

`cgrun` is the legacy pass manager running the ~55 machine passes
`X86PassConfig` installs at `CodeGenOptLevel::None` (dump them with
`--llvm-opt=-debug-pass=Structure`) over a function with three instructions.
`cgsetup` is constructing those passes, the `MCContext`, the asm backend, the
code emitter, the object writer and the `AsmPrinter` — pure per-module
overhead, and flat at ~100 µs whatever the method is. `orc` self is the
`MaterializationUnit`, the symbol lookup across three dylibs, and building and
relocating a `LinkGraph` for one tiny object.

None of that is compiling the method. It is the cost of entering and leaving
LLVM once.

## What the knobs are worth

Paired, alternating runs, one binary, arms behind environment variables.

**The IR verifier** (on by default because LLVM has assertions). Quiet paired
runs: `compile` 7009 → 6334 ms, of which `pipeline` 2156 → 1670 ms. That is
**~9%**, not the ~6% on record.

**The tier-0 pipeline.** Three arms — the stock O1 function simplification
pipeline, a hand-cut `SROA/EarlyCSE/InstCombine/SimplifyCFG`, and nothing at all
beyond the passes this backend must run:

| arm | compile ms | pipeline ms | cgrun ms |
| --- | --- | --- | --- |
| full (O1 function simplification) | 7200 | 2250 | 2530 |
| lite (SROA, EarlyCSE, InstCombine, SimplifyCFG) | 6300 | 1390 | ~2900 |
| none | 5500 | 630 | 3060 |

Note the third column: taking the optimizer away hands codegen more IR and
`cgrun` grows to eat about a third of what the pipeline gave back. Removing the
optimizer **entirely** — which is not a landable change — buys 24%.

The knobs together (no pipeline, no verifier) buy about 32%.

## Would the six tests pass?

No, and here is how that was settled rather than argued.

`test-async-20` was rebuilt with its `Task.WaitAll (…, 1000)` widened to 120 s
and a stopwatch on each sub-test, so each one reports what it actually needs
against the 1000 ms it is given:

| sub-test | system mono | here, default | here, no pipeline + no verifier |
| --- | --- | --- | --- |
| `Add_1` | 113–128 ms | 4206–4580 ms | 3058–3129 ms |
| `AssignCompound_1` | 22–24 ms | 743–796 ms | 531–563 ms |
| `Convert_1` | 1–2 ms | 114–132 ms | 88–91 ms |
| `Invocation_1` | 28–35 ms | 1054–1128 ms | 761–782 ms |

`Add_1` is the first dynamic test and pays for the whole binder. It needs
**4.2–4.6x its budget**, and the arm that deletes the optimizer still leaves it
at 3x. Running the real test confirms it: `Add_1` reported `FAILED (Timeout)` in
3/3 runs on the default arm and 3/3 on the fastest arm. `Invocation_1` is the
one that would flip — it is over budget by 10% today and comfortably under it
with a 30% cut, which is exactly why it fails intermittently.

The other five behave differently, and the triage's characterisation of them
needs correcting. At the current tip and ordinary load they **pass**:

| test | default | no pipeline + no verifier |
| --- | --- | --- |
| `linker-mscorlib-test-task-01` | 15/15 | 15/15 |
| `unhandled-exception-3` @sgen, no handler | 15/15 | 15/15 |
| `unhandled-exception-1` @boehm, handler | 15/15 | 15/15 |

They only fail under CPU pressure. Spinners on an eight-core box, against
`unhandled-exception-3` @sgen with no handler:

| spinners | default | no pipeline + no verifier |
| --- | --- | --- |
| 0 | 15/15 | 15/15 |
| 8 | 12/15 | 15/15 |
| 16 | 0/20 | 15/20 |

So these five are not a fixed 4-in-15 failure rate, and calling them flaky was
wrong in the other direction too: they are a budget that ordinary load does not
quite exhaust and a loaded sweep does. A 32% compile-time cut moves the
threshold — it does not remove it. At twice-oversubscribed the arm with no
optimizer at all still loses a quarter of its runs.

## What follows

The fixable levers here are all small, and they are trades rather than wins:
the verifier is on deliberately, and cutting the pipeline costs generated-code
quality that nothing in this measurement priced. Together they are ~32% against
a test that needs 4.5x.

What the floor says is that the cost is per *invocation* of LLVM, not per
instruction compiled. So the things that would actually move it are the ones
that reduce how often the JIT enters LLVM at all, or that stop it being LLVM:

- compiling more than one method per module, so `cgsetup`, the `MCContext`, the
  `AsmPrinter`, the object and the `LinkGraph` are amortised — hard, because the
  JIT is demand-driven one method at a time;
- caching translated IR or linked objects across runs;
- a cheaper tier underneath, which is what mini's back end used to be.

None of those is a small change, and all three are architecture decisions rather
than optimizations. The honest summary is that the six tests are not going to be
fixed by tuning this pipeline.
