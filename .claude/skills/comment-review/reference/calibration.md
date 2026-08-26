# Calibration

Worked before/after pairs, and the fixtures that decide whether a reviewer is tuned right.

Read this when a finding feels borderline. The pairs are the only demonstrations in the
record that were checked against the bar rather than proposed against it.

---

## The pairs

### Pair 1 — `mono_llvm_jit_stub_for`, `runtime.h`

Before:

```c
/// Returns the address to call a method at, without compiling it.
///
/// This is the same stub mono_llvm_jit_compile_method () returns, so the two
/// agree whichever asked first. The body behind the stub is compiled by the
/// first call that arrives through it.
///
/// A caller that only wants the address gets it at the price of a symbol and a
/// stub. A caller that needs the code to exist, or that needs a refusal to
/// come back as an error it can raise, has to compile instead.
///
/// Returns NULL and sets the error if the method cannot be published.
```

After:

```c
/// Returns the function pointer for this method, without compiling it.
///
/// This allocates a stub if not already created. If you need the method to be
/// compiled immediately use mono_llvm_jit_compile_method (), otherwise the
/// method will be compiled when it is first called.
///
/// Returns NULL and sets the error if the method cannot be published.
```

Eleven lines to seven. G1 on the summary, G2 on the second paragraph, G3 and G6 delete the
third entirely. The NULL contract survives untouched — it is the one paragraph that tells a
caller what to write.

### Pair 2 — `CodeArena`, `jitlink-memory.hpp`

Before:

```cpp
/// The code memory a domain's compiled methods and stubs come out of.
///
/// A reservation is readable, writable and executable, and stays that way.
/// There is no free. A retired method keeps its bytes until the whole arena
/// goes, which is when its domain does.
///
/// The arena holds a code manager of its own rather than the domain memory
/// manager's. mono_mem_manager_code_reserve () takes the domain lock - it is
/// mono_domain_lock (), see memory-manager.c - and code is reserved with linker
/// locks held. A mutator can arrive at those locks while it holds the domain
/// lock already, so taking the two in that order deadlocks.
```

After:

```cpp
/// An arena allocator for executable memory.
///
/// Individual allocations within this arena are not freed until the whole thing
/// is. Takes mono_domain_lock () internally.
```

Eleven lines to four. G9 on the summary, G4 and G5 on the permissions sentence, G7 on the
free rule, G8 on the whole third paragraph.

### Pair 3 — `shift_amount`, `method-to-llvm/bitops.cpp`

Before:

```cpp
/// Returns the shift amount, resized to type's width and masked into its range.
///
/// An LLVM shift needs both operands in one type. Table III.6 pairs an int32 or native
/// int amount with any of the three shiftable value types. One operand always changes
/// width to match the other.
///
/// The mask keeps the result defined. If the shift amount reaches the operand's width,
/// the spec leaves the result unspecified. LLVM treats that case as poison instead, and
/// poison can affect code far from where it happened.
```

After — the doc keeps the caller-facing semantics, and the reason moves down to the line
that would otherwise look arbitrary:

```cpp
/// Returns amount widened or narrowed to match type.
///
/// The shift amount is taken modulo type's width.
```

```cpp
	// ECMA-335 leaves an over-wide shift unspecified. LLVM makes it poison, so
	// the amount is masked instead of passed through.
	return builder.CreateAnd (value, llvm::ConstantInt::get (type, bits - 1));
```

Nine lines to three, plus two at the line. This pair took two rounds: the first attempt
wrote the surviving fact as "The result is masked to less than type's width, so an amount
at or past the width wraps", which is G10 — two clauses describing one operation from both
ends, with the joining term (*modulo*) left for the reader to supply.

### Pair 4 — `passes/restore-tail-position.hpp`, the file doc

Before: 21 lines — the invariant, the antagonist, why SimplifyCFG exempts `musttail`, the
silent symptom, and a paragraph restating the pass body.

After:

```
LLVM can only turn a `tail call` into a jump if it is immediately followed by a
`ret`. The SimplifyCFG pass merges all the `ret`s into one block, which breaks
this. We still want tail calls to happen, so this pass undoes the merge for
function calls.
```

Three sentences: invariant, antagonist, what the pass does.

The interesting part is what the **intermediate draft** got wrong. It made every cut above
and then stopped at the antagonist, so the doc set up a problem and never said the pass
solves it — G11. It also spent three references on *a call*, *the marker* and *a marked
call* rather than saying `tail call` (G12), kept a sentence denying what *only when* had
already excluded (G13), and kept the silent-failure symptom (G14). Four rules, one edit.

### Pair 5 — `mono_lsda.cpp`, the wire-format block

**The only pair where the comment got longer**, and the one that produced the
specification rule.

The cuts were routine: the leading `mono_lsda.cpp:` (the reader has the path); a sentence
restating `build_ex_info`'s doc from the header; every column annotation in the byte table,
all of which live on `MonoLsdaEntry`; and "A marker entry describes something other than a
protected region", which defines a thing as not-the-other-thing.

The two findings were not:

**A claim that was true and incomplete.** "One protected region contributes one entry per
clause in its chain" omits the outer loop: a try with N protected calls yields one landing
pad with N invoke ranges, and the format is one entry per invoke range. Repeated
`clause_index`/`handler_off` rows over disjoint ranges are normal, and the sentence
predicts they are not. **An incomplete claim about a format is a false one, because a
reader decodes against it.**

