# The catalogue

What people write instead of what the rules ask for, ordered by how hard the pushback
lands. Every entry is a correction that was actually made in this tree. The full record,
with the commits behind each, is `.claude/docs/comment-review.md`.

**Being in the tree means nothing.** The sweep covered `mono/llvm/method-to-llvm/`,
`jit.cpp/hpp`, `mono_lsda*`, `runtime.h(pp)` and the interp test corpus. Everything else
— `runtime/backend.hpp`, `passes/`, most of `mono/mini/` — was never swept and still
carries the banned patterns. Do not calibrate on a neighbouring comment.

---

## A. Saying something the reader already has

The largest family.

**A1. Restating the code on the next line.** `native.cpp` is on the next line and
`ENABLE_EXPORTS` on the one after, so "native methods for the P/Invoke tests" was the
whole of what the comment had to say. A search does not need a paragraph saying what its
result is for (72 lines to 18).

**A2. A summary above a quoted standard that says what the quote says.** Nineteen
emitters carried one: what `add` does on overflow, which exceptions `div` throws. The
quote is the better version of all of it, and **two statements of one rule is one more
than can be kept true.**

**A3. A second copy of a fact that has a home.** The two copies drift, and the one the
reader finds first is the one they believe. The `.mono_lsda` byte table was written out
in both the header and the `.cpp`, and by then the two disagreed about what an entry
describes.

**A4. A convention restated once per file instead of hoisted.** Twenty-eight files
opened with the same sentence `runner.cpp`'s header already states. The inverse failure:
the vararg cookie was spelled out in five places, each carrying the part its own function
needed, and **no one of them said what the buffer looks like.** Hoist the mechanism to
one block above the code that builds it, then cut every restatement.

## B. Saying something that is not this code's business

**B1. Describing the mechanism where the contract belongs.** Cut back to a summary, then
only what a caller cannot see from the signature and would otherwise get wrong (150 lines
to 59).

**B2. Documenting a caller's policy as if this function guaranteed it.** The subtlest
one. That icall wrappers compile into the root domain is `mini-runtime.c`'s policy; the
backend compiles into whatever it is handed, so the rule **read as a guarantee that is
not made**.

**B3. Teaching the tool.** What `--rerun-failed` does is ctest's, and a comment in one
`CMakeLists.txt` is not where anyone learns it.

**B4. Naming who calls or who includes.** Stale on the first new caller. Grep answers it.

**B5. Bookkeeping that expires.** A failure count and a wall time were going to be wrong
within a week.

## C. Justifying what is not there

**C1. Arguing about attributes the code does not set.** Four allocator declarations spent
most of their comment on why there is no `allockind`, why not `AllocFnKind::Zeroed`, why
the call is not `nounwind`. **A reader cannot check any of that against the code, because
none of it is in the code.**

Note the second round: the replacement text went too, because "the reader can see the
attribute go on, and what it means there is plain." The first pass corrected the
*subject*; the second asked whether the comment was needed at all. Ask the second
question.

**C2. Narrating the defect the test would catch.** "so an index computed from the first
argument's width is wrong from the second one on" **makes the reader build the defect
before they can see the intent**, and the test below already shows the shape. "Sixteen
arguments alternating int and long, so the stride changes at every index" says it in one
read. The rationale for how a test is *built* stays — a `NoInlining` helper exists so the
transform cannot fold the operand away, and that is why the code is shaped as it is.

**C3. "Why not X."** Still the most common thing a fresh comment does.

## D. Claims that are false

**D1. Names that do not exist.** `MonoLSDAStreamer` (nothing in `mono/llvm` has Streamer
in its name, attributed four times in one header), `MonoSeqPointFlags` (not a type — the
flags are `MONO_SEQ_POINT_FLAG_*` defines), `emit_resume_unwind` (it is
`emit_resume_exit`). None cost the code anything and each cost every future reader a
failed grep.

**D2. True on the main path, false on an early return.** `code_address_symbol ()`
promised that a call through the pointer it hands back is an ordinary call in this
backend's convention. It fails on the early return above it: a no-wrapper icall's
published address is the registered C function, and a call to that one is C.

**D3. Stale after a refactor.** `method_override_for ()` stopped building the record and
its doc kept saying it does. `register_symbol ()` was documented as idempotent with "the
first address wins"; it returns an error — the opposite.

**D4. Scope words that overclaim.** "where every instrumented function's counters landed"
reads as the whole program and is false — it is this method's counters.

