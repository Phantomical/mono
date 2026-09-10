# Verifying the ETW JIT events against PerfView

`test-etw-profiler.cpp` gates the payload computations without a session. This is the
other half: a `PerfView collect` procedure against a live process, which nothing runs
automatically because a real-time ETW session needs admin. Nobody has run this capture -
every step below is either read off PerfView's own source (cited) or checked locally
without ETW (the tiering claims, via `MONO_JIT_DUMP`). Neither substitutes for actually
opening the result in PerfView, which whoever runs this owns.

## The capture command line

PerfView decodes CLR events from its own compiled-in parser, keyed on provider GUID,
event ID and version - no manifest has to be registered on the machine. `-ClrEvents:`
selects which of the regular CLR provider's keyword bits are on;
`ClrTraceEventParser.Keywords` (`src/TraceEvent/Parsers/ClrTraceEventParser.cs`) names
them. Verified there:

| plan's name | verified name | value |
| --- | --- | --- |
| `Jit` | `Jit` | 0x10 |
| `StopEnumeration` | `StopEnumeration` | 0x80 |
| `JittedMethodILToNativeMap` | `JittedMethodILToNativeMap` | 0x20000 |
| `SupressNGen` | `OverrideAndSuppressNGenEvents` | 0x40000 |
| `Loader` | `Loader` | 0x8 |

`SupressNGen` (one p) is a real name, and it is not rundown-only. `ClrTraceEventParser.Keywords`
- the regular provider's own enum, the one `-ClrEvents:` reads - defines `SupressNGen`
itself, as a second name for the same 0x40000 bit as `OverrideAndSuppressNGenEvents`;
`JITSymbols`, the composite this line is built to match, is written with the short
spelling (`Jit | StopEnumeration | JittedMethodILToNativeMap | SupressNGen | Loader`).
`ClrRundownTraceEventParser.Keywords` also defines its own `SupressNGen` at the same
value, in the same file, but that is a second member of a different enum, not the only
one. `-ClrEvents:SupressNGen` and `-ClrEvents:OverrideAndSuppressNGenEvents` parse to the
same value (`ParseSimpleEnumValue`, `commandLine.cs`, matches an enum by member name), so
the plan's original spelling was never wrong. The line below keeps the longer name; either
one reaches the same bit.

`src/PerfView/Utilities/commandLine.cs` is where the qualifier parser itself lives:
`-Name:Value` and `/Name:Value` are both accepted (`arg[0] == '/'`), `:` and `=` are both
accepted as the separator (`separators = { ':', '=' }`), and a `[Flags]` enum's value
list takes `,` or `+` between names (`IndexOfAny (new [] { ',', '+', '-' }, ...)`) - `-`
subtracts a name instead of adding it. `-ClrEventLevel:Verbose` is `CommandLineArgs`'s own
default (`ClrEventLevel = TraceEventLevel.Verbose`), so it does nothing here beyond
saying so. UsersGuide.htm's own examples put qualifiers before the verb
(`PerfView /wpr collect`), which is what the line below follows:

```
PerfView /ClrEvents:Jit,StopEnumeration,JittedMethodILToNativeMap,OverrideAndSuppressNGenEvents,Loader /ClrEventLevel:Verbose /DataFile:etw-jit-verify.etl collect
```

`collect` starts the session and waits for a stop (`PerfView stop`, or Enter in the
console PerfView opens) rather than wrapping one command the way `run` does - both
verification runs below want that, because the late-attach one needs the target already
running before the session starts, which `run` cannot do.

## What to run under it

`mono/benchmark/fib.exe` (`Fib:fib`, `mono/benchmark/fib.cs`) is the corpus: `fib (32)`
recurses about 7 million times, run `repeat * 50` times, so one method is called often
enough to promote past tier 0 with room to spare. Lowering the thresholds
(`--llvm-opt=-mono-tier1-threshold=` and `-mono-tier2-threshold=`, `runtime/options.cpp`)
shortens how much of that recursion it takes:

```
MONO_CFG_DIR=build/runtime/etc MONO_PATH=build/mcs/class/lib/net_4_x \
  build/mono/mini/mono-sgen.exe \
  --llvm-opt=-mono-tier1-threshold=20000 --llvm-opt=-mono-tier2-threshold=2000000 \
  build/mono/benchmark/fib.exe 1
```

**Checked locally, without a session**: `MONO_JIT_DUMP=tier0-asm,tier1-ir,tier2-ir
MONO_JIT_DUMP_FILTER=Fib:fib MONO_JIT_DUMP_DIR=<dir>` against the exact command above
writes all three dumps, so the thresholds above do reach every tier for this method on
this build. `MONO_LLVM_JIT_TRACE=1` is not a reliable way to see the same thing - one
run of it printed the tier-0 and tier-2 `translating` lines for `Fib:fib` but not the
tier-1 one, and the dump was what settled that tier 1 still ran. Trust
`MONO_JIT_DUMP`'s dumps for this, not a `grep` over the trace.

## What to check in the result

