# Claudish

The register an assistant writes by default: polished, contrast-heavy, metaphor-led, and
stating one proposition several times at different levels of abstraction. Adapted from
the translation protocol at
`https://github.com/programasweights/claudish/blob/main/specs/claudish-to-english.md`,
narrowed to comments and fitted to the carve-outs this tree already has.

**It is the family the other passes cannot see.** A Claudish block is true, so Pass 1
finds nothing. Its subject is this function, so Pass 2 finds nothing. Its modals are
indicative, so Pass 4 finds nothing. What is wrong is the ratio between the block and
what it says, and only a paraphrase measures that.

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

## The four moves

**1. Collapse restatement.** Sentences that emphasize a claim without adding to it,
attach a metaphorical label to it, dramatize it, contrast it with an alternative nobody
believes, summarize a conclusion already drawn, or redescribe the same relationship one
level up — all of these collapse into the first statement of the claim.

The near relatives already in the catalogue are G5, G6, G10 and G13. Each of those names
one shape. This is the general case, and it fires on a block whose sentences are each
defensible in isolation.

**2. Lower the abstraction.** Prefer ordinary verbs and direct relationships to
nominalizations and system metaphors.

| prefer | over |
| --- | --- |
| "Only the domain lock orders these two." | "Ordering here is domain-lock-gated." |
| "Do not publish the body until every symbol resolves." | "Symbol resolution is a mandatory publication requirement." |
| "The counter says the body is hot." | "The counter provides evidence of hotness." |

**3. Delete the scaffolding.** These carry no fact and have no shorter form:

- staged emphasis — *the key distinction*, *the deeper point*, *the honest answer*, *the
  cleanest way to see this*, *the real question*
- orientation — *in other words*, *put differently*, *in one sentence*, *to be clear*
- aphoristic endings — *that is the boundary*, *that distinction matters*, *and that is
  the constraint*
- a claim restated in fresh vocabulary one sentence later

**4. Decode a compound into its relationship.** *X-gated*, *X-backed*, *X-side*,
*X-level*, *X-first*, *X-safe* and the noun stacks beside them name a relationship
without stating it. Recover the verb: "release requires approval", not "an
approval-gated release path".

## The dictionary is the trap

The word lists above are diagnoses, not substitutions. Half the vocabulary that marks
Claudish elsewhere is this tree's own: a fast **path**, a **cold** block, the **surface**
`runtime.h` publishes, a ctest **gate**, a commit that **landed**, profile **drift**
across a rebuild. Each of those is the clearest name for the thing, and rewriting one
costs a reader the term the code uses (G12).

Rewrite a metaphor only where it stands in for a relationship the sentence never states.
Where the sentence states the relationship, the metaphor is the domain's word and stays.

**Contrast is usually a fact here.** A sample of *rather than* across `mono/llvm/`
comments is nearly all factual: it names the alternative a reader would otherwise assume
— "a fatal error rather than a stub", "a rule rather than a tuning choice". The test is
whether a reader would believe the rejected half. If they would, the contrast is what
stops them, and it stays. If the rejected half exists only to set up the preferred one,
cut it and keep the statement.

And a shape that fires across many files is a sweep, not a review comment. SKILL.md's tic
rule governs: narrow the pattern until it selects the defect, count that, and report the
rest.

## A compression must not strengthen the claim

This is where a rewrite turns into the most expensive finding in the document. Check each
of these against the block you started from:

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
own edit, and nothing downstream catches it.

## What this pass does not reach

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
