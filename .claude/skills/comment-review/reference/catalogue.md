# The catalogue

What people write instead of what the rules ask for, ordered by how hard the pushback
lands. Every entry is a correction that was actually made in this tree.

**Being in the tree means nothing.** Most of the tree has never been swept and still
carries every pattern below. Do not calibrate on a neighbouring comment.

## Contents

- A. Saying something the reader already has
- B. Saying something that is not this code's business
- C. Justifying what is not there
- D. Claims that are false
- E. Register and shape
- F. The reverse pushback — cutting the wrong thing
- G. Written from the implementation, not for the caller

---

## A. Saying something the reader already has

The largest family.

### A1. Restating the code on the next line

A comment that only restates what the next line already shows has nothing left to say.
Cut it.

### A2. A summary above a quoted standard that says what the quote says

The quote is the better version of the rule, and two statements of one rule is one more
than can be kept true. Cut the summary.

### A3. A second copy of a fact that has a home

The two copies drift, and the one the reader finds first is the one they believe.

### A4. A convention restated once per file instead of hoisted

The same sentence opens file after file, each one a full copy of a rule that belongs in
one place. Hoist the mechanism to one block above the code that builds it, then cut every
restatement.

### A5. A convention split into pieces, none of which states the whole of it

The inverse of A4: no single comment states the whole mechanism — each function
documents only the fragment it needed. Hoist it to one block, and have every function
that needs it point there instead of reassembling it.

### A6. Saying what the thing is, where the name says it

A comment that says what a thing *is* is a claim about the name. Either the name carries
it, and the comment goes; or the name does not, and the fix is the name. A hazard, a
precondition, a lifetime rule, a reason survive — a name cannot carry any of them.

Two things make this one hard to catch. It fires on new code, where there is no stale
claim and no second copy to find, so passes 1 and 2 come back clean. And an earlier name
may have needed a comment that a later rename made redundant — the comment survives
because the question had shifted from whether to keep it to how to word it. Rerun the
governing test after a rename.

## B. Saying something that is not this code's business

### B1. Describing the mechanism where the contract belongs

Cut back to a summary, plus the facts a caller cannot see from the signature and would
otherwise get wrong.

### B2. Documenting a caller's policy as if this function guaranteed it

The subtlest one: a rule some caller happens to observe gets written on the callee as if
the callee enforced it, and reads as a guarantee that was never made.

### B3. Teaching the tool

Explaining what a third-party tool's own flag or feature does belongs in that tool's
manual, not in a comment here.

### B4. Naming who calls or who includes

Stale on the first new caller. A grep resolves it.

### B5. Bookkeeping that expires

A failure count or a wall time that the comment itself has to keep current goes stale
almost immediately. Cut it.

## C. Justifying what is not there

### C1. Arguing about attributes the code does not set