**D5. Tied to a transient implementation detail.** Arguing a cost from FastISel tied the
comment to today's codegen level. The prediction came true: tier 2 now runs codegen at an
optimizing level.

**D6. A described state that cannot happen.** A comment covered "a compile that captured
no object if gdb registration was turned on after this JIT started"; `gdbjit::enabled ()`
is a function-local static and latches on its first call.

**D7. The fact the author believes is in the comment.** A `WeakVH` field was defended
against a future sweep on the grounds that its doc said why a raw pointer would dangle and
named the two ways the value goes stale. The doc said when the value goes null, and named
one of them. The reason had been worked out and never written down, which from the author's
side reads exactly like a comment that carries it. **Grep the comment for the fact you
think it states before you argue from it.** The check is one command, and the author is the
one reader who cannot run it by eye.

The repair found the worse half. The field said "the two ways" over a list of three, and
the three are not the same kind of thing: one path frees the copy, one is the null-for-root
case the paragraph above already states, and one leaves the copy **live in another module**.
So the sentence reached for a list to justify a claim about `WeakVH` when most of that list
is about a different test. **A count that disagrees with a heterogeneous list is a mechanism
error, not a numbering one.** Where the count is wrong, read what the members are before
correcting the number.

## E. Register and shape

Lower severity individually. Corrected in bulk.

| defect | correction |
| --- | --- |
| `should`/`would`/`may`/`might`/`could` | `can`, `will`, `must`, or restructure |
| summary opens with a noun or a bare parameter name | open with a verb |
| UPPERCASE parameter names (`METHOD`, `AT`) | ordinary lower-case prose |
| parameter defined in a sentence of the description | a `\param` entry |
| parenthetical in the summary line | split or narrow the summary |
| "The one X …" opener | say what the thing is |
| passive with the agent right there | name the actor |
| "nothing here", "nothing else" | say "we", or name the thing |
| "load-bearing" | say what breaks |
| semicolon joining two clauses | two sentences |
| sentences over 25 words | split |
| `/* */` for a one-line remark | `//`, and `///` for a doc |
| imperative summary among indicative neighbours | match the file |
| hypotheticals ("what assigning the bit *would* do") | state the ordering fact directly |
| filler: simply, just, note that, essentially | delete |
| an em dash | cut the aside it carries, not the dash |

The em dash earns its own line because replacing it with " - " fixes the character and keeps
the defect. An author reaches for one when a sentence carries an aside, which is the sentence
the house style wants split. The same author put one back in the same file an hour after the
first was removed, and the second time cutting the aside made both dashes unnecessary.

## F. The reverse pushback — cutting the wrong thing

Both produced a revert. Both are the most costly failure mode, because a sweep that
removes them reports success.

**F1. Deleting quoted standard text.** Twenty verbatim ECMA-335 passages went out on the
grounds that they were long and restated the standard. **Being the standard is why they
are there.** The one legitimate edit is to *replace* a quote that documents nothing local:
150 lines of Partition III sat above `emit_prefix ()` describing six prefixes to a reader
who has never met them, and what replaced it is the part not in the standard — which
prefixes are read elsewhere, and by whom.

**F2. Deleting rationale.** When a block is a reason and the code still needs it, the
shortening that looks safest — keeping the description, dropping the "because" — is the
one that destroys the value. Rationale in the wrong place gets **moved** to the line, not
deleted. The exception is G8.

## G. Written from the implementation, not for the caller

The family the sweeps never reached, because every member is true.

**G1. Circumlocution where the domain has a word.** "Returns the address to call a method
at" → "Returns the function pointer for this method."

**G2. Facts offered instead of the decision they imply.** The worst of the family, because
it looks like thoroughness.

> "This is the same stub `mono_llvm_jit_compile_method ()` returns, so the two agree
> whichever asked first. The body behind the stub is compiled by the first call that
> arrives through it."
>
> → "This allocates a stub if not already created. If you need the method to be compiled
> immediately use `mono_llvm_jit_compile_method ()`, otherwise the method will be compiled
> when it is first called."

If the content amounts to "here is how it works, you work out when to use it", convert it
to "use this when X, otherwise use Y". Register note: the summary line stays indicative
third person; guidance to the caller is imperative and may address them directly.

**G3. A cost stated in implementation units.** "at the price of a symbol and a stub" is
not a number and not a choice. Document a cost only when the caller can spend it
differently — and then give the quantity.

