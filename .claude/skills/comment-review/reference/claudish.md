# Claudish

The register an assistant writes by default: polished, contrast-heavy, metaphor-led, and
stating one proposition several times at different levels of abstraction. Adapted from
the translation protocol at
`https://github.com/programasweights/claudish/blob/main/specs/claudish-to-english.md` —
the sections below are the ones that protocol names, restated with this tree's own
examples, plus the carve-outs the rest of this skill already protects.

**No other pass catches it.** A Claudish block is true, so Pass 1
finds nothing. Its subject is this function, so Pass 2 finds nothing. Its modals are
indicative, so Pass 4 finds nothing. What is wrong is the ratio between the block and
what it says, and only a paraphrase measures that.

## Contents

- The test
- Prefer semantic compression
- Rewrite at the lowest useful level of abstraction
- Remove Claudish rhetorical structure
- Decode structural and process metaphors
- Preserve logical scope exactly
- Decompress technical compounds
- Normalize over-formal register
- Preserve legitimate terminology
- Perform a visible rewrite
- Cases not covered by this pass
- Worked pair

---

## The test

Write out the smallest set of ordinary propositions the block states. Then read the block
against the list. Whatever the block has and the list has not is ornament.

Ornament is **deleted, not paraphrased**. Replacing a staged contrast with a shorter
staged contrast keeps the defect and hides it.

The paraphrase is the whole check, so do it in writing, next to the block, the way Pass 2
enumerates subjects. A restatement held in the head reads as a summary of the block
rather than a rival to it.

Do not write one sentence for each sentence you read. A five-sentence block that states
one proposition becomes one sentence.

## Prefer semantic compression

Claudish states one idea several times through different abstractions. Each of these
collapses into the first statement of the claim:

- emphasizing it without adding to it
- attaching a metaphorical label to it
- dramatizing it
- contrasting it with an alternative nobody believes
- summarizing a conclusion already drawn
- redescribing the same relationship one level up

If deleting a clause changes no fact, condition, permission, or degree of certainty,
delete it.

The near relatives already in the catalogue are G5, G6, G10 and G13. Each of those names
one shape. This is the general case, and it fires on a block whose sentences are each
defensible in isolation.

## Rewrite at the lowest useful level of abstraction

Prefer ordinary verbs and direct relationships to rhetorical framing, nominalizations,
and system metaphors — the simplest phrasing that stays accurate.

| prefer | over |
| --- | --- |
| "Only the domain lock orders these two." | "Ordering here is domain-lock-gated." |
| "Do not publish the body until every symbol resolves." | "Symbol resolution is a mandatory publication requirement." |
| "The counter says the body is hot." | "The counter provides evidence of hotness." |

## Remove Claudish rhetorical structure

Delete these outright rather than paraphrase them. A simpler replacement is still
ornament if the original carried no fact:

- staged emphasis — *the key distinction*, *the deeper point*, *the honest answer*, *the
  cleanest way to see this*, *the real question*, *the load-bearing constraint*
- redundant orientation — *in other words*, *put differently*, *in one sentence*, *to be
  clear*
- aphoristic endings — *that is the boundary*, *that distinction matters*, *and that is
  the constraint*
- a contrastive frame built to be rejected — *not X, but Y*, where X is only the
  reader's own default assumption stated back to them. A contrast that corrects a real
  surprise is different, and is rare — see "Preserve legitimate terminology" below
- a claim restated in fresh vocabulary one sentence later
- *X is the gate*. An assistant tic, and it must not appear anywhere: a comment, a
  commit message, a plan, a handoff, a review report. The frame announces a role the
  file it sits in already carries and says nothing about what the fixture holds. Say
  what it holds, or cut the sentence. Its relatives go with it — *X is what gates
  this*, *X is the thing that catches it*, *that is the gate*
- *X answers Y*, and any abstract noun cast as **settling** or **deciding** a
  question. The second assistant tic, banned in the same places as the first. A
  function returns, a table covers, a stub is known by a symbol, a rule holds — each
  of those is an ordinary relation the word stood in for. `reference/catalogue.md`
  G20 has every grammatical shape with its plain rewrite, and the note there that a
  synonym is not a rewrite: *settles* and *decides* keep the shape under a word the
  grep does not find. Name the real actor and reuse its own verb

## Decode structural and process metaphors

A word standing in for a relationship the sentence never states is ornament. Recover the
relationship and say it plainly, choosing the simplest reading the surrounding block
supports rather than swapping from a fixed dictionary. Most of this list is this tree's
own vocabulary rather than metaphor — "Preserve legitimate terminology" below is the
other half of the judgment call:

