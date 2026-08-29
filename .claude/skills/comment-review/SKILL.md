---
name: comment-review
description: Review the comments and doc comments in this tree's C/C++ sources against the house rules — cut every block that cannot be proven necessary, catch false claims, move rationale to the line it justifies, translate assistant-written prose back into plain English, and fix register. Use when asked to review, sweep, tighten, de-slop or write comments in mono/llvm, mono/mini, mono/interp or the CMake files, or before committing a change that adds doc comments. Not a code review: it reads the comments, and the code only to check them.
---

# Comment review

This skill holds the house rules, the procedure for applying them, and the catalogue of
what people write instead. `CLAUDE.md` points here and carries none of them. The full
evidence — commits, pushback wording, worked before/after pairs — is in
`.claude/docs/comment-review.md`.

Read `reference/catalogue.md` before reporting anything. Read
`reference/calibration.md` when a finding feels borderline: it holds the before/after
pairs and the two fixtures that look like false positives and are not. Read
`reference/claudish.md` when a block reads well and says little.

## The governing test

**Can the caller do something differently because of this sentence?** If not, cut it.
True, interesting, and relevant to the implementation are all failing grades. So is
useful. The test is necessity: the comment stays only where the code cannot carry the
fact and a reader without it gets something wrong.

**The burden of proof is on the comment.** Do not ask what a block gives a reader. Ask
which reader goes wrong without it and what they do, and answer with both. A block you
cannot make that case for goes, however well it reads and however much it taught you.

**The unit is the fact, not the block.** One sentence that passes the test carries no
other sentence with it, and a fact that passes carries no clause welded to it. Take the
block apart, run the test on each fact on its own, and keep the ones that pass. G23 is
what a block judged whole looks like: three sentences in one paragraph, each failing a
filter that already existed.

Length is evidence against a comment, not for it. Every correction in the record made
the text shorter. A comment that teaches you a lot about the implementation is the
characteristic failure mode in this tree, not the goal.

**Most functions here want a one-line doc.** One line is the normal size, not the
degenerate case.

**When the call is close, cut.** The two errors are not symmetric. A comment cut wrongly
costs one reader one trip to the code, which is what they came to read anyway. A comment kept wrongly costs every reader after it, and it
is the kept ones that go stale, get restated in the next file, and have to be re-checked
by every sweep that follows. So "I could not decide" resolves to a cut, and so does "it
seems helpful".

**The default answer to "should this say more?" is no.** Before adding a sentence, ask
whether the code can carry the fact instead — a name, a type, an assert, a static
assertion. A comment is the last place to put a fact, not the first. Adding text is a
finding that has to clear a higher bar than a cut, not a safe middle course between
keeping and cutting.

This bias does **not** reach the four keeps: a quotation, a live hazard, a specification,
and rationale that still has code to justify. Those are decided by their own rules below,
and F1 and F2 are what happens when a bias runs over them.

## The house rules

The tree's style guide. The passes below are how these get applied to a diff, and each
pass carries the diagnostics for the rules it enforces.

Comments are read by humans who know this codebase and know JIT compilation. Write for
them. Dense or cryptic comments that cannot be understood are not useful.

**Length.** Match it to what the thing needs. A subtle invariant is usually arguable in a
few sentences. If IR or pseudocode conveys the shape of a transform faster than prose,
use that instead. A wall of text is not more rigorous than a short one: past a certain
length it hides the one or two sentences that matter, which is worse than being terse.

**What a doc comment is for.** It states the contract: what the thing does, and what a
caller needs to use it correctly and safely. Explain *what*, not *how* — anyone
who needs the mechanism reads the implementation. These are internal docs, so a short
introduction plus whatever heads off a non-obvious misuse is enough.

What earns its place is what a caller cannot see from the signature and would otherwise
get wrong: a locking rule, a precondition, what NULL means, an operation that can
silently not happen, a lifetime or stability guarantee. Internal ordering, which helper
does the work, and why the function exists at all are none of the caller's business.
Cut them.

