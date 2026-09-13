# Register

The full checklist for Pass 4, and the carve-out each rule needs to fire correctly. Read
this before running a register pass, and check `scripts/register-check.sh`'s hits
against it — the script finds candidates, this decides which are real.

- **Modals.** `can`, `will` and `must` in an indicative sentence. `should` and `might`
  are hedges — say what happens, or say in the report that you could not establish it.
  **`would` and `could` are correct in a counterfactual and stay there**: *Without this,
  the dispatcher would be renamed to the method it dispatches for* cannot be said any
  other way, and rewriting it indicative asserts that the renaming happens. The tell is a
  governing clause — *without this*, *left alone*, *rather than*, *otherwise*, *if it
  were*. Measured over one sweep, nine of ten `would`s sat in one of those and every
  "fix" to them was a revert. `may` is the same: real possibility about runtime state
  (*the vtable might not be initialized yet*) is a fact, not a hedge.
- **A FIXME or a TODO is not documentation.** It is a note to whoever picks the work up,
  in that author's own words. Leave its register alone. The register rules govern what
  the code's documentation claims.
- **Summary shape.** A **function** summary starts with a verb, indicative, no
  parenthetical, no "The one X". The one carve-out is a predicate: `Whether mbb leaves
  inside the clause` is the house form for a function returning `bool`, and is not a
  defect. A **type** summary is a noun phrase naming the kind of thing — not a relative
  clause describing what passes through it, and not the type's invariant. A **file** doc
  is `\file` then `\brief` then one sentence of what the file is for. It never opens with
  the file's own name: the reader has the path.
- **Parameters** go in `\param`, lower case in prose. Never UPPERCASE.
- **`.cs` files carry no Doxygen syntax.** No `\param`, `\p`, `\brief`, `\file` — nothing
  under `mono/tests/*.cs` runs through Doxygen, so a backslash command there is dead
  syntax copied from the C++ side, not a convention this language shares. Write plain
  prose instead, with an identifier in backticks: `` `run (7)` ``, not `\p run (7)`. The
  marker itself still follows the rule below — `///` above a declaration — since that is
  C#'s own XML-doc comment syntax, not Doxygen's.
- **Sentence shape.** No semicolons joining clauses. Sentences under 25 words. No `-ing`
  as a **verb** (a gerund subject — "Calling this while holding the lock" — is a noun
  phrase and is fine).
- **Name the actor.** No "nothing here" or "nothing else". No "load-bearing" — say what
  breaks.
- **Marker.** Ask **doc or remark** before you ask anything about length. A doc comment
  sits above a declaration and is `///`, or `/** */` when it runs to paragraphs — never
  `/* */`, however long it is. That one is unambiguous and always worth fixing. A remark
  inside a body is `//`, and `/* */` only once it runs to several paragraphs. What the
  file around you already does decides nothing: fix the marker in the blocks you are
  rewriting anyway, and report the rest as a sweep with the count.
- **Filler out:** simply, just, note that, essentially, basically, obviously.
- **Do not flag plain passives.** The diagnosis is almost always circumlocution instead,
  and the preferred rewrites in the record use passives freely. **This does not cover a
  nominalized subject that merely reads passive.** "An empty finally has no
  return-value effect" is grammatically active — there is no "is …ed" anywhere in it —
  and still fails SKILL.md Pass 3's abstraction check, because "effect" stands in for a
  verb and no actor appears in the sentence at all. Passive voice is exempt here; a noun
  standing in for what should be a verb is not, and is Pass 3's finding, not this one's.

## Two ways a register fix changes the claim

Removing `would` drops a conditional, so an indicative rewrite states as fact something
the code does on one path only. And swapping the subject to name the actor can empty the
sentence: *A marker is the whole transfer function* says which fact drives the dataflow,
while *transfer () is each block's transfer function* restates the name of the function
on the next line. If your rewrite is true of any code with that name, you deleted the
content.