A reader cannot check an argument about an absent attribute against the code, because
none of it is in the code. Fixing the subject of such an argument is not enough — ask
whether the comment is needed at all (Pass 2's second question, SKILL.md).

### C2. Narrating the defect the test would catch

Describing the shape of the bug a test catches makes the reader reconstruct the defect
before they can see what the fixture actually is; describe the fixture instead and let
the test show the defect. The rationale for how a test is *built* stays — a helper that
exists specifically to block one optimization is why the code is shaped as it is, and
that belongs in the comment.

### C3. "Why not X"

Still the most common thing a fresh comment does.

## D. Claims that are false

### D1. Names that do not exist

A name in a comment — a type, a function, a flag — that does not exist in the code costs
the code nothing and costs every future reader a failed grep.

### D2. True on the main path, false on an early return

A claim that holds for the common case and fails on an early return is a false claim, not
a mostly-true one — check every path, not just the one that reads naturally.

### D3. Stale after a refactor

The doc kept describing behavior a refactor had already changed — sometimes to the
opposite of what it now claims.

### D4. Scope words that overclaim

*every*, *all*, *always* read as covering more than the code actually reaches — check
what the claim is counted over.

### D5. Tied to a transient implementation detail

A cost argument tied to today's codegen level, optimization setting, or thread count is
stale as soon as any of those change — and they do.

### D6. A described state that cannot happen

A comment can describe a state the code, once actually read, turns out to make
impossible — check that the described case can still occur.

### D7. The fact the author believes is in the comment

A comment can be defended by describing what its author believes it says rather than
what it actually says — the reasoning was worked out in the author's head and never
fully written down, which from the author's side reads exactly like a comment that
carries it. **Grep the comment for the fact you think it states before you argue from
it.** The check is one command, and the author is the one reader who cannot run it by
eye.

This failure has a second layer: a count in the comment can disagree with the list
beside it, because the list is not actually one kind of thing — some members support the
claim the count is attached to and others are there for an unrelated reason. **A count
that disagrees with a heterogeneous list is a mechanism error, not a numbering one.**
Read what the members are before correcting the number.

## E. Register and shape

**Circumlocution: a plain thing said by a roundabout route.** A periphrasis standing in
for a condition (*"the first time X"* for *"when X"*), an implication spelled out
instead of the fact itself, a premise already established restated as if it were new
(*"and nothing catches it"*), a nominalization standing in for a verb, a negation spread
over a set standing in for the direct claim about the whole (*"none of them says what
X looks like"* for *"no single comment states X"*) — one defect wearing many shapes. The table below only catalogues the shapes caught so far, and the
test generalizes past every row in it: **write the plainest sentence that loses no
fact, and compare.** If the plain version is shorter or clearer, ship it — this is
SKILL.md Pass 3's "say it out loud" test, run sentence by sentence. A row below is
evidence the shape recurs often enough to name. Its absence is not evidence a sentence
is clean; run the test regardless of whether the sentence matches a row.

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
| imperative summary | indicative, third person |
| hypotheticals ("what assigning the bit *would* do") | state the ordering fact directly |
| filler: simply, just, note that, essentially | delete |
| an em dash | cut the aside it carries, not the dash |
| "what X does not Y" | state it directly: "X does not Y" as a sentence, "Cases not covered by X" as a heading |
| "the first time X" for a plain condition | "when X" |
| "and nothing catches/notices/prevents it" | delete — the sentence is already about the case where nobody does |
| "none of them says X" / "no one of them says X" over a scattered set | say X directly: "no single Y states X" |

The em dash earns its own line because replacing it with " - " fixes the character and
keeps the defect. An author reaches for one when a sentence carries an aside, which is
the sentence the house style wants split — cutting the aside is what actually removes
the need for the dash, not swapping its punctuation.

## F. The reverse pushback — cutting the wrong thing

Both produced a revert. Both are the most costly failure mode, because a sweep that
removes them reports success.

### F1. Deleting quoted standard text

Verbatim standard text goes out because it is long and restates the standard. **Being
the standard is why it is there.** The one legitimate edit is to *replace* a quote that
documents nothing local, with the part not in the standard — what this backend does with
it, and why.

### F2. Deleting rationale

When a block is a reason and the code still needs it, the shortening that looks safest —
keeping the description, dropping the "because" — is the one that destroys the value.
Rationale in the wrong place gets **moved** to the line, not deleted. The exception is
G8.

## G. Written from the implementation, not for the caller

The family the sweeps never reached, because every member is true.

### G1. Circumlocution where the domain has a word

"Returns the address to call a method at" → "Returns the function pointer for this
method."

### G2. Facts offered instead of the decision they imply

The worst of the family, because it looks like thoroughness.

> "This is the same stub `mono_llvm_jit_compile_method ()` returns, so the two agree
> whichever asked first. The body behind the stub is compiled by the first call that
> arrives through it."
>
> → "This allocates a stub if not already created. If you need the method to be compiled
> immediately use `mono_llvm_jit_compile_method ()`, otherwise the method will be compiled
> when it is first called."

If the content amounts to "here is how it works, you work out when to use it", convert it
to "use this when X, otherwise use Y". Register note: the summary line stays indicative
third person. Guidance to the caller is imperative and can address them directly.

### G3. A cost stated in implementation units

"at the price of a symbol and a stub" is not a number and not a choice. Document a cost
only when the caller can spend it differently — and then give the quantity.

### G4. Documenting the default

Stating a property a reader already assumes — that memory is readable, that a value
starts at zero — adds nothing. State a property only when it differs from the default,
or when it changes; staying at the default is a non-event and does not deserve a
sentence.

### G5. Repeating in the body what the summary fixed

The summary's terms are in force for the rest of the comment.

### G6. Redundancy between paragraphs of one comment

Harder to catch than a duplicate across files, because the second copy is rephrased
rather than copied.

### G7. A trailing clause carrying a second fact

"A retired method keeps its bytes until the whole arena goes, which is when its domain
does." → "Individual allocations within this arena are not freed until the whole thing
is." One sentence, one fact.

### G8. Debugging archaeology

A comment that reads as "here is the bug we hit and how we dodged it" is presumed out
until it is shown to change what a caller does — set the prior at *rarely*. This does
not contradict `a-workaround-comment-names-a-live-hazard`: that is about how to **read**
such a comment when you find one — as an unfiled bug report — not a licence to write
more.

### G9. A type summary that describes the traffic instead of naming the type

"The code memory a domain's compiled methods and stubs come out of." → "An arena
allocator for executable memory."

The second form survived a rewrite that was consciously applying G9: "A queue of
compiles nobody waits for, and the thread that runs them." → "A background job queue for
compilation work." The class's **invariant** is not its name either. Name the category,
then state the invariant below it — or, per G15, not at all.

### G10. A "so …" clause that restates its own sentence from the other end

"The result is masked to less than type's width, so an amount at or past the width
wraps." → "The shift amount is taken modulo type's width."

Two clauses, one operation seen twice: the range of the output, then the effect on the
input. **If a sentence needs a consequence clause to land, it is usually one term
short.** Near relative of G1 and G7, and worth checking separately because it survives
both: G1 asks whether a term exists for a concept the sentence *describes*; G10 fires
when the sentence describes the concept **twice**, so each half looks like it is pulling
its weight.

### G11. A file doc that describes the problem and never says what the file does

The tell is a doc whose last sentence is still about the antagonist. One sentence of
*what*, none of *how* — "so this pass undoes the merge for function calls" is purpose;
"turns a branch back into the `ret` it was, in the blocks whose last instruction is a
marked call" is mechanism, and the body shows it.

### G12. Circling a name the code already has

*a call* plus *the marker* plus *a marked call*, where the IR says `tail call`.
**Precision by circumlocution reads worse than the plain name.**

### G13. Denying what the sentence before already excluded

"…only when a `ret` follows it in the same block. The marker alone does not do it."
*Only when* has already said it. Tell: a short second sentence built on *alone*, *by
itself*, *not enough*, *does not mean*.

*The extension.* Attach a rationale and the same clause reads as new information:

> "Returns the record for a dynamic method's stub. […] Returns null for every other
> method, whose record is owned by the domain and cannot be removed."
>
> → "Returns the record for a dynamic method's stub, and null otherwise."

**Amputate the because-half and read the clause alone.** If what is left restates the
sentence before, the clause goes, and the rationale must justify its own place from
scratch. It usually cannot: a reason for a fact nobody acts on is a reason nobody acts
on.

Keep the sentinel: `and null otherwise` names the value the caller branches on, which is
what lets them write `if (ji) remove (ji)` rather than re-deriving `method->dynamic` at
the call site.

*The complement written out long.* `for every other method` names a **set** where the
sentence before named a condition, so the reader has to intersect the two. `otherwise`
says it as a complement, so there is nothing to check. The one case needing the long form
is a first clause naming more than one condition — split that sentence instead.

### G14. The symptom of a bug this file prevents

"LLVM emits no diagnostic. The call stays a call, and the frame `tail.` promised to hand
away stays on the stack." Silent breakage is genuinely non-obvious, which is why this is
easy to miss. It still goes: **the reader who needs a symptom lives in the world where
the fix is absent, and this file is why that world does not exist.** A consequence
clause — "which breaks this" — carries everything a present-day reader can act on. Do
not extend this to a hazard that is still live: what goes wrong if a *future* change
removes an ordering or a lock is a constraint on the reader, and stays.

### G15. Arguing for a rule the type already enforces

An argument for a rule can be real, true, and still not belong, if the type already
makes the mistake impossible to make — if only a change to the class could break the
rule, a caller cannot break it by using the class wrongly, and the argument is not the
caller's business.

The correction to G14's carve-out, and easy to confuse with it. A live hazard stays when
the reader can **still walk into it**. A design rationale goes when the type makes the
mistake unreachable. The test is not "would removing this make the rule look arbitrary"
— that argument weighs one reader's need against a class doc every caller reads, and
loses. What survives is the part a caller can still get wrong.

### G16. A hazard the reader cannot evaluate

"Both must be called with no lock the work could want, the loader lock included." → "Do
not call them while holding the loader lock or a domain lock." *A lock the work could
want* asks the reader to enumerate what a compile takes, which is the thing they came
here to avoid doing.

Same check on *some callers*, *certain paths*, *the relevant lock*, *the appropriate
tier*. There are **two** repairs and the second is usually better:

> → "Do not call it while holding a lock."

Widening to everything is as evaluable as naming the members and costs less to maintain
— a named pair goes stale when the compile path grows a third lock, and the stale
version reads as permission. For a deadlock rule, over-restriction is the safe direction.
Name the members only when the wider rule would forbid something callers legitimately
do. **The failure is the middle**: a restricted set stated in terms the reader has to
resolve.

### G17. A precondition hoisted away from the call site

The hoist rule covers a convention several functions obey, not a precondition. A
convention is one mechanism the reader needs once and can be sent to. A precondition is
a rule obeyed at the call site, by someone looking at exactly one function.
**Preconditions duplicate.** That is not a defect: a precondition changing for one
caller and not another is a real possibility, unlike a wire format changing for one
writer.

### G18. A prohibition with the consequence left out

"Do not call it while holding a lock: the compile it waits for takes locks of its own."
→ "Calling this while holding the domain or loader lock can deadlock." **Prefer the
consequence when the reader can have a reason to do the thing anyway** — a rule alone
leaves someone with a genuine need to guess how hard it binds. It is also shorter:
*deadlock* is the whole of "the compile it waits for takes locks of its own".

Interaction with G16: this is the case where naming the members wins. With the
consequence stated, `a lock` is over-broad against a stated outcome rather than
conservatively safe.

### G19. The actor buried in a relative clause

"The order is the ranking `publish ()` compares, so a new tier goes between tier2 and
detoured." → "`publish ()` compares tiers by this order, so …"

Both are active voice. The first still hides the actor, because the grammatical subject
is an abstraction and the thing that acts is demoted into a relative clause with its
object deleted (*the ranking publish () compares* ← compares what?). **The test is not
"is there a `by`" but "is the actor the subject".** When a sentence opens on a noun the
code cannot execute — *the order*, *the rule*, *the mechanism*, *the reason* — find the
verb further in and promote its owner.

### G20. `answer` standing in for a plain relation, in any grammatical form

"Answers the record …" → "Returns the record …". A function does not answer, and
neither does a stub, a table or a proxy — each of the shapes below names one ordinary
relation, and none of them needed the word. **Grep a suspected tic before filing it as a
one-line fix**: this one runs into the hundreds across `mono/llvm/`, which makes it a
sweep and a style-guide entry rather than a review comment. The count sizes the fix. It
does not make the tic defensible, and neither does a different grammatical shape — an
earlier pass here carved out the three shapes below as "correct English" and stopped at
the literal return-value swap. That was wrong. Rewrite every shape:

| form | example | rewrite |
| --- | --- | --- |
| *answers X* (return value) | "section_address answers with the address" | "section_address returns the address" |
| *answers to* | "the stub already answers to the symbol" | "the stub is already known by the symbol" |
| *answers for* | "a proxy answers for the class it stands in for" | "a proxy covers the class it stands in for" |
| *answers X with Y* | "library methods the transform answers with an opcode" | "library methods the transform replaces with an opcode" |
| *the answer* | "a rule at the wrong offset unwinds to a wrong answer" | "a rule at the wrong offset unwinds to the wrong result" |

**A synonym is not a rewrite either.** A grep for this tic finds only the word `answer`,
and a correction can drift past it while keeping the same shape: `eliminate_type_tests
() ... answers the test for every class that slot admits` came back as `... settles the
test for every class that slot admits`, and a neighboring fix turned `a bound answers
nothing here` into `the parameter's bound decides nothing here`. Both keep exactly what
the rule bans — an abstraction (*the rule*, *a bound*) cast as an agent resolving a
yes/no question — under a word the grep does not know to look for. The test is not "does
this contain `answer`"; it is "is an abstract noun cast as answering, settling or
deciding a question". Say what actually happens instead: name the real actor and reuse
the function's own verb (`eliminate_type_tests () eliminates the test`), or state the
dependency plainly (`whether the array is IMarker[] depends on its actual element
class`, not `the element's class decides this`).

### G21. Documenting something other than this comment's own subject

A comment can be true and not padding, and still document the wrong subject —
explaining how a called function works above the code that calls it, or what a library
does above the code that uses it, rather than the subject its own position requires.

**The subject test applies to every comment, not just a function's doc comment.** A
comment's subject must match what its position allows. A doc comment's subject must be
the function, a parameter, or the return value. A file header's subject must be the file
— what it checks, for a test file, and what a failure means. Strike every sentence whose
subject is neither. What is left is the doc. An empty result means the block was never
about its own subject at all.

**Default to deletion, not relocation.** Check for an existing home before writing a new
one. Most sentences this check catches are re-deriving a fact that already has a home
elsewhere, not a fact that needs one — if the named subject's own doc comment already
states it, cut the copy rather than moving it anywhere.

Relocate a fact only when it is both real and has nowhere else to live:

- Give the named subject's mechanism its own home when none exists, or cut it when
  nothing needs it stated at all — a caller reasoning about the function in front of
  them never needs another function's internals explained.
- Put another component's behavior beside the decision it drives here, not beside the
  thing it merely happens to be true of.

This is G8's move, with one extra step: notice the sentence is about something else
before finding the line to put it beside — or, for a file header, before finding that
there is no line, only the file itself.

**Two shapes recur specifically in file headers, because both read as orientation
rather than as a claim.** Narrating how the pass under test reaches its cases — "the
front end gives it no IL of its own, which is the elimination's simplest input" — has
the pass for its subject, exactly like a doc comment explaining a called function's
mechanism above the call site (B1); the pass's own file is the home for that, if
anywhere, and the fix is still to cut, not to cite it. And a sentence contrasting this
file's coverage against a sibling test's — "eliminate-empty-finally-tests.cpp checks the
removal itself against hand-built IR; this file instead exercises a real compiled
try/finally/catch" — has that sibling for its subject. Both are true, both name
something real, and both survive Pass 1's grep and a read-by-eye untouched, which is why
they keep recurring: run the on-paper subject table (SKILL.md Pass 2) on the header
itself, not only on doc comments sitting above a declaration.

### G22. A verb that names the caller's decision, not this function's action

> "Refuses a call this method cannot make, and emits a throw in its place." → "Emits a
> throw of MethodAccessException in place of a call to \p callee."

The function itself refuses nothing — it emits a call, points the builder at a cold
block and returns success. The refusal happened in the caller. The doc borrowed the
caller's word and attached it to the function that cleans up after the decision, so
someone looking for the policy reads this and stops looking.

**The test is the return value.** A verb the return cannot support is the wrong verb —
*refuses* was falsified by the signature before anyone read the body. Where the return
is less telling, name the effect the body has on the world and compare.

**And the word can already be taken.** A word already claimed by a type or a function
name in this codebase, then reused loosely in prose for something else, covers at least
two unrelated mechanisms at once. That is the inverse of G20 and the worse half of
one-word-one-meaning: a synonym rotation costs a reader a moment, a collision costs them
a wrong model.

### G23. The second paragraph written because there is a second paragraph

A block being split into two paragraphs does not mean the second one earned its place —
every sentence in it still has to pass the same filters as the first.

**Two filters, in order.** The subject test runs first and takes the sentences about
other code. Then ask of each survivor whether it is contract or mechanism — a sentence
can name this function and still be describing how it works.

**And a paragraph that ends as one clause was never a paragraph.** The blank line
invites a paragraph, and a paragraph invites sentences to fill it. Put the clause in the
summary and delete the blank line rather than finding two more sentences to keep it
company.

### G24. Written as a defence, so it leads with the evidence and omits the decision

A constant's comment can cite the measurement that motivated it, and still mislead, if a
later judgment call moved the value away from what the measurement alone would pick —
the comment then cites, as authority, the very evidence the decision overrode, and the
next reader who trusts it "fixes" the constant back to what the evidence says.

The cause generalises past this shape, which is why it is worth a name: **the author
wrote the comment to defend the number against a reviewer rather than to inform a
reader.** A defence leads with evidence, because that is what a challenge asks for. A
reader needs the decision, because that is the part no amount of reading the code
recovers. The tell is a comment whose stated evidence and whose subject disagree — and it
is worse than saying nothing, since it hands the next reader an argument for the wrong
value.

The two questions that separate them: what does this comment lose if nobody ever
challenges the code, and can a reader derive it by reading? Evidence survives the first
and fails the second. A decision fails the first and survives it. Keep the part a reader
cannot derive. This is the constructive half of the advice rule in SKILL.md — a comment
that needs defending is usually failing the governing test, and a comment that is
genuinely needed is still written wrong when it is written as a defence.

Numbers belong to this too. A measurement pinned in a source file goes stale when
anything upstream moves. Put the counts in the plan document, where a stale number costs
nothing, and leave the file the sentence that stays true.

### G25. Bare `fold` for one of the two things this tree calls that

`fold` is banned on its own: this tree uses it for constant folding (`is` resolved from
what the IR already says about the operand) and, in older text, for inlining a callee
into its caller — two different mechanisms, one word. A reader who does not already know
which era wrote the sentence cannot tell which is meant. Say `constant fold` when that is
the one meant, and name the mechanism (`inline`, `eliminate`) otherwise. This is the same
failure as G20 one level out: the word itself, not a grammatical shape of it, is what
carries the ambiguity, so the fix is never a synonym — it is saying which of the two.