Before you keep a fact, make sure that this function is what enforces it. A rule some
caller observes belongs to that caller, and stating it here reads as a guarantee this
function makes. `mono_llvm_jit_compile_method ()` compiles into whatever domain it is
handed. That icall wrappers get handed the root domain is mini's policy, so mini
documents it.

**Where rationale goes.** Beside the line it justifies. A doc comment that argues why the
code takes one approach is holding text the body wants: move it down, do not erase it.
The doc then keeps the contract and the body keeps the argument. Moving it usually
sharpens it too, because next to the code you can say which case it is about. Comments
inside a method are otherwise minimal, and explain *why*.

**One home per fact.** A convention several functions obey gets one block above the code
that builds it, not a piece in each doc comment. The mono vararg cookie was spelled out
in five places, each carrying the part its own function needed, and none of them said
what the buffer looks like. Hoist the mechanism, then cut every restatement. A comment
that points at the home stays. The same rule covers restating the signature, the file
extension, or who calls a function: two copies disagree eventually, and the copy a
reader finds first is the one they believe.

**Do not write:**
- Archeology. A comment about deleted or legacy code goes stale the next time the code
  moves.
- A reference to the current plan or task list. For a later reader without the plan
  documents, these hide what is actually going on.
- An explanation of what is *not* happening. Justify the code that is there. Do not
  narrate the absence of some other mechanism, unless that absence is itself the
  non-obvious thing a reader needs to trust the code.

**Do not write a count the reader cannot check from the sentence.** "Its whole surface is
sixteen functions" is wrong the next time someone adds one, and a reader who doubts it has
to leave the document to find out. Say what bounds the set instead — "keep that surface
small" is the actual rule, and the count only ever stood in for it. A count is fine when
the same sentence enumerates what it counts, as in "the three places a call arrives:" and
then the three. A fourth site then contradicts the list in the place someone adding one is
already typing. This is the one claim grepping a name does not catch, because the number
greps clean while the set moves under it.

**Quoted standards are never rewritten.** Several files carry the ECMA-335 Partition III
passage for the opcode they emit, verbatim, and that is deliberate. It is the normative
text the code has to satisfy, and it belongs next to the code that satisfies it.
Summarising one turns the thing you check against into a paraphrase that nothing checks.
These style rules govern what we write about the code, not the quote. If a block is
genuinely in the wrong file, move it. Do not shorten it.

Where a quoted block documents the function, it is the whole doc comment. Do not add a
summary above an emitter saying what the passage below already says. Add a comment only
for what the standard does not cover: what this backend does with the instruction, which
local table governs it, or why it departs from the text.

**A comment is a claim.** Treat every comment that names something — an identifier, a
file, a section, a pass, an environment variable — as an assertion to be checked, and grep
it before you keep it. This holds when writing as much as when reviewing: assert a
mechanism only after you observe it, because where a cheap observation exists it beats
reasoning about what the code probably does. Pass 1 is where a review runs the check.

## The tree is not the standard

**The style guide outranks the comments in the tree, always.** A block is measured against
the house rules below and the references beside this file, and against nothing else. Most of the
comments here were written by an assistant and most of them fail those rules, so the tree
is a body of defects rather than a body of examples.

**Never keep a block, a phrasing or a convention because the code around it does the same
thing.** Sixty instances of a defect are sixty findings, not a house style. A file that
breaks a rule from top to bottom is wrong from top to bottom. A block an earlier sweep
left standing is not evidence either — it was reviewed under whatever these rules said
then, or not reviewed at all.

A grep over the tree says how wide a defect is, which decides whether the fix is one edit
or a sweep. It never decides whether the thing is a defect.

## Procedure

### 0. Scope

Take the target the user named — a file, a directory, a diff. If they named nothing,
review the working-tree diff. Read the whole file, not the hunks: a duplicated fact and
a hoistable convention are both invisible in a hunk.

Never rebuild and never run ctest for a comment-only change. Prove it by preprocessing
the edited file against the committed one:

```bash
git show HEAD:<path> > /tmp/before.cpp
gcc -fpreprocessed -dD -E -P /tmp/before.cpp > /tmp/a.i
gcc -fpreprocessed -dD -E -P <path>        > /tmp/b.i
diff /tmp/a.i /tmp/b.i && echo "comment-only"
```

**Never `git stash` to get the old text.** `git show HEAD:<path>` reads it without
touching the working tree. A stash puts the user's other work at risk for nothing.

### 1. Classify every block first

The name-grep cannot fire on two kinds of block, and those are the two that cost most
when cut. Classify before checking:

| kind | check | action |
| --- | --- | --- |
| **Claim** — names an identifier, file, section, pass, env var | grep every name | remove or fix what does not survive |
| **Quotation** — ECMA-335 and the like | none applies | **do not touch.** Move it if it is in the wrong file, never shorten it. The one legitimate edit is to replace a quote documenting nothing local with the part not in the standard (F1) |
| **Reason** — why a branch exists, why an order is what it is | read it against the code | cut only what the code now contradicts. Keeping the description and dropping the "because" is the shortening that destroys the value (F2) |
| **Specification** — a wire format, an ABI, a table layout | can a second implementation be written from it? | **add what is missing.** Economy is not the test here |

A sweep that runs only the name-grep and reports clean has produced a report that is true
and irrelevant at once, and nobody investigates a clean report.

### 2. Pass 1 — is it false? (highest severity)

This is by a wide margin the most productive check.

- Grep every name the comment uses. A name with no definition is a delete, not a
  reword (D1).
- Read the claim against **every path**, the early returns included. A sentence true on
  the main path and false on an early return is a false sentence, and worse than a
  missing one, because it reads as a guarantee. `code_address_symbol ()` promised that a
  call through the pointer it hands back uses this backend's convention, which holds for a
  method this backend compiles and fails on the early return above it, where a no-wrapper
  icall's published address is a registered C function.
- `every` / `each` / `all` / `always` / `never` claim a scope. What is it counted over,
  and does the code hold all of it? A doc promising a container holds many when the code
  puts one in it is a bug in the type — fix the type, not the sentence.
- A claim about a caller that this function does not enforce belongs to the caller.
- A fact tied to a transient setting — opt level, ISel, thread count — is already stale.

**A verification that comes back negative is a claim too, and it is the dangerous one.**
"I grepped and found nothing" is what deletes a true fact, and nothing downstream catches
it — the sentence is gone and the diff looks like a cleanup. Before you delete a name as
nonexistent, satisfy yourself you looked where it lives:

| the name is | where it lives |
| --- | --- |
| an LLVM type, field or parameter | `~/projects/llvm-project/install/include`, not this repo |
| a file the comment says used to do something | `git log --all --diff-filter=D -- '*<name>'` |
| a macro | it can be defined under an `#if` you did not compile |
| a build fact | `build/compile_commands.json`, and the CMake files |

A one-line grep of the working tree answers none of those. Where a name is real inside a
sentence that is stale in some other way, **fix the stale part and keep the name**. A
compound false claim is not licence to delete the true clause with it.

**A replacement must still make a claim.** Rewriting a definite sentence into "X can
happen, and Y can erase it" is a way of being unfalsifiable rather than correct. If you
could not establish what actually happens, say so in the report and leave the comment
alone. Hedging is a worse outcome than either keeping or cutting.

That leave-it-alone covers a fact you could not check. It is not the answer to a block you
could not argue for, which the governing test cuts. Uncertain about the **fact**: write
nothing new. Uncertain about the **worth**: cut.

### 3. Pass 2 — does it belong?

Run these in order. Each is a faster way of failing the governing test.

**The subject test.** Strike every sentence whose grammatical subject is not this
function, one of its parameters, or its return value. What is left is the doc. If
nothing is left, the block documented some other thing.

Do this on paper, not by eye. For each doc comment, write out the sentences with their
subjects before you decide anything:

```
record_ranges:
  s1 "Bracket each maximal run …"        -> this function      KEEP
  s2 "An EH_LABEL emits nothing but …"   -> EH_LABEL           STRIKE, move
```

Read by eye, the second sentence looks like it belongs, because it is true and it is
about something the function does. Written down next to its subject it is another thing's
contract.

**Run it on doc comments only.** A remark sitting at a line is *about* the code under it,
so its subject is routinely something else — that is what rationale is, and the subject
test deletes it every time. `// An argument a throw helper reports is often a boxed
value.` above `case MONO_CEE_BOX:` has a throw helper for its subject and is the reason
that opcode is on the list. Judge a remark by whether the line under it looks arbitrary
without it, never by its subject.

**Then, of each survivor: contract or mechanism?** A sentence can name this function and
still be describing how it works. Mechanism goes down to the line it explains.

**Then the cuts.** Full diagnostics and examples in `reference/catalogue.md`:

- restates the next line, the signature, or the summary
- a second copy of a fact that has a home elsewhere — point at the home, or say nothing
- a convention restated per file instead of hoisted to one block above the code
- documents the default: a reader assumes code memory is readable. Comment the surprise
- states facts where it could state the decision → "use this when X, otherwise use Y"
- a cost in implementation units — "at the price of a symbol and a stub". Document a
  cost only when the caller can spend it differently, and then give the quantity
- argues about code that is not there ("why not `nounwind`") — a reader cannot check it
- narrates a bug that was hit and dodged. **Presumed out.** Keep the rule, not the story
- the symptom of a bug this code prevents. The reader who needs it lives in the world
  where the fix is absent. A **live** hazard is different and stays
- argues for a rule the type already enforces — if only a change to the class could
  break it, the argument is not the caller's business

**Then ask the second question.** Every fix here — a subject corrected, a stale clause
repaired, a mechanism moved down — leaves a survivor that has never been read cold. Read
it cold and ask whether it was wanted at all. C1 went twice: the first round corrected the
subject, the second deleted what the first round wrote. A block you have just rewritten
reads as valuable because you spent the effort.

**Rationale moves, it does not vanish.** A doc comment arguing why the code takes one
approach is holding text the body wants. Move it to the line. The exception is a
debugging narrative: it does not clear the governing test, and moving it does not
launder it.

**Text you move is text you rewrite.** A sentence written for one home is wrong at the
next one: its subject, its trailing clause and its length were all chosen elsewhere. Run
the subject test again where it lands. A fact hoisted onto a function whose whole job is
that fact usually collapses into the summary — "Plants a label at \p at that emits no
code" rather than a paragraph about what an `EH_LABEL` is.

**Preconditions duplicate. Conventions hoist.** A convention is one mechanism several
functions build — the reader needs it once. A precondition is obeyed at the call site by
someone looking at exactly one function, so it goes on each function it constrains.

### 4. Pass 3 — does it say its fact cleanly?

Different fault from Pass 2, and cheaper to test, so run it first on any sentence you
are keeping. A sentence can earn its place and still be assembled wrong.

**Say the sentence out loud, as if to a colleague. If the restatement is shorter or
sharper, ship the restatement.** This has found a defect every time it has been run, on
sentences that had already survived several passes. The tell that it fired: your
restatement contains a term the comment did not (*modulo*), or it leads with what the
comment buried (*do not dereference this*).

**Then run the same test over the whole block.** What Claudish is, and why the other
passes cannot see it, is in `reference/claudish.md`. Read it before running this.

Write out the smallest set of ordinary propositions the block states, then read the block
against that list. Whatever the block has and the list has not is ornament, and ornament
is deleted rather than paraphrased. Do not write one sentence for each sentence you read.
The four moves are below. Each one over-fires without the carve-out beside it in that
file:

- collapse sentences that restate one proposition through a second abstraction, a
  metaphorical label, or a contrast with an alternative nobody believes
- lower the abstraction: ordinary verbs over nominalizations. "Only the domain lock
  orders these two" over "ordering here is domain-lock-gated"