**G4. Documenting the default.** "A reservation is readable, writable and executable, and
stays that way." Readable and writable are assumed; executable was already in the summary;
"stays that way" documents a non-event. Permissions *changing* would deserve the sentence.

**G5. Repeating in the body what the summary fixed.** The summary's terms are in force for
the rest of the comment.

**G6. Redundancy between paragraphs of one comment.** Harder to catch than a duplicate
across files, because the second copy is rephrased rather than copied.

**G7. A trailing clause carrying a second fact.** "A retired method keeps its bytes until
the whole arena goes, which is when its domain does." → "Individual allocations within
this arena are not freed until the whole thing is." One sentence, one fact.

**G8. Debugging archaeology.** The pushback, verbatim:

> "This more or less all seems to be implementation details and not relevant to
> implementors. I think the only useful bit is along the lines of 'this class takes
> `mono_domain_lock ()` internally'. Otherwise this looks like a narration of problems run
> into while debugging. While solutions to debugging issues can sometimes (quite rarely)
> make for useful comments, most do not meet this bar."

Set the prior at *rarely*. A comment that reads as "here is the bug we hit and how we
dodged it" is presumed out until it is shown to change what a caller does. This does not
contradict `a-workaround-comment-names-a-live-hazard`: that is about how to **read** such
a comment when you find one — as an unfiled bug report — not a licence to write more.

**G9. A type summary that describes the traffic instead of naming the type.** "The code
memory a domain's compiled methods and stubs come out of." → "An arena allocator for
executable memory."

The second form survived a rewrite that was consciously applying G9: "A queue of compiles
nobody waits for, and the thread that runs them." → "A background job queue for
compilation work." The class's **invariant** is not its name either. Name the category,
then state the invariant below it — or, per G15, not at all.

**G10. A "so …" clause that restates its own sentence from the other end.** "The result is
masked to less than type's width, so an amount at or past the width wraps." → "The shift
amount is taken modulo type's width."

Two clauses, one operation seen twice: the range of the output, then the effect on the
input. **If a sentence needs a consequence clause to land, it is usually one term short.**
Near relative of G1 and G7, and worth checking separately because it survives both: G1
asks whether a term exists for a concept the sentence *describes*; G10 fires when the
sentence describes the concept **twice**, so each half looks like it is pulling its
weight.

**G11. A file doc that describes the problem and never says what the file does.** The tell
is a doc whose last sentence is still about the antagonist. One sentence of *what*, none
of *how* — "so this pass undoes the merge for function calls" is purpose; "turns a branch
back into the `ret` it was, in the blocks whose last instruction is a marked call" is
mechanism, and the body shows it.

**G12. Circling a name the code already has.** *a call* plus *the marker* plus *a marked
call*, where the IR says `tail call`. **Precision by circumlocution reads worse than the
plain name.**

**G13. Denying what the sentence before already excluded.** "…only when a `ret` follows it
in the same block. The marker alone does not do it." *Only when* has already said it. Tell:
a short second sentence built on *alone*, *by itself*, *not enough*, *does not mean*.

*The extension.* Attach a rationale and the same clause reads as new information:

> "Returns the record for a dynamic method's stub. […] Returns null for every other
> method, whose record is owned by the domain and cannot be removed."
>
> → "Returns the record for a dynamic method's stub, and null otherwise."

**Amputate the because-half and read the clause alone.** If what is left restates the
sentence before, the clause goes, and the rationale must justify its own place from
scratch. It usually cannot: a reason for a fact nobody acts on is a reason nobody acts on.

Keep the sentinel: `and null otherwise` names the value the caller branches on, which is
what lets them write `if (ji) remove (ji)` rather than re-deriving `method->dynamic` at
the call site.

*The complement written out long.* `for every other method` names a **set** where the
sentence before named a condition, so the reader has to intersect the two. `otherwise`
says it as a complement, so there is nothing to check. The one case needing the long form
is a first clause naming more than one condition — split that sentence instead.

**G14. The symptom of a bug this file prevents.** "LLVM emits no diagnostic. The call
stays a call, and the frame `tail.` promised to hand away stays on the stack." Silent
breakage is genuinely non-obvious, which is why this survived several passes. It still
goes: **the reader who needs a symptom lives in the world where the fix is absent, and
this file is why that world does not exist.** A consequence clause — "which breaks this" —
carries everything a present-day reader can act on. Do not extend this to a hazard that is
still live: what goes wrong if a *future* change removes an ordering or a lock is a
constraint on the reader, and stays.