| the word | decodes to |
| --- | --- |
| *X-gated*, *gated on X* | X is required, or must happen first |
| *owner-gated*, *approval-gated* | only an owner may do it, or sign-off is required — neither role exists in this tree's code |
| *hard gate*, *hard boundary*, *hard stop* | a strict requirement or blocker |
| *load-bearing* | essential — say what breaks without it |
| *handoff* | one thread, phase or tier passing something to the next |
| *spine* | the central structure |

## Preserve logical scope exactly

Be careful decoding a restriction, a prerequisite, a trigger or a dependency: nothing
here should get stronger or broader than the block that produced it.

| the input says | the rewrite must not say |
| --- | --- |
| do X when Y happens | X happens only when Y |
| X requires Y | X is defined by Y |
| Y must happen first | Y is why X happens |
| required | sufficient |
| not tested | wrong |
| has not started | is in progress |
| only owners may publish | what a non-owner may do |

Where the block is ambiguous, keep the narrowest reading the surrounding code supports.
A shorter sentence that claims more than the long one is a Pass 1 finding against your
own edit.

## Decompress technical compounds

*X-gated*, *X-backed*, *X-side*, *X-level*, *X-first*, *X-safe* and the noun stacks
beside them name a relationship without stating it. Recover the actual relationship and
prefer the verb over the invented noun: "release requires approval," not "an
approval-gated release path."

## Normalize over-formal register

The original protocol's list — *frontier*, *regime*, *trajectory*, *headline*,
*confirmatory*, *clears*, *survives* — is research-report vocabulary this tree's
comments essentially never carry. Its own version of the same drift is procedural:
*canonical*, *the source of truth*, *authoritative* for a value that already has one
obvious owner. Simplify it the same way — say the plain claim the fancier word stood in
for — and keep the word where it names a real technical distinction rather than dressing
up an ordinary one.

## Preserve legitimate terminology

The word lists above are diagnoses, not substitutions. Half the vocabulary that marks
Claudish elsewhere is this domain's own name for the thing: a fast **path**, a **cold**
block, the **surface** `runtime.h` publishes, a ctest **gate**, a commit that **landed**,
profile **drift** across a rebuild. Each of those is the clearest name for the thing, and
rewriting one costs a reader the term the code uses (G12).

**Gate** appears on that list as a noun. Nothing on the list licenses the frame *X is
the gate*, which is banned outright above.

Rewrite a metaphor only where it stands in for a relationship the sentence never
states — where the sentence states it, the metaphor stays.

**A contrast has to earn its place, not just be true.** Being factual about which half
really happens is not the bar — the governing test still applies to a `rather than`
clause the same as anywhere else, and the burden of proof is on the comment. Keep one
only when the actual behavior is something a reader would not expect by default, to the
point that it has to be said: "a fatal error rather than a stub" passes, because nothing
about the name suggests a stub was ever on the table. Most contrasts do not clear this.
The rejected half is usually the reader's own default assumption, stated back to them
only to make the preferred half land — restating an assumption is not correcting one,
and the fix is to cut the whole contrast and keep the plain statement. Default to
deleting it. A contrast surviving this is rare, not usual.

And a shape that fires across many files is a sweep, not a review comment. SKILL.md's tic
rule governs: narrow the pattern until it selects the defect, count that, and report the
rest.

## Perform a visible rewrite

Do not swap a handful of Claudish words while keeping the original's structure. When it
applies:

- reduce the sentence count
- collapse redundant clauses
- lower the abstraction level
- turn nominalizations into verbs
- remove a contrast built to be rejected
- replace a metaphor with the relationship it stands for
- remove emphasis that adds no fact

The result should read like someone just said what the block means, and it is fine —
often better — for that to come out shorter than what it replaced.

## Cases not covered by this pass

The four keeps are decided by their own rules and none of them is Claudish:

- a **quotation** — never shortened, whatever it reads like (F1)
- a **specification** — a second implementer has to work from it, so completeness beats
  economy
- a **live hazard** — a rule the reader can still walk into (G14, G15)
- **rationale** with code still to justify — it moves to the line, it does not compress
  to nothing (F2)

## Worked pair

`mono/mini/mini-generic-sharing.c`, above the signature the `gsharedvt` out wrapper
builds. The tree still carries the *before*, so this pair is proposed rather than landed:

Before:

```c
/*
 * The call goes to the method's own entry point, so it is made under the
 * method's own signature. Describing it with anything else is not merely
 * an unread argument: a hidden return pointer sits behind the first
 * argument, so an extra one moves it into a register the callee never
 * reads.
 */
```

After:

```c
/*
 * The call goes to the method's own entry point, so it is made under the
 * method's own signature. An extra argument moves the hidden return
 * pointer into a register the callee never reads.
 */
```

*Not merely an unread argument* is a rejected alternative built to make the second half
land, and the second half lands on its own. The propositions are two — the signature has
to match, and a mismatch misplaces the return pointer — and both survive.
