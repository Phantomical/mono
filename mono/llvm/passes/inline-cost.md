# The copy of LLVM's inline cost model

`inline-cost.cpp` is `llvm/lib/Analysis/InlineCost.cpp`, taken from
`llvmorg-22.1.8`. `CallAnalyzer` and `InlineCostCallAnalyzer` are in no header,
so a subclass outside LLVM cannot reach them. A copy is what lets the model be
told what the managed metadata knows about a call site: the class a caller
already settled its argument to, a type test that class answers, and a dispatch
that resolves to a direct call.

The copy carries two kinds of change, and a diff against upstream has to sort
every hunk into one of them:

    diff -u ~/projects/llvm-project/llvm/lib/Analysis/InlineCost.cpp \
            mono/llvm/passes/inline-cost.cpp

The four under "What the copy changes" are mechanical. Each is there to let one
process hold two copies of the same code, and none of them moves a number.

The two under "What the copy asks mono" are the point of taking a copy at all.
Each is one call into `inline-policy.cpp`, which holds the policy, so a hunk
that reasons about managed metadata inside this file belongs somewhere else.

## What the copy changes

**Every command-line option carries a `mono-` prefix.** `-inline-threshold`
becomes `-mono-inline-threshold`, and every other one the same way. LLVM's
CommandLine calls `report_fatal_error ()` on a name registered twice, and the
mono runtime links the LLVM dylib, which registers all of them at load. Without
the prefix the process dies before `main ()`. Each option keeps its default, so
the copy prices a site the way LLVM does until somebody passes one of the
`mono-` names. `DEBUG_TYPE` is renamed for the same reason.

**The `namespace llvm` block comes out.** LLVM defines
`getStringFnAttrAsInt ()` and `InlineConstants::getInstrCost ()` in this file
and declares both in `InlineCost.h`, so a copy of either under `llvm` is a
second definition of a name the dylib already carries. Moving the `CallBase`
overload of `getStringFnAttrAsInt ()` into `mono` instead is no better: the
argument is an llvm type, so argument-dependent lookup finds the exported
overload beside ours and each call in the file is ambiguous. It comes out, and
the calls reach the dylib's copy, which is the same code. The `Attribute` and
`Function *` overloads move into `mono`, because no header declares them and
nothing else defines them. `getInstrCost ()` goes for good: it reports
`InstrCost` to a caller outside the file, and this copy has no such caller.

**Everything sits in `namespace mono`.** So `int llvm::getCallsiteCost (...)`
loses its qualifier and defines `mono::getCallsiteCost` instead, and the calls
inside the file gain a `mono::`. Those calls need it. Their arguments are llvm
types, so argument-dependent lookup finds the exported overload beside ours and
the two are ambiguous. `inline-cost.hpp` declares the same entry points
`InlineCost.h` does, which is also what makes the ones defined at the foot of
the file reachable from the class bodies above them.

**`InlineCostAnnotationPrinterPass::run ()` comes out.** It defines a member of
a class `InlineCost.h` declares, so it collides with the definition the dylib
carries. `-mono-print-instruction-comments` turns the same annotation on here.

## What the copy asks mono

**`CallAnalyzer::analyze ()` skips the raising arm of a folded null check.**
`implicit_null_check_successor ()` answers which successor a branch marked
`!make.implicit` reaches, and the walk then treats the other arm the way it
treats an arm a constant condition settled. Every managed dereference carries
such a check, so the arms are most of what a freshly translated body costs.

**`InlineCostCallAnalyzer::updateThreshold ()` adds `call_site_bonus ()`.**
It goes in behind `SingleBBBonus` and `VectorBonus`, which are shares of the
threshold, and behind the target's multiplier, because what it adds is an
absolute count rather than a proportion.

`inline-policy.hpp` documents what each answer means. Each answer is gated by an
option of its own, so a run can be put back on LLVM's own answers without a
rebuild.

`InlineCostFeaturesAnalyzer`, `getInliningCostFeatures ()` and
`getInliningCostEstimate ()` stay, and nothing in this tree calls them. They are
what LLVM's ML inline advisor reads the model through, and keeping them costs
only object size. Cutting them makes the next diff against upstream longer.

## Reading the copy against a later release

The copy drifts at every LLVM bump. On a bump, diff the new upstream file
against the old one and apply what it says to the copy by hand. A new `cl::opt`
needs the `mono-` prefix, and a new definition in `namespace llvm` needs the
same decision the two above got: drop it when a header declares it, keep it when
no header does. Watch the block walk in `analyze ()` and the tail of
`updateThreshold ()`, because that is where the two mono calls sit.

A cost and a budget that `MONO_LLVM_JIT_TRACE` prints are not what an upstream
build gives for the same pair, so a comparison against clang or against LLVM's
own pipeline needs the options off first. Turning off
`-mono-inline-implicit-null-free` and setting each `mono-inline-*-bonus` to zero
is what gets LLVM's own answers back. LLVM's `getInlineCost ()` is linked in and
still reachable as well, so one run can be read against the other.