- delete the scaffolding — staged emphasis (*the key distinction*, *the deeper point*),
  orientation (*in other words*, *put differently*) and aphoristic endings (*that is the
  boundary*)
- decode a compound — *X-gated*, *X-backed*, *X-side* — into the relationship it stands
  for, where the sentence never states that relationship

**A compression must not strengthen the claim.** *Required* is not *sufficient* and *not
tested* is not *wrong*. A shorter sentence that claims more than the long one is a Pass 1
finding against your own edit, and `reference/claudish.md` has the table of the swaps that
do it.

- Use the domain's word. "Returns the address to call a method at" is a circumlocution
  for *function pointer*. A summary that ends on a preposition is the tell
- Use the name the IR or the source uses — `tail call`, not *a call* plus *the marker*
- A "so …" clause that restates its own sentence from the other end is one term short.
  Tell: a mechanism word in the first clause (*masked*, *stored*), a consequence in the
  second
- A trailing clause carrying a second fact: split or cut. One sentence, one fact
- A short second sentence built on *alone*, *by itself*, *not enough* denies what the
  sentence before already excluded. Cut it. If a rationale is welded on, amputate the
  rationale and read the clause alone — the reason then has to earn its own place.
  **Check first that the welded clause is a reason and not a duty.** *…and null
  otherwise, which has to be taken back out again when the method is freed* looks like
  the same shape, and is a caller obligation: the record really must reach
  `mono_jit_info_table_remove ()`. Amputating a duty deletes contract. A reason answers
  *why*, and a duty tells the caller what to write
- A complement spelled out as a set — *for every other method* — is `otherwise`
- The subject is a noun the code cannot execute (*the order*, *the rule*, *the reason*):
  find the verb further in and promote its owner. Active voice is not enough on its own
- A hazard stated in a category the reader cannot resolve — *a lock the work could
  want*, *some callers*, *certain paths* — is not a rule. Name the members, or widen the
  rule to everything. The failure is the middle
- A prohibition with the consequence left out. Name the outcome — *can deadlock* — and
  the rule falls out of it, and the mechanism clause then goes
- A verb phrase standing in for a named function — *take it back out*, *hand it off*.
  Name the call
- Read the summary's verb against the return value. A verb the return cannot support is
  the caller's word: *refuses* on something that returns success
- A verb this codebase has given to a type (*refuse* against `SharingRefusal`) is a
  collision, which is worse than a synonym — it hands the reader a wrong model
- A body paragraph that survives as one clause was never a paragraph. Put the clause in
  the summary and delete the blank line

### 5. Pass 4 — register (lowest severity)

Comments here are written in ASD-STE100 Simplified Technical English, descriptive
register. Invoke the `simple-english:simple-english` skill rather than working from
memory of it. This is not a style preference: the constraints strip out exactly the
padding that makes a comment take three reads.

- Modals: `can`, `will` and `must` in an indicative sentence. `should` and `might` are
  hedges — say what happens, or say in the report that you could not establish it.
  **`would` and `could` are correct in a counterfactual and stay there**: *Without this,
  the dispatcher would be renamed to the method it dispatches for* cannot be said any
  other way, and rewriting it indicative asserts that the renaming happens. The tell is a
  governing clause — *without this*, *left alone*, *rather than*, *otherwise*, *if it
  were*. Measured over one sweep, nine of ten `would`s sat in one of those and every
  "fix" to them was a revert. `may` is the same: real possibility about runtime state
  (*the vtable might not be initialized yet*) is a fact, not a hedge
- **A FIXME or a TODO is not documentation.** It is a note to whoever picks the work up,
  in that author's words. Leave its register alone. The register rules govern what the
  code's documentation claims
- A **function** summary starts with a verb, indicative, no parenthetical, no "The one
  X". The one carve-out is a predicate: `Whether mbb leaves inside the clause` is the
  house form for a function returning `bool`, and is not a defect. A **type** summary is
  a noun phrase naming the kind of thing — not a relative clause describing what passes
  through it, and not the type's invariant