**G15. Arguing for a rule the type already enforces.** Six sentences argued that no thread
may wait on a background compile. The deadlock is real and every sentence is true. It
still goes, because **`CompileQueue` has no wait to call** — `enqueue ()` returns `bool`
and hands back no handle, so a caller cannot break the rule by using the class wrongly,
only by changing it.

The correction to G14's carve-out, and easy to confuse with it. A live hazard stays when
the reader can **still walk into it**. A design rationale goes when the type makes the
mistake unreachable. The test is not "would removing this make the rule look arbitrary" —
that was the defence offered, and it weighs the modifier's needs against a class doc every
caller reads. What survives is the part a caller can still get wrong: here, the waits that
**do** exist, and the lock a caller must not hold across one.

**G16. A hazard the reader cannot evaluate.** "Both must be called with no lock the work
could want, the loader lock included." → "Do not call them while holding the loader lock
or a domain lock." *A lock the work could want* asks the reader to enumerate what a
compile takes, which is the thing they came here to avoid doing.

Same check on *some callers*, *certain paths*, *the relevant lock*, *the appropriate
tier*. There are **two** repairs and the second is usually better:

> → "Do not call it while holding a lock."

Widening to everything is as evaluable as naming the members and costs less to maintain —
a named pair goes stale the first time the compile path grows a third lock, and the stale
version reads as permission. For a deadlock rule, over-restriction is the safe direction.
Name the members only when the wider rule would forbid something callers legitimately do.
**The failure is the middle**: a restricted set stated in terms the reader has to resolve.

**G17. A precondition hoisted away from the call site.** The hoist rule covers a
convention several functions obey, not a precondition. A convention is one mechanism the
reader needs once and can be sent to. A precondition is a rule obeyed at the call site, by
someone looking at exactly one function. **Preconditions duplicate.** That is not a
defect: a precondition changing for one caller and not another is a real possibility,
unlike a wire format changing for one writer.

**G18. A prohibition with the consequence left out.** "Do not call it while holding a lock:
the compile it waits for takes locks of its own." → "Calling this while holding the domain
or loader lock can deadlock." **Prefer the consequence when the reader might have a reason
to do the thing anyway** — a rule alone leaves someone with a genuine need to guess how
hard it binds. It is also shorter: *deadlock* is the whole of "the compile it waits for
takes locks of its own".

Interaction with G16: this is the case where naming the members wins. With the consequence
stated, `a lock` is over-broad against a stated outcome rather than conservatively safe.

**G19. The actor buried in a relative clause.** "The order is the ranking `publish ()`
compares, so a new tier goes between tier2 and detoured." → "`publish ()` compares tiers by
this order, so …"

Both are active voice. The first still hides the actor, because the grammatical subject is
an abstraction and the thing that acts is demoted into a relative clause with its object
deleted (*the ranking publish () compares* ← compares what?). **The test is not "is there
a `by`" but "is the actor the subject".** When a sentence opens on a noun the code cannot
execute — *the order*, *the rule*, *the mechanism*, *the reason* — find the verb further in
and promote its owner.

**G20. A synonym for `return`.** "Answers the record …" → "Returns the record …". A
function does not answer, and a reader deciding whether *answers* means something
different from *returns* has been given work for nothing. **Grep a suspected tic before
filing it as a one-line fix**: this one ran 63 times against 157 uses of `returns`, which
made it a sweep and a style-guide entry rather than a review comment.

**The sweep has been done.** What is left is not the tic, and re-flagging it is the
mistake to avoid now. `answer` carries four other senses here, all of them correct:

| form | means | example |
| --- | --- | --- |
| *answers to* | is known by that name | "the stub already answers to the symbol" |
| *answers for* | is responsible for, covers | "a proxy answers for the class it stands in for" |
| *answers X with Y* | handles X by doing Y | "library methods the transform answers with an opcode" |
| *the answer* | the noun | "a rule at the wrong offset unwinds to a wrong answer" |

Only a **function's return value** takes the fix. The test: name the function and the
value. *"section_address answers with the address"* passes, and *"the jit-info table
answers for a compiled entry"* does not — the table is not returning the entry to anyone,
it is the thing that covers that case.

**G21. Documenting something other than this function.**

