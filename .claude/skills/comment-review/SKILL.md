---
name: comment-review
description: Review the comments and doc comments in this tree's C/C++ sources against the house rules — cut what a caller cannot act on, catch false claims, move rationale to the line it justifies, and fix register. Use when asked to review, sweep, tighten or write comments in mono/llvm, mono/mini, mono/interp or the CMake files, or before committing a change that adds doc comments. Not a code review: it reads the comments, and the code only to check them.
---

# Comment review

The house rules are in `CLAUDE.md` ("Commenting Guidelines", "A comment is a claim",
"Register"). This skill is the procedure for applying them, plus the catalogue of what
people actually write instead. The full evidence — commits, pushback wording, worked
before/after pairs — is in `.claude/docs/comment-review.md`.

Read `reference/catalogue.md` before reporting anything. Read
`reference/calibration.md` when a finding feels borderline: it holds the before/after
pairs and the two fixtures that look like false positives and are not.

## The governing test

**Can the caller do something differently because of this sentence?** If not, cut it.
True, interesting, and relevant to the implementation are all failing grades.

Length is evidence against a comment, not for it. Every correction in the record made
the text shorter. A comment that teaches you a lot about the implementation is the
characteristic failure mode in this tree, not the goal.

**Most functions here want a one-line doc.** One line is the normal size, not the
degenerate case.

**When the call is close, cut.** The two errors are not symmetric and must not be weighed
as if they were. A comment cut wrongly costs one reader one trip to the code, which is
what they came to read anyway. A comment kept wrongly costs every reader after it, and it
is the kept ones that go stale, get restated in the next file, and have to be re-checked
by every sweep that follows. So a block you cannot argue for goes, and "I could not decide"
resolves to a cut.

**The default answer to "should this say more?" is no.** Before adding a sentence, ask
whether the code can carry the fact instead — a name, a type, an assert, a static
assertion. A comment is the last place to put a fact, not the first. Adding text is a
finding that has to clear a higher bar than a cut, not a safe middle course between
keeping and cutting.

This bias does **not** reach the four keeps: a quotation, a live hazard, a specification,
and rationale that still has code to justify. Those are decided by their own rules below,
and F1 and F2 are what happens when a bias runs over them.

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
touching the working tree; a stash puts the user's other work at risk for nothing.

### 1. Classify every block first

The name-grep cannot fire on two kinds of block, and those are the two that cost most
when cut. Classify before checking:

| kind | check | action |
| --- | --- | --- |
| **Claim** — names an identifier, file, section, pass, env var | grep every name | remove or fix what does not survive |
| **Quotation** — ECMA-335 and the like | none applies | **do not touch.** Move it if it is in the wrong file. Never shorten it |
| **Reason** — why a branch exists, why an order is what it is | read it against the code | cut only what the code now contradicts |
| **Specification** — a wire format, an ABI, a table layout | can a second implementation be written from it? | **add what is missing.** Economy is not the test here |

A sweep that runs only the name-grep and reports clean has produced a report that is
true and irrelevant at once — worse than a red, because a red gets investigated.

### 2. Pass 1 — is it false? (highest severity)

This is by a wide margin the most productive check.

- Grep every name the comment uses. A name with no definition is a delete, not a
  reword. The record's worst finds named a type with no definition anywhere and a
  function attributed to a file that has never been in the tree.

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

A one-line grep of the working tree answers none of those. And when a name turns out to
be real inside a sentence that is stale in some other way, **fix the stale part and keep
the name** — a compound false claim is not licence to delete the true clause with it.

**A replacement must still make a claim.** Rewriting a definite sentence into "X can
happen, and Y can erase it" is a way of being unfalsifiable rather than correct. If you
could not establish what actually happens, say so in the report and leave the comment
alone. Hedging is a worse outcome than either keeping or cutting.

That leave-it-alone covers a fact you could not check. It is not the answer to a block you
could not argue for, which the governing test cuts. Uncertain about the **fact**: write
nothing new. Uncertain about the **worth**: cut.
- Read the claim against **every path**, the early returns included. A sentence true on
  the main path and false on an early return is a false sentence, and worse than a
  missing one, because it reads as a guarantee.