- A **file** doc is `\file` then `\brief` then one sentence of what the file is for. It
  never opens with the file's own name: the reader has the path
- Parameters in `\param`, lower case in prose. Never UPPERCASE
- No semicolons joining clauses. Sentences under 25 words. No `-ing` as a **verb** (a
  gerund subject — "Calling this while holding the lock" — is a noun phrase and is fine)
- Name the actor. No "nothing here" or "nothing else". No "load-bearing" — say what
  breaks
- Ask **doc or remark** before you ask anything about length. A doc comment sits above a
  declaration and is `///`, or `/** */` when it runs to paragraphs — never `/* */`,
  however long it is. That one is unambiguous and always worth fixing. A remark inside a
  body is `//`, and `/* */` only once it runs to several paragraphs. What the file around
  you already does decides nothing: fix the marker in the blocks you are rewriting anyway,
  and report the rest as a sweep with the count
- Filler out: simply, just, note that, essentially, basically, obviously
- **Do not flag plain passives.** The diagnosis is almost always circumlocution instead,
  and the preferred rewrites in the record use passives freely

**A register fix must not change what the sentence claims.** Re-read every sentence you
touched in this pass against Pass 1 before you keep it. A register nit is the cheapest
finding in the document, and turning one into a false claim is the most expensive.

Two ways it goes wrong. Removing `would` drops a conditional, so an indicative rewrite
states as fact something the code does on one path only. And swapping the subject to
name the actor can empty the sentence: *A marker is the whole transfer function* says
which fact drives the dataflow, while *transfer () is each block's transfer function*
restates the name of the function on the next line. If your rewrite is true of any code
with that name, you deleted the content.

### 6. Run the mechanical check — before reporting, and again after editing

Register defects do not survive eyeballing. Run the script:

```bash
.claude/skills/comment-review/scripts/register-check.sh <file>...
```

It lists semicolons joining clauses, the hedging modals and the conditional ones on
separate lines, filler, empty subjects, rhetorical scaffolding, every UPPERCASE token,
sentences over 25 words, and `/* */` blocks holding a single paragraph.

The scaffolding line finds the phrases that have a fixed form. The Claudish shapes that
cost most — one proposition stated three ways, a contrast against an alternative nobody
believes — have no fixed form, so a clean run there says nothing about the block.

**Every hit is a candidate, not a verdict.** The UPPERCASE line mixes real acronyms with
parameter names — `CLAUSE MBB MI NOT WOULD` against `DWARF LLVM PEI` — and a long
sentence inside quoted standard text stays. Read each one.

On sentence length, weight by how badly it reads: a 26-word sentence that parses in one
go is a nit, and a 45-word one whose relative clause has come apart is a defect. Fix the
long ones and the ones in blocks you are rewriting anyway.

**Run it a second time on the edited file.** This is where a rewrite that introduced a
semicolon, or an indicative that lost a conditional, shows up.

### 7. Report

Order findings by severity: false, then does-not-belong, then shape, then register.
Stop reporting register nits on a block whose substance is wrong — fix the substance and
the nit usually goes with it.

For each finding give the file and line, the quoted text, which check fired, and the
replacement text. A finding with no replacement is not finished. **A cut is a complete
finding** — an empty replacement is the commonest one here, and a shorter rewrite offered
where the block does not survive the governing test is an unfinished cut.

Say what you left alone and why when a block looks wrong and is not — a quotation, a
live hazard, a specification that reads long because a second implementer needs it.

**Do not edit unless the user asked for the fix.** Where they did, apply the edits and
then run the preprocess identity check above.

## Advice about one comment

The procedure above is written for a diff, and the rules hold for a single block as well.
What gets dropped there is the deletion bias: the question arrives shaped as "how should I
word this", and answering it as asked has already conceded that the text stays. Answer the
cut first, then the wording.

