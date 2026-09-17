---
name: bug-fix-commits
description: "Writes bug-fix commit messages in this repo's house style. Use when drafting or reviewing the commit message for a bug fix, not a feature, refactor, or chore commit."
---

# Bug-fix commit messages

A bug-fix commit message is read by someone who has never seen the bug: not today, by
the person who just fixed it, but months later, by someone who opened `git blame` on a
line and needs to know whether the reasoning behind it still applies to whatever they
are about to do. Write for that reader.

This is not the register `comment-review` governs. That skill's "one sentence, one
fact" test, its bias toward cutting, and its ban on walking the reader through
mechanism all exist because a comment sits next to code a competent reader can already
read. A commit message is the one place in this repo where the opposite is true: the
reader does not have the bug in front of them, and density hides the mechanism instead
of conveying it. Do not compress a bug-fix message to satisfy the comment style guide.
Length is fine. A subtle bug earns a long message, and indented IR, code, and tables of
steps are welcome wherever they carry more than prose would.

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

## The model to copy

`aacac3523ba81f30e53e005c42aed64a17b98663` (`mono/llvm: keep the caller's own argument
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
> Upstream passes only the callee's own parameter here. So A->getArgNo ()
> indexes the callee's parameter list, CandidateCall is the call to that
> callee, and parameter N of the callee is argument N of the call. The
> number always means what it is read as. That correspondence is the whole
> invariant.
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
> callee. Two unrelated lists, and the number is read against the wrong
> one.
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
> 0, and finds nonnull %arbitrary. The guard folds and the three
> @expensive () calls leave the price, on an attribute that was about a
> different value entirely. Index 0 meant "the caller's first parameter"
> and was read as "this call's first argument". It prices -40 against the
> 60 the same shape costs with nothing proven, and a negative cost inlines
> eagerly.
>
> The same mistake has a louder face when the number lands past the end.
> root_late_argument passes its third parameter to a site taking two, so
> nothing sits at index 2 and paramHasAttr () asserts:
>
>     Assertion failed: ArgNo < arg_size() && "Param index out of bounds!",
>     llvm/lib/IR/Instructions.cpp, line 417
>
> That half is harmless with assertions off, because
> AttributeList::getAttributes () bounds-checks and returns an empty set
> for an index past its own list. The in-range half has no such backstop
> and reaches every build, which is why this has been folding guards
> wrongly in CI and in players while only an assertions build said
> anything.
>
> The fix refuses a settled value that is an Argument of any function
> other than the callee. What 5a7803f8033 bought is untouched: for a
> type-test elimination the settled value is the callee's own operand, so
> its number is the callee's and the invariant holds. The three cases
> already in that file still pass.
>
> Both new gates were checked to fail with the guard taken back out,
> root_crossed_argument on the -40 and root_late_argument on the
> assertion. The gensources compile that aborted five of five now runs
> five of five with byte-identical output.

Read it against the six parts above: setting (`CallAnalyzer`, `SimplifiedValues`),
invariant (parameter N of the callee is argument N of the call), what broke it (the
settled-value lookup can resolve to the *caller's* own argument), the worked example
(`root_crossed_argument`, traced to `-40` against `60`), both symptoms (the silent
wrong-cost fold, and the louder out-of-bounds assertion), and the fix (refuse a settled
value that belongs to another function). The closing paragraph about the two failing
gates and the byte-identical gensources output is what was verified — worth keeping in
the message as evidence the fix works, but it is not one of the six parts: it reports
on the fix rather than explaining the bug.
