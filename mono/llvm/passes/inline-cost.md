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

Everything under "What the copy changes" is mechanical. Each is there to let one
process hold two copies of the same code, and none of them moves a number.

Everything under "What the copy asks mono" is the point of taking a copy at all.
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

**The indirect-call boost is an option and it is off.** `getInlineCost ()` passed
`BoostIndirect` as a literal `true`; it now reads
`-mono-inline-boost-indirect-calls`. The boost weighs the callee a resolved site
names and takes the slack off the cost, and a managed dispatch resolves to a
declaration this module holds no body for. `analyze ()` sets the threshold before
it returns on an empty body, so such a callee reports the whole threshold as
slack — a discount for a body nothing weighed. A site the walk resolved
concretely is charged the ordinary call penalty instead.

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

**`InlineCostCallAnalyzer::isColdCallSite ()` and `getHotCallSiteThreshold ()`
ask `tier2_site_heat ()` first.** Both otherwise rank the site against the
module's `ProfileSummary`, and a tier-2 compile holds one promoted body, so that
summary is built from that body's own counters. The percentile each threshold is
taken at then lands on one of that body's own count levels, and the ranking
degenerates in opposite directions: in a body with a loop the cold threshold
lands on the entry count, so every call the body always makes reads cold; in a
body without one every counter holds the same value, so every block reads hot.
`tier2_site_heat ()` answers against the caller's entry count instead. It
answers nothing for a caller that carries no tier-2 counter, and the summary
then decides as before.

**`CallAnalyzer::isLoweredToCall ()` asks `lowers_to_a_load ()` first.** Mono
writes a dispatch read as a call to a declaration, and
`TargetTransformInfoImpl::isLoweredToCall ()` answers true for any named
declaration without reading an attribute. So a read that lowers to one load was
charged a call penalty and an argument setup on top of it.

**`CallAnalyzer::visitLoad ()` asks `folded_vtable_read ()` first.** A class's
vtable is a defined constant while a compile optimizes, and the fields and slots
it states fold. The walk has no memory model of its own, so the answer follows
the vtable store an allocation carries — one step no other simplification here
takes. It goes in front of the SROA question, which otherwise consumes the load.

**`CallAnalyzer::visitCallBase ()` asks `folded_type_test ()`.** A type test is
one call each until `lower_type_tests ()`, which runs behind the inliner, so the
model sees the form `cast_answer ()` settles. It goes behind
`simplifyCallSite ()`, which is where a call that is really a value belongs. The
tests are cheap and the arms they rule out are not: a cascade picking one
implementation out of five measured 850 unanswered and 100 answered.

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
same decision the ones above got: drop it when a header declares it, keep it when
no header does. Watch the block walk in `analyze ()`, the tail of
`updateThreshold ()`, the head of `isLoweredToCall ()`, the head of
`visitLoad ()`, and the heads of `isColdCallSite ()` and
`getHotCallSiteThreshold ()`, because that is where the mono calls sit.
`isLoweredToCall ()` and `visitLoad ()` churn upstream more than the rest do, so
budget for them at each bump.

A cost and a budget that `MONO_LLVM_JIT_TRACE` prints are not what an upstream
build gives for the same pair, so a comparison against clang or against LLVM's
own pipeline needs the options off first. Setting each `mono-inline-*-bonus` to
zero, turning off `-mono-inline-implicit-null-free`,
`-mono-inline-dispatch-is-a-load`, `-mono-inline-fold-vtable-fields`,
`-mono-inline-answer-casts` and `-mono-inline-tier2-site-heat`, and turning
**on**
`-mono-inline-boost-indirect-calls` is what gets LLVM's own answers back. Every
answer this file adds owes an option here, so that this list stays the whole of
it. LLVM's `getInlineCost ()` is linked in and still reachable as well, so one
run can be read against the other.
