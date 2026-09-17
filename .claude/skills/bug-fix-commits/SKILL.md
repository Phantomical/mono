---
name: bug-fix-commits
description: "Writes bug-fix commit messages in this repo's house style. Use when drafting or reviewing the commit message for a bug fix, not a feature, refactor, or chore commit."
---

# Bug-fix commit messages

A bug-fix commit message is read by someone who has never seen the bug: not today, by
the person who just fixed it, but months later, by someone who opened `git blame` on a
line and needs to know whether the reasoning behind it still applies to whatever they
are about to do. Write for that reader.

`comment-review` splits in two here, and the split is what decides which of its rules
reach a commit message.

Its **deletion bias** does not. The "one sentence, one fact" test, the rule that a
close call resolves to a cut, and the ban on walking the reader through mechanism all
exist because a comment sits next to code a competent reader can already read. The
reader of a commit message does not have the bug in front of them, and density hides
the mechanism instead of conveying it. Do not compress a bug-fix message to satisfy the
comment style guide. Length is fine. A subtle bug earns a long message, and indented
IR, code, and tables of steps are welcome wherever they carry more than prose would.

Its **`reference/claudish.md`** does. That pass is about how a sentence is built rather
than about whether the content stays, so it applies here unchanged. Run it over every
draft. "How the register goes wrong" below is what it catches in this shape of message.

This does not extend to a feature, a refactor, or a chore commit. Those describe what
changed and why it was worth doing, at whatever length that takes, and the tree's usual
terseness applies to them same as anywhere else. What follows is only for the commit
that fixes a defect.

## The shape

Build the message in this order. Each part answers a question the last one raises.

1. **The setting.** Name the mechanism and the data structure the bug lives in — the
   pass, the map, the table, the protocol — in enough detail that the next part makes
   sense to someone who has never opened the file. This is scene-setting, not the whole
   story: a sentence or two, or a short code excerpt, is usually enough.
2. **The invariant.** State the rule that made the original code correct. Not what the
   code does — what has to stay true for what it does to be right. This is usually the
   sentence a reviewer would need before they could have caught the bug themselves.
3. **What broke it.** Name the change, the added call, the new caller, or the case
   nobody considered that stopped the invariant from holding. Say why it looked fine —
   the invariant usually still holds on the path whoever wrote it was picturing.
4. **A worked example.** Trace real identifiers and real numbers through the broken
   path to the wrong answer. Not a description of the bug's shape in the abstract — an
   actual instance: an actual variable name, an actual IR snippet, an actual cost or
   count, followed step by step to where it goes wrong. This is the part that turns "I
   see how that could be wrong" into "I see that it is wrong, right here." If the
   codebase already has a minimal repro — a test fixture, an IR file, a small program —
   trace through that one instead of inventing a fresh example; a reader can then open
   it and follow along.
5. **Both symptoms.** A bug usually has more than one face, and they are worth naming
   separately because a search-and-check reader may only recognize one of them. A quiet
   face is a wrong answer nothing flags — the dangerous one, because nothing points at
   it. A loud face is a crash or an assertion — easier to find, and worth explaining
   why it stayed hidden anyway (an assertion compiled out, a check that only fires
   sometimes) if that is what happened.
6. **The fix.** What actually changed, stated against the invariant from step 2: what
   makes it hold again, and what it leaves alone. If the fix is narrower than "undo
   what step 3 did," say what it preserves and why the narrower version is enough.

Not every bug needs every part at length — a one-line off-by-one may need only a
sentence for the invariant and a short trace for the example. But do not drop a part
because the tree's other commit messages don't have it: a bug-fix message is judged
against this shape, not against the terser messages beside it in `git log`.

Before finalizing a draft, check it against each part in turn:

- [ ] Setting: the mechanism and data structure named
- [ ] Invariant: the rule that made the original code correct
- [ ] What broke it: the change or case that stopped the invariant holding
- [ ] Worked example: real identifiers and numbers traced to the wrong answer
- [ ] Both symptoms named, where the bug has more than one face
- [ ] Fix: stated against the invariant, including what it leaves alone

A part that is missing or thin is worth a second look before the message ships, not a
reason to pad the others.

## How the register goes wrong

A long message has more room for ornament, and these are the shapes it takes here.

**Leave the tests, the doc fixes and the repro steps out.** A bug-fix commit is
expected to add a case that fails without the fix, so saying it fails without the fix
reports the convention rather than the bug. The same goes for how many cases pass now,
for a doc comment the change corrected on its way past, and for the commands that
reproduce the fault. The diff carries all of it, and none of it is what a reader
opening `git blame` came for. Where an existing fixture is the clearest worked example,
trace through it and name it — that is step 4 doing its job, not a report on testing.

*X is the gate* must not appear at all. `comment-review`'s `reference/claudish.md`
bans the frame everywhere, and a commit message has no reason to name a test's role in
the first place.

**Do not correct an assumption the reader never made.** The commit context already says
this is a bug fix, and the reader has the diff open. They will not have supposed the
failure was a broken makefile, or that an intermittent fault was a race, so a clause
rejecting either is written for a reader who does not exist. State what happens and
stop:

| cut | keep |
| --- | --- |
| stops at `gensources failed (1)`, which reads like a broken makefile rather than a JIT fault | stops at `gensources failed (1)` |
| passes under `-mono-workers=1`, which is not a race: one worker compiles slowly enough that … | passes under `-mono-workers=1`, where one worker compiles slowly enough that … |
| the key does not change, so this aborts rather than answering wrongly | an assertions-off build computes the same key, so the guard changes only the abort |
| the abort stops the build, not a program | gensources runs on the in-tree runtime, so the class libraries stop |