> `register_code_stub ()`: "A stub pushes nothing, so the frame at any instruction in one
> is still the caller's. That is what the arch CIE describes …"
>
> `count_return_registers ()`: "LLVM flattens an aggregate return into its scalar leaves
> and gives each leaf a register, so the count of leaves … is what decides the demotion."

Neither is false and neither is padding. They document the wrong subject: the first
explains how a *stub* works, the second what *LLVM* does.

**The subject test.** Strike every sentence whose grammatical subject is not this function,
one of its parameters, or its return value. What is left is the doc. If that is empty, the
block was never about this function at all.

The repair is not deletion, because both facts are real:

- Mechanism of the subject → its own home, or nowhere. A caller of `register_code_stub ()`
  never reasons about a stub's prologue.
- Behavior of another component → beside the decision it drives.
  `returns_by_hidden_pointer ()` counts leaves instead of bytes *because* LLVM flattens, so
  the flattening belongs there, where someone about to replace the count with a size
  threshold is standing. On the counter it is inert.

Same move as G8, with the extra step that finding the line means first noticing the
sentence is about something else.

**G22. A verb that names the caller's decision, not this function's action.**

> "Refuses a call this method cannot make, and emits a throw in its place." → "Emits a
> throw of MethodAccessException in place of a call to \p callee."

`emit_method_access_failure ()` refuses nothing. It emits a call, points the builder at a
cold block and returns `llvm::Error::success ()`. The refusal happened in the caller. The
doc borrowed the caller's word and attached it to the function that cleans up after the
decision, so someone looking for the policy reads this and stops looking.

**The test is the return value.** A verb the return cannot support is the wrong verb —
*refuses* was falsified by the signature before anyone read the body. Where the return is
less telling, name the effect the body has on the world and compare.

**And the word is taken.** `SharingRefusal` is a type here and `sharing_refusal` a
function. Around seventy prose uses of *refuse* sit on top of that and cover at least two
unrelated mechanisms. One word standing for two mechanisms is the inverse of G20 and the
worse half of one-word-one-meaning: a synonym rotation costs a reader a moment, a collision
costs them a wrong model.

**G23. The second paragraph written because there is a second paragraph.** Three sentences,
each failing a filter that already exists:

> "The caller decides that \p callee is inaccessible, and this does not test that again."
> — subject is *the caller*. G21.
>
> "Translation continues from a new cold block, so the evaluation stack keeps its shape and
> a path that never reaches this instruction still runs." — first clause is contract with
> the actor demoted (G19). The rest is why a cold block, which is G8: it belongs at
> `SetInsertPoint`.
>
> "The returned error means the throw could not be emitted." — passes the subject test and
> says what every other `llvm::Error` in the emitter says.

**Two filters, in order.** The subject test runs first and takes the sentences about other
code. Then ask of each survivor whether it is contract or mechanism — a sentence can name
this function and still be describing how it works.

**And a paragraph that ends as one clause was never a paragraph.** The blank line invites a
paragraph, and a paragraph invites sentences to fill it. Put the clause in the summary and
delete the blank line rather than finding two more sentences to keep it company.

**G10. Written as a defence, so it leads with the evidence and omits the decision.** A
constant's comment gave the measurement behind it — fold counts on two corpora at four
depths — and the measurement picks 4 where the code says 8. The reason for 8 was a judgment
call to err toward inlining, and it was in a chat message and a handoff document rather than
in the file. So the comment cited as authority the very evidence the decision overrode, and
the next reader who trusts it "fixes" the constant back.

The cause generalises past this shape, which is why it is worth a name: **the author wrote
the comment to defend the number against a reviewer rather than to inform a reader.** A
defence leads with evidence, because that is what answers a challenge. A reader needs the
decision, because that is the part no amount of reading the code recovers. The tell is a
comment whose stated evidence and whose subject disagree — and it is worse than saying
nothing, since it hands the next reader an argument for the wrong value.

The two questions that separate them: what does this comment lose if nobody ever challenges
the code, and can a reader derive it by reading? Evidence survives the first and fails the
second; a decision fails the first and survives it. Keep what a reader cannot derive. This is
the constructive half of the advice rule in SKILL.md — a comment that needs defending is
usually failing the governing test, and a comment that is genuinely needed is still written
wrong when it is written as a defence.

Numbers belong to this too. A measurement pinned in a source file goes stale the first time
anything upstream moves and nothing catches it. Put the counts in the plan document, where a
stale number costs nothing, and leave the file the sentence that stays true.
