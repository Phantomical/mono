# The copy of LLVM's inline cost model

`inline-cost.cpp` is `llvm/lib/Analysis/InlineCost.cpp`, taken from
`llvmorg-22.1.8`. `CallAnalyzer` and `InlineCostCallAnalyzer` are in no header,
so a subclass outside LLVM cannot reach them. A copy is what lets mono tell the
model what the managed metadata knows about a call site: the class a caller
already settled its argument to, a type test that class answers, and a dispatch
that resolves to a direct call.

The copy carries two kinds of change, and a diff against upstream has to sort
every hunk into one of them:

    diff -u ~/projects/llvm-project/llvm/lib/Analysis/InlineCost.cpp \
            mono/llvm/passes/inline-cost.cpp

Two items under "What the copy changes" are policy choices: the indirect-call
boost and the last-call-to-static bonus. Each one's note says why. The rest is
mechanical, there to let one process hold two copies of the same code, and moves
no number.

Every entry under "What the copy asks mono" is the point of taking a copy at all.
Each calls into `inline-policy.cpp`, which holds the policy, so a hunk
that reasons about managed metadata inside the copy belongs somewhere else.

## What the copy changes

**Every command-line option carries a `mono-` prefix.** `-inline-threshold`
becomes `-mono-inline-threshold`, and every other one the same way. LLVM's
CommandLine calls `report_fatal_error ()` on a name registered twice, and the
mono runtime links the LLVM dylib, which registers all of them at load. Without
the prefix the process dies before `main ()`. Each renamed option keeps its
default, so the rename moves no number. `DEBUG_TYPE` is renamed for the same
reason.

**The `namespace llvm` block comes out.** LLVM defines
`getStringFnAttrAsInt ()` and `InlineConstants::getInstrCost ()` in this file
and declares both in `InlineCost.h`, so a copy of either under `llvm` is a
second definition of a name the dylib already carries. Moving the `CallBase`
overload of `getStringFnAttrAsInt ()` into `mono` instead is no better: the
argument is an llvm type, so argument-dependent lookup finds the exported
overload beside ours and each call in the file is ambiguous. It comes out, and
the calls reach the dylib's copy, which is the same code. The `Attribute` and
`Function *` overloads move into `mono`, because no header declares them, so
argument-dependent lookup does not reach the dylib's. `getInstrCost ()` goes for
good: it reports `InstrCost` to a caller outside the file, and this copy has no
such caller.

**Everything sits in `namespace mono`.** So `int llvm::getCallsiteCost (...)`
loses its qualifier and defines `mono::getCallsiteCost` instead, and the calls
inside the file gain a `mono::`. Those calls need it. Their arguments are llvm
types, so argument-dependent lookup finds the exported overload beside ours and
the two are ambiguous. `inline-cost.hpp` declares these entry points under
`mono`, which is also what makes the ones defined at the foot of the file
reachable from the class bodies above them.

**`InlineCostAnnotationPrinterPass::run ()` comes out.** It defines a member of
a class `InlineCost.h` declares, so it collides with the definition the dylib
carries. `-mono-print-instruction-comments` turns the same annotation on here.

**The indirect-call boost is an option and it is off.** `getInlineCost ()` passed
`BoostIndirect` as a literal `true`. It now reads
`-mono-inline-boost-indirect-calls`. The boost weighs the callee a resolved site
names and takes the slack off the cost, and a managed dispatch resolves to a
declaration this module holds no body for. `analyze ()` sets the threshold before
it returns on an empty body, so such a callee reports the whole threshold as
slack — a discount for a body nothing weighed. A site the walk resolved
concretely is charged the ordinary call penalty instead.

**The last-call-to-static bonus is zero.** `LastCallToStaticBonus` reads
`TTI.getInliningLastCallToStaticBonus ()` upstream, and the discount prices
deleting a body the fold takes the last call to. `isSoleCallToLocalFunction ()`
counts the uses the module holds, and a module here holds one root and the copies
translated for it, so that count describes one compile rather than the program. A
copy nothing folds is erased by `StripInlineCopiesPass` either way, and the callee
keeps its own body behind its thunk, so no fold deletes a body. Neither LLVM nor
the copy carries an option for this number. `isSoleCallToLocalFunction ()` itself
stays, because `analyze ()` also reads it to refuse a callee holding a
`noduplicate` call.

## What the copy asks mono

**`CallAnalyzer::analyze ()` skips the raising arm of a folded null check.**
`implicit_null_check_successor ()` answers which successor a branch marked
`!make.implicit` reaches, and the walk then treats the other arm the way it
treats an arm a constant condition settled. Every managed dereference carries
such a check, so the arms are most of what a freshly translated body costs.