- **"How do I stop a sweep cutting this?"** A comment that needs defending is failing the
  governing test already. A live hazard needs no defence: it names what breaks, and every
  carve-out here keeps it. So the first answer is that the fact belongs where the code
  enforces it — a type, an assert, a name — and the second is that the comment goes.
  Growing it to survive the next reader is the last resort, not the first.
- **"Is this doc missing something?"** Missing is one of two findings: a claim that is
  false by omission (Pair 5), or a specification a second implementer cannot work from.
  Anything else that reads as missing is the block asking to be shorter.
- **An addition has to say what it buys.** Name the reader and the mistake they make
  without it. "They read the code" is not a mistake.

## CMake files

Everything above applies, with the changes below.

**The marker rules do not.** CMake has one comment marker. A comment above a
`function ()`, `macro ()` or `option ()` is a doc comment and gets the doc rules — verb
first, contract not mechanism, `\param` has no counterpart so name the arguments in
prose. Everything else is a remark and gets the remark rules. A **separator banner**
(`# -------`) carries nothing and goes.

**The identity proof is different.** There is nothing to preprocess. Prove the change is
comment-only by reading the diff:

```bash
git diff -U0 -- <path> | grep -E '^[-+]' | grep -vE '^(\+\+\+|---)' |
  grep -vE '^[-+][[:space:]]*(#|$)'
```

Anything that prints is a line you changed that is not a whole-line comment. A trailing
comment on a code line prints too, so read what comes out rather than requiring silence.

**The names to grep are different, and this is where the yield is.** A build file's
comments name variables, cache variables, targets, test labels, options, generated files
and paths. Every one of those is greppable, and a build system that has been rewritten
carries comments describing the one before it. Check:

| the name is | where it lives |
| --- | --- |
| a variable or cache variable | `git grep -n '<name>' -- '*.cmake' '*CMakeLists.txt'` |
| a target | the `add_*` call that makes it, anywhere in the tree |
| a test or a label | `ctest --test-dir build -N`, and `--print-labels` |
| a file the build writes | the `add_custom_command` `OUTPUT` that writes it |
| an autotools artefact | `configure`, `Makefile.am` and `autogen.sh` are **gone** |

**The house failure mode here is teaching CMake.** A comment explaining what
`set (... PARENT_SCOPE)` does, what a generator expression is, or how ctest picks tests
fails the governing test: the reader has the manual. Keep what is local — why *this*
build makes that choice, which upstream defect a flag works around, what a magic number
was measured at.

Run `scripts/register-check-cmake.sh <file>...` instead of `register-check.sh`.

## A tic is a sweep, not a review comment

Grep a suspected house tic before filing it. `answers` for `returns` ran 63 times in
`mono/llvm/`. Sixty instances is a sweep and a style-guide entry. One review comment on
one site is noise. The count decides the shape of the fix. It never decides whether the
thing is a defect: that one was, and it has since been swept.
`reference/catalogue.md` G20 has the four senses of *answer* that are not the tic and
must not be re-flagged.

**Counting the word is not counting the tic.** The grep found 309 uses and 30 of them
were the defect. *Answers to*, *answers for*, *answers X with Y* and the plain noun are
all correct, and a sweep run off the raw count would have rewritten ten right sentences
for every wrong one. Narrow the pattern until it selects the defect, then count that.

This holds for a **convention** as much as for a word — comment markers, summary mood,
`\brief` against a bare first line. A file can break one of these from top to bottom, and
that makes it a wide defect rather than a local style. What bounds the fix is churn:
converting every block in a file you opened for one false claim buries the substance
findings, and the diff stops being reviewable.

The rule: **fix the convention in blocks you are already rewriting, and report the rest
as a sweep** with the count. A block you touched for a false claim gets its marker and
its summary mood fixed on the way past. A block you would otherwise not have opened does
not.

**The boundary is between files, not inside one.** If finishing the convention across the
file you are reviewing costs a handful of lines, finish it. Leaving five summaries
indicative and two imperative reads as carelessness. Report a sweep when the rest of it
lives in files you were not asked about.