- `every` / `each` / `all` / `always` / `never` claim a scope. What is it counted over,
  and does the code hold all of it? A doc promising a container holds many when the code
  puts one in it is a bug in the type — fix the type, not the sentence.
- A claim about a caller that this function does not enforce belongs to the caller.
- A fact tied to a transient setting — opt level, ISel, thread count — is already stale.

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
about something the function does. Written down next to its subject it is obviously
another thing's contract. This is the check that most often gets skipped, and the
enumeration is what stops that.

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
reads as valuable because you spent the effort, and that is where the governing test is
hardest to apply and most often skipped.

**Rationale moves, it does not vanish.** A doc comment arguing why the code takes one
approach is holding text the body wants. Move it to the line. The exception is a
debugging narrative: it does not clear the governing test, and moving it does not
launder it.

**Text you move is text you rewrite.** A sentence written for one home is wrong at the
next one: its subject, its trailing clause and its length were all chosen elsewhere. Run
the subject test again where it lands. A fact hoisted onto a function whose whole job is
that fact usually collapses into the summary — "Plants a label at \p at that emits no
code" rather than a paragraph about what an `EH_LABEL` is.

**Preconditions duplicate; conventions hoist.** A convention is one mechanism several
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
  *why*; a duty tells the caller what to write
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

Invoke the `simple-english` skill rather than working from memory of it. Descriptive
register.

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
  in that author's words. Leave its register alone; the register rules govern what the
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
  however long it is; that one is unambiguous and always worth fixing. A remark inside a
  body is `//` for a line or two and `/* */` once it runs to paragraphs, and **the middle
  is decided by the file**: `mono/llvm/` holds 90 multi-line `//` runs against 32
  single-paragraph `/* */` blocks, so there is no tree-wide answer to apply. Match what
  the file around you already does, and leave a uniform file alone
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

It lists semicolons joining clauses, banned modals, filler, empty subjects, every
UPPERCASE token, sentences over 25 words, and `/* */` blocks holding a single paragraph.

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

The procedure above is written for a diff. The rules hold when someone instead asks about a
single block, but the deletion bias is what gets dropped there, because the question
arrives shaped as "how should I word this" and answering it as asked has already conceded
that the text stays. Answer the cut first, then the wording.

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

Everything above applies, with four changes.

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

## Two traps that produced reverts

Both are the most costly failure mode, because a sweep that commits them reports
success.

1. **Deleting quoted standard text.** A sweep once deleted twenty verbatim ECMA-335
   passages for being long and restating the standard. Being the standard is why they
   are there. The one legitimate edit is to *replace* a quote that documents nothing
   local, with the part not in the standard.
2. **Dropping the "because" and keeping the description.** When a block is a reason and
   the code still needs it, that is the shortening that looks safest and destroys the
   value.

## A tic is a sweep, not a review comment

Grep a suspected house tic before filing it. `answers` for `returns` ran 63 times
against 157 uses of `returns` in `mono/llvm/`, which read as deliberate. Sixty instances
is a sweep and a style-guide entry; one review comment on one site is noise. That one has
since been swept — `reference/catalogue.md` G20 has the four senses of *answer* that are
not the tic and must not be re-flagged.

**Counting the word is not counting the tic.** The grep found 309 uses and 30 of them
were the defect. *Answers to*, *answers for*, *answers X with Y* and the plain noun are
all correct, and a sweep run off the raw count would have rewritten ten right sentences
for every wrong one. Narrow the pattern until it selects the defect, then count that.

This holds for a **convention** as much as for a word — comment markers, summary mood,
`\brief` against a bare first line. Where a file is uniformly on the older side of a
convention, converting all of it buries the substance findings in churn, and the diff
stops being reviewable.

The rule: **fix the convention in blocks you are already rewriting, and report the rest
as a sweep** with the count. A block you touched for a false claim gets its marker and
its summary mood fixed on the way past. A block you would otherwise not have opened does
not.

**The boundary is between files, not inside one.** If finishing the convention across the
file you are reviewing costs a handful of lines, finish it. Leaving five summaries
indicative and two imperative reads as carelessness, and the next reader cannot tell
which mood the file wants. Report a sweep when the rest of it lives in files you were not
asked about.