Open the `.etl` (PerfView zips it into `.etl.zip` on stop) and look at a CPU-sampled
stack under `Fib.exe`'s process: a frame inside `Fib.fib`'s recursion has to resolve to
that name rather than a bare address, and the module PerfView attributes it to has to be
`fib.exe` rather than an unnamed synthetic one - `ModuleLoad` is what supplies that, and
it is emitted for every non-metadata-only image regardless of the `-ClrEvents:` keyword
set above (`image_event ()`, `etw-profiler.cpp`).

The JIT Stats view (`JitStats.cs`) has an `OptimizationTier` column, but it is hidden by
default - `-ShowOptimizationTiers` (`CommandLineArgs.cs`) has to be passed when PerfView
itself is launched, or turned on from the GUI, before opening the trace; the column is
otherwise absent, not merely empty. Once it is on, `Fib.fib` has to appear as **three**
rows sharing one method - `MethodLoadVerbose` fires once per body, and a promotion never
retires the old one (`test-tier.cpp`'s `MethodTier.EachSupersededBodyKeepsItsOwnTier`) -
each at its own start
address, in this order and reading as `clr_tier ()` (`etw-profiler.cpp`) promises:

| this runtime's tier | `MethodFlags` bits | the column reads |
| --- | --- | --- |
| tier 0 (classic) | 1 | `MinOptJitted` |
| tier 1, tier 2 on | 6 | `QuickJittedInstrumented` |
| tier 1, `-mono-tier2=0` | 3 | `QuickJitted` |
| tier 2 | 4 | `OptimizedTier1` |

The plan names the tier-1-with-tier-2-on string `InstrumentedTier`. TraceEvent's actual
`OptimizationTier` enum (`ClrTraceEventParser.cs`) does not have a member by that name -
value 6, the one this runtime sends for that case, is `QuickJittedInstrumented`. Look for
`QuickJittedInstrumented` in the column, not `InstrumentedTier`.

`Fib.fib` is a method on a top-level, non-generic class, so it is a check on tiering
alone. Naming fidelity - the nested-class chain and a generic instantiation showing up
correctly in `MethodNamespace` - is what `EtwProfilerNaming`'s cases gate
(`test-etw-profiler.cpp`), and nothing about opening a trace re-checks it; PerfView shows
whatever string `etw_method_namespace ()` sent, whether or not it is right.

## Late attach

This is the only run that exercises the rundown Package C added -
`Private_EventControlCallback` (`etw-profiler.cpp`) fires only when a provider is
(re-)enabled, and a method compiled before that never gets a live `MethodLoadVerbose` of
its own.

```
MONO_CFG_DIR=build/runtime/etc MONO_PATH=build/mcs/class/lib/net_4_x \
  build/mono/mini/mono-sgen.exe \
  --llvm-opt=-mono-tier1-threshold=20000 --llvm-opt=-mono-tier2-threshold=2000000 \
  build/mono/benchmark/fib.exe 30 &
```

Started first, and with a `repeat` large enough to still be running by the time the
steps below finish - `Fib.fib` reaches tier 2 within the first handful of recursive
calls at these thresholds, so a few seconds' head start is enough for every body to
exist before any session opens. Then start the same `collect` line as above, let it run
a few seconds, and stop it (`PerfView stop`, or Enter in the console window).

`DoClrRundownForSession` (`CommandProcessor.cs`) is what answers a stop: a *second*,
separate session that enables `Microsoft-Windows-DotNETRuntimeRundown` fresh, with
`ClrRundownTraceEventParser.Keywords.Default` unless `/NoRundown` or `/NoClrRundown` was
given - neither was above. That `Default` carries `ForceEndRundown` (0x100) and not
`StartEnumeration` (0x40) - the commit that landed Package C (`5f9a25aad77`) names this
same fact as why the old code, which always emitted the DCEnd flavour regardless of
`MatchAnyKeyword`, worked against PerfView's own default by luck. So
`etw_rundown_pass ()` sees `MatchAnyKeyword` with the end bit set and not the start bit,
and answers `{ start: false, end: true }` - every JIT'd method still alive at stop,
`Fib.fib`'s three bodies included, is reported once each as
`MethodDCEndVerbose` rather than `MethodLoadVerbose`, bracketed by one `DCEndInit_V1`
before the walk and one `DCEndComplete_V1` after (`on_attach ()`, `etw-profiler.cpp`). A
truncated rundown - the process killed mid-walk, or the walk crashing - is a
`DCEndInit_V1` with no matching `DCEndComplete_V1`; a complete one has both, and the
three `Fib.fib` rows between them, none of which have a `MethodLoadVerbose` counterpart
anywhere earlier in the trace, because the session started after all three compiled.

## Measurement caveat for a KSP capture

Carried over from the design plan rather than re-derived here, because nothing in this
package changes it. The player carries `player-connection-debug=1` and starts the
debugger agent, which sets `gen_sdb_seq_points`, which turns off both inliners
(`folding_off_for_seq_points ()`, `mono/llvm/runtime/inline-scope.cpp`). A capture taken
against KSP without an override is a capture of a runtime that does not inline.
`MONO_DEBUG=force-disable-seq-points` is the override that reaches it
(`mono/mini/mini-runtime.c`); `--debug=force-disable-seq-points` on the player's own
command line does not, because a Unity player's argv never reaches
`mono_jit_parse_options ()`.