A `rather than` clause almost never earns its place. The rejected half is the reader's
own default stated back to them, and restating an assumption is not correcting one.
Delete the contrast and keep the plain statement. A comparison against a measured
number is a different thing and stays: "-40 against the 60 the same shape costs with
nothing proven" is data.

**Cut the shapes `claudish.md` names.** Every one below came out of the model message's
own first draft:

- a conclusion already drawn, restated — "Two unrelated lists, and the number is read
  against the wrong one", after the sentence that had just said it
- an aphoristic ending — "That correspondence is the whole invariant"
- a cleft construction — "What the abort stops is the build", "strip_casts () is what
  breaks that". Use a plain subject and verb
- a metaphor standing in for a relationship the sentence never states — "has a louder
  face", "no such backstop"
- this tree's `answer` tic — "which is what getIndexTypeSizeInBits () answers" is
  "returns"
- emphasis carrying no fact — "an attribute that was about a different value entirely"

## The model to copy

`2627f652303b6d399fe14f37995ac6c1b6d80349` (`mono/llvm: keep the caller's own argument
out of a callee's null check`, `claude/inline-cost-caller-argument`) is the message that
prompted this skill and the one to reread when a draft feels too thin or too dense.
Reproduced here so the shape survives a rebase that changes the hash:

> When the tier-2 inliner weighs inlining Bar into Foo, CallAnalyzer walks
> Bar's body and adds up what it would cost, folding as it goes so that
> the dead side of a branch it settles costs nothing. It keeps a map,
> SimplifiedValues, from a value inside the callee to what that value is
> at this call site, seeded with each formal parameter mapped to the
> argument the caller passed:
>
>     SimplifiedValues[Bar's %o] = Foo's %p
>
> A null compare is one of the things it tries to settle, through
> isKnownNonNullInCallee ():
>
>     if (Argument *A = dyn_cast<Argument>(V))
>       if (CandidateCall.paramHasAttr(A->getArgNo(), NonNull))
>         return true;
>
> Upstream passes only the callee's own parameter here, so A->getArgNo ()
> indexes the callee's parameter list, CandidateCall is the call to that
> callee, and parameter N of the callee is argument N of the call.
>
> 5a7803f8033 also tries the settled value, so that a null compare on a
> mono.cast.isinst result reaches the object the type-test elimination
> substituted for the call:
>
>     Value *Settled = getSimplifiedValueUnchecked(I.getOperand(0));
>     if (isKnownNonNullInCallee(I.getOperand(0)) ||
>         (Settled && isKnownNonNullInCallee(Settled)))
>
> On a bare formal parameter that lookup gives back the caller's actual
> argument, and where the caller forwarded one of its own parameters that
> is an Argument belonging to the caller. Its getArgNo () indexes the
> caller's parameter list while CandidateCall is still the call to the
> callee, so the number is read against the wrong list.
>
> root_crossed_argument in ir/inline-cost-dom-chain.ll is that shape:
>
>     define ptr @raw_operand(ptr %unused, ptr %o) {
>       %isnull = icmp eq ptr %o, null
>       br i1 %isnull, label %dead, label %live
>     dead:
>       call void @expensive()            ; x3
>       ...
>     }
>
>     define ptr @root_crossed_argument(ptr %p) {
>       %arbitrary = call ptr @opaque_ptr()
>       %r = call ptr @raw_operand(ptr nonnull %arbitrary, ptr %p)
>       ret ptr %r
>     }
>
> Costing that inline, SimplifiedValues maps %o to %p. The direct try asks
> about %o, raw_operand's parameter 1, reads the call's argument 1, finds
> %p carries nothing and says no, which is right. The settled try asks
> about %p, root_crossed_argument's parameter 0, reads the call's argument
> 0, and finds nonnull %arbitrary. Index 0 meant "the caller's first
> parameter" and was read as "this call's first argument", so the guard
> folds and the three @expensive () calls leave the price. It prices -40
> against the 60 the same shape costs with nothing proven, and a negative
> cost inlines eagerly.
>
> The same number can also land past the end. root_late_argument passes
> its third parameter to a site taking two, so nothing sits at index 2 and
> paramHasAttr () asserts:
>
>     Assertion failed: ArgNo < arg_size() && "Param index out of bounds!",
>     llvm/lib/IR/Instructions.cpp, line 417
>
> That half is harmless with assertions off, because
> AttributeList::getAttributes () bounds-checks and returns an empty set
> for an index past its own list. Nothing catches the in-range half on any
> build, which is why this has been folding guards wrongly in CI and in
> players while only an assertions build said anything.
>
> The fix refuses a settled value that is an Argument of any function
> other than the callee. 5a7803f8033's substitution still works: for a
> type-test elimination the settled value is the callee's own operand, so
> its number is the callee's.

Read it against the six parts above: setting (`CallAnalyzer`, `SimplifiedValues`),
invariant (parameter N of the callee is argument N of the call), what broke it (the
settled-value lookup can resolve to the *caller's* own argument), the worked example
(`root_crossed_argument`, traced to `-40` against `60`), both symptoms (the silent
wrong-cost fold, and the out-of-bounds assertion), and the fix (refuse a settled value
that belongs to another function). It ends on the fix, with nothing after it.
`root_crossed_argument` earns its place as the worked example rather than as a test:
the message never says it is a new case, that it fails without the fix, or what else
in the suite passes.