**`InlineCostCallAnalyzer::updateThreshold ()` adds `call_site_bonus ()`, at two
places.** The first is behind `SingleBBBonus` and `VectorBonus`, which are shares
of the threshold, and behind the target's multiplier, because what it adds is an
absolute count rather than a proportion. Both places pass `GetConstantValues`, a
member with no upstream counterpart, threaded the same way as `GetBFI` from
`getInlineCost ()`'s caller down through `InlineCostCallAnalyzer`'s constructor.
It answers a settled walk of the caller's own values, off the same memory model
`FoldDelegateInvokesPass` reads, which is what a delegate argument bonus needs
and no other bonus here does.

The second is the early return `updateThreshold ()` takes when
`allowSizeGrowth ()` answers no, which upstream leaves at a threshold of zero.
That test reads the terminator of the block the call returns into, and a block
ending `unreachable` runs almost nothing. What the bonus counts sits elsewhere in
the function: the call is handed the object, so
`BasicAAResult::getModRefInfo ()` answers ModRef for it, and the loop that pays
is in another block. A `Dispose ()` call in a landing pad is such a site. In
`linq-devirt`'s `ListOne` that call kept the enumerator field in memory. The
early return is above the addition at the tail, so the bonus needs a second site.

**`InlineCostCallAnalyzer::isColdCallSite ()` and `getHotCallSiteThreshold ()`
ask `tier2_site_heat ()` first.** Both otherwise rank the site against the
module's `ProfileSummary`, and a tier-2 compile holds one promoted body, so that
summary is built from that body's own counters. The percentile each threshold is
taken at then lands on one of that body's own count levels, and the ranking
degenerates in opposite directions. In a body with a loop the cold threshold
lands on the entry count, so every call the body always makes reads cold. In a
body without one every counter holds the same value, so every block reads hot.
`tier2_site_heat ()` answers against the caller's entry count instead. It
answers nothing for a caller that carries no tier-2 counter, and the summary
then decides as before.

**`CallAnalyzer::isLoweredToCall ()` asks `lowers_to_a_load ()` first.** Mono
writes a dispatch read as a call to a declaration, and
`TargetTransformInfoImpl::isLoweredToCall ()` answers true for any declaration
outside its list of libm names, without reading an attribute. So a read that
lowers to one load was charged a call penalty and an argument setup on top of it.

**`CallAnalyzer::visitLoad ()` asks `folded_object_vtable ()` first.** Mono
writes the read of an object's vtable word as a load. The walk has no
memory model of its own, so the answer follows the vtable store an allocation
carries — one step no other simplification here takes. It goes in front of the
SROA question, which otherwise consumes the load.

**`CallAnalyzer::visitCallBase ()` asks `folded_type_test ()` and then
`folded_vtable_read ()`.** A type test is one call each until
`lower_type_tests ()`, which runs behind the inliner, so the model sees the form
`cast_answer ()` settles. A class's vtable is a defined constant while a compile
optimizes, so the fields it states fold once the walk has that symbol. Both go
behind `simplifyCallSite ()`, which is where a call that is really a value
belongs. The tests are cheap and the arms they rule out are not: a cascade
picking one implementation out of five measured 850 unanswered and 100 answered.

**`InlineCostCallAnalyzer::onAnalysisStart ()` adds `save_lmf_cost ()` beside the
coldcc penalty.** A `save_lmf` callee's frame — the LMF slot, the callee-saved
clobber, the frameaddress/stacksave pair — arrives with the translated body, and
the walk that follows prices each of those instructions like any other. Nothing
in the walk otherwise charges for the caller-side registers the clobber forces.

`inline-policy.hpp` documents what each answer means. Each answer asks an option
of its own first, so a run can be put back on LLVM's own answers without a
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
no header does. Watch the block walk in `analyze ()`, the head and the tail of
`updateThreshold ()`, the head of `isLoweredToCall ()`, the head of
`visitLoad ()`, the head of `visitCallBase ()`, the heads of
`isColdCallSite ()` and `getHotCallSiteThreshold ()`, and the coldcc check in
`onAnalysisStart ()`, because that is where the mono calls sit.
`isLoweredToCall ()` and `visitLoad ()` churn upstream more than the rest do,
so budget for them at each bump.

A cost and a budget that `MONO_LLVM_JIT_TRACE` prints are not what an upstream
build gives for the same pair, so a comparison against clang or against LLVM's
own pipeline needs the options off first. Set each `mono-inline-*-bonus` to
zero, along with `-mono-inline-save-lmf-penalty`, turn off
`-mono-inline-implicit-null-free`, `-mono-inline-dispatch-is-a-load`,
`-mono-inline-fold-vtable-fields`, `-mono-inline-answer-casts` and
`-mono-inline-tier2-site-heat`, and turn **on**
`-mono-inline-boost-indirect-calls`. The last-call-to-static bonus is what a run
set up that way still does not get back, because no option reaches it: LLVM's
value for it needs a rebuild. LLVM's `getInlineCost ()` is linked in and still
reachable as well, so one run can be read against the other.