**A property the block never pinned.** Entry order is significant — `eh-gather.cpp` reverses
`TypeIds` to restore innermost-first, and `.mono_lsda` carries that by position. Anyone
writing a second reader from the block alone would sort or hash the entries and lose the
nesting chain. Nothing in the block was wrong; the fact was simply absent, which is why no
ordinary check fires on it.

For a specification, run the checks backwards: take each field, then each structural
property — count, order, repetition, alignment, endianness, what may appear more than once
— and ask whether the block pins it.

The same block also produced a legitimate keep. This survives:

```c
 * native_code must not be dereferenced. The tests pass a base that points at
 * nothing, so a read faults there and never in a real compile.
```

It clears the bar on a narrow argument: a violation fails immediately — the test segfaults
— but it fails in a way that **misdirects the repair**. The obvious reading of that crash
is "the test passes a bogus pointer". The comment exists to stop the wrong fix, not to
announce a property. **A comment whose only job is to redirect a diagnosis is legitimate;
the test is whether the failure names its own cause.**

---

## Fixtures — flag these

`mono/llvm/runtime/backend.hpp` fails four rules in nine lines:

```cpp
/// Release everything associated with a MonoMethod.
///
/// Calling this while the method is still in use will lead to UB.
static void release_method (MonoMethod *method);

/// Stop all background compilation. Blocks until any in-progress work is
/// completed.
///
/// Note that this will prevent any compilations from running again.
static void stop_compilation ();

/// Where METHOD's body starts in DOMAIN, or null when this engine has not
/// compiled it there.
```

Imperative summaries where the file's neighbours are indicative; "Note that" filler;
UPPERCASE parameter names; `will` where the approved modal is `can`; and "UB" for what is a
use-after-free — the language's term for a construct the standard does not define, borrowed
for a runtime lifetime rule it does not govern.

**The substance under that is fine.** Both are real caller obligations. Do not cut them.

---

## Fixtures — do **not** flag these

These look like findings and are not. A reviewer that reports them is tuned wrong.

**1. A gerund subject is not `-ing` used as a verb.**

> "Calling this while the method is still in use will lead to UB."

An earlier review flagged `Calling` under the `-ing` rule. STE 3.5 bans the `-ing` form as
a **verb**; here it heads a gerund subject, which is a noun phrase and is approved. A
register rule is a claim too, and this one was applied from the shape of the word rather
than from its part of speech in the sentence.

**2. "Note that" is sometimes the verb *note*.**

```cpp
/// Note that a piece of code, and the record registered for it, belong to a
/// method - see TranslationTarget::remember.
using RememberFn = ...
```

Here *note* is the imperative verb meaning *record* — it describes what the callback does,
not padding. It still reads as filler on the first pass, which is the one-word-one-meaning
failure. The fix is "Records that…", **not** deletion.

**3. A good comment is allowed to break register rules.**

`compile-queue.hpp` states its rule before the reasoning and names the tempting change that
breaks it — which is what makes the block necessary rather than merely informative, because
it is aimed at the person about to make that change. It also uses `would` twice and one "nothing here". That
is the right severity ranking: flag those as register nits and do not suggest touching the
substance.

(Separately, that block is also the G15 example — the argument goes because the type
enforces the rule. Both readings are in the record. The point here is the ranking: never
let a register nit drive a substance edit.)

**4. Rationale at the line stays.**

```cpp
/*
 * The thunks are pointed at the interpreter before the method is
 * transformed, and that order matters: transforming runs the class
 * initializer, and this can be running inside a lazy thunk's callback. A
 * cctor that calls back into this very method would re-enter the trampoline
 * being resolved and start its compile again below itself, with nothing to
 * stop it. Publishing first means such a call lands on the entry instead.
 */
```

The argument sits next to the two statements whose order it is about, not in the function's
doc comment. It names the whole chain and ends with what the chosen order buys. This is the
shape G8 rationale is supposed to be moved *into*.

**5. A quote plus what the quote does not cover.**

The ECMA-335 III.2 passages for all six prefixes sit verbatim at the top of
`method-to-llvm/prefixes.cpp`. The comment above `emit_prefix ()` says only what the
standard cannot: that a prefix emits no code, that three of the six are consumed elsewhere
and by whom, and that `no.` records nothing because permission to skip a check is not an
obligation to skip it. Where a quote documents the function, the quote **is** the doc
comment, and our text covers only what this backend does.

**6. `\param` plus a hoisted convention.**

```cpp
/// Builds the buffer a vararg call passes its variable arguments in.
///
/// \param args  the call's arguments in order, the this included.
///
/// The first word holds the signature. Each variable argument follows at the
/// running sum of mono_type_stack_size () over the ones before it. That is what
/// System.ArgIterator reads: Setup starts its walk at the second word, and
/// IntGetNextArg advances by that same size without realigning. Sizes are not
/// uniform - a float takes four bytes, not a whole slot - so an offset that
/// disagrees does not fault. The next argument reads as garbage.
```

Verb-first summary; the parameter in a `\param` entry, out of the prose; the layout stated
once as the shared convention; a closing sentence saying how the bug presents. "Does not
fault. The next argument reads as garbage" is worth more than any amount of "this must
match".
