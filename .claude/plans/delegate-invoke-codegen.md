# Delegate invocation: emitting the invoke glue beside the target

The proposal on the table was one line: *emit the delegate invoke
implementation alongside the method it dispatches to, in fastcc, so it costs no
separate compile and drops a mini codegen dependency.*

Tracing it end to end turned up something else first, and it changes the
answer. **Every delegate call in this build re-enters
`mono_delegate_trampoline ()` — the whole C trampoline, metadata lookups and
all — because the call-site half of delegate dispatch was deleted with the
classic front end and never rebuilt in the translator.** Measured, a delegate
call costs about 14x a direct call here and about 1.0x on stock mono 6.8.

So the recommendation is:

- **Do the call-site fix.** It is ~40 lines in `method-to-llvm/call.cpp`, it is
  the change that recovers the ~400 ns, and it is independent of everything
  else here.
- **Do not do the proposal as written.** Emitting an invoke thunk beside the
  target saves 0–3 compiles per program (counted), taxes every compiled method
  with two thunks it will never use, cannot cover multicast or open-instance
  delegates at all, and needs the delegate trampoline to be entered in fastcc —
  which has a silent-corruption failure mode through
  `mono_arch_get_this_arg_from_call ()`. It is worth revisiting only *after*
  the call-site fix, and then only as a small constant-factor follow-up.
- The "mini codegen dependency" it would drop is real but much smaller than the
  phrase suggests: one 65-line amd64 emitter in `mono/mini/arch-amd64.c`. A
  second, similar-sized one beside it is already dead and can be deleted today
  without any of this.

## How the claims here were established

All in `.claude/worktrees/fix-pool-3` at `450fcc40b4e`, no runtime code
changed. Scratch under `.claude/scratch/delegate-invoke/`.

1. **Trace census.** `MONO_LLVM_JIT_TRACE=1` over nine `mono/mini` corpora,
   `mcs.exe` compiling a LINQ source, and that source's own run — counting how
   many `(wrapper delegate-invoke)` methods the backend translates (§4).
2. **Firing count.** gdb breakpoints on `mono_delegate_trampoline` and
   `mini_init_delegate`, counted with `ignore`, over a microbenchmark, an
   `mcs.exe` run and `objects.exe` (§3, §4).
3. **Microbenchmark.** `DelBench.cs` — five delegate shapes plus a
   `NoInlining` direct call as the control — against this runtime and against
   the machine's mono 6.8 (§3).
4. **Archaeology.** `git show 57346fc2f10^:mono/mini/calls.c` — the deleted
   call-site emitter (§3).

**On timings.** The box was carrying a load average between 26 and 53
throughout (four other agents). Every absolute number below is therefore
inflated and only the **ratio of a delegate call to the direct call in the same
process** is worth reading; four runs of each are quoted so the spread is
visible.

## 1. What happens today, end to end

`Delegate::Invoke` carries `METHOD_IMPL_ATTRIBUTE_RUNTIME`, so no compiler
translates it — the runtime supplies a body.

**Delegate construction.** `mono_delegate_ctor ()` (`mono/metadata/object.c:8938`)
forwards to `mini_init_delegate ()` (`mono/mini/mini-runtime.c:3609`), which sets

- `delegate->method` and `delegate->target`;
- `delegate->method_ptr` — the `addr` an `ldftn` supplied, or, when there is
  none, `create_delegate_method_ptr ()` (`mini-runtime.c:3592`): a *jump
  trampoline* for an ordinary method, and `mono_compile_method_checked ()`
  directly for a dynamic method, because a trampoline over one would leak;
- `delegate->invoke_impl = mono_create_delegate_trampoline ()`
  (`mini-runtime.c:3684`) — a specific `MONO_TRAMPOLINE_DELEGATE` trampoline,
  one per `(delegate class, method)` pair, cached in the domain's
  `delegate_trampoline_hash`.

**Compiling `Invoke`.** The backend declines it —
`implemented_outside_il ()` (`mono/llvm/method-to-llvm/signature.cpp:293`) is
true for any non-wrapper `RUNTIME` method, so `translate_and_compile ()` hands
it to `mono_jit_compile_method ()` (`mono/llvm/runtime.cpp:888`). mini's
`compile_special ()` recognises the delegate class and returns
**`mono_create_delegate_trampoline ()`** (`mini-runtime.c:2237`). That address
is what gets published for `Invoke`.

**The call site.** csc and mcs both emit `callvirt Del::Invoke`, and delegate
`Invoke` is `public virtual hidebysig newslot` — *not* `final`. So
`overridable` at `call.cpp:1191` is true and the translator emits the ordinary
dispatched shape: null check, load `vtable[slot]`, indirect call. The slot is
filled by the vcall trampoline from `mono_jit_compile_method (Invoke)`, i.e.
with the delegate trampoline.

**Every call therefore lands in `mono_delegate_trampoline ()`**
(`mini-trampolines.c:904`), which each time:

- recovers the delegate with `mono_arch_get_this_arg_from_call ()`;
- resolves the target method, adding remoting / unbox / synchronized wrappers
  as needed (`:931-1038`);
- compiles it and stores the address into `delegate->method_ptr` (`:1044-1060`);
- picks the implementation (`:1064-1083`) — see §2 — and stores it into
  `delegate->invoke_impl` (`:1085`);
- returns it, and the generic trampoline jumps there.

The last two steps are a cache that in this build **nobody reads**.
`delegate->invoke_impl` has exactly one reader left in the JIT configuration
(`tramp-amd64-gsharedvt.c:148`), and `tramp_info->method_ptr` /
`tramp_info->invoke_impl` have none. `MonoDelegate::method_code` is never
written either, so the shortcut at `mini-trampolines.c:1047` never fires.

## 2. The five shapes, and what serves each

The trampoline's choice at `mini-trampolines.c:1064-1083`:

| shape | `method_ptr` | implementation chosen |
|---|---|---|
| **static target** (`target == NULL`, static method) | jump trampoline for the method | `impl_nothis` — arch stub, shifts every argument register down one and `jmp *[rdi + method_ptr]` |
| **closed static** (static method with one extra leading param bound to `target`) | as above | `impl_this` |
| **instance target** | jump trampoline (or compiled address for a dynamic method) | `impl_this` — arch stub, `rdi = delegate->target` then `jmp *[rax + method_ptr]` |
| **virtual method** | as instance; the override is resolved *eagerly* at `icall.c:7477` for `CreateDelegate`, or by `ldvirtftn` | `impl_this`. The lazy form (`method_is_virtual`) is dead — nothing writes that field since `method-to-ir.c` went. The abstract-method fallback (`mini-trampolines.c:1029`) still fires, with `enable_caching = FALSE`. |
| **multicast** | — | **not eligible.** Falls to `mono_marshal_get_delegate_invoke ()`, JIT-compiled |
| **open instance** (`target == NULL`, instance method, `Invoke` has one extra param) | **not set at all** — the `if (method && !callvirt)` guard at `:1045` skips it | **not eligible.** The `callvirt` wrapper variant does the virtual dispatch itself, in IL |

Two more that fall to the wrapper: a signature with a **struct return** or with
**more than four parameters** or a non-register-sized one, because
`mono_arch_get_delegate_invoke_impl ()` (`arch-amd64.c:2076`) returns NULL for
those and the `!code` branch takes over.

The arch stubs (`get_delegate_invoke_impl ()`, `arch-amd64.c:1930`) are keyed by
`(has_target, param_count)` in file-static caches — **at most six of them exist
in a process, ever**, and each is three instructions.

The wrapper (`mono_marshal_get_delegate_invoke ()`, `marshal.c:2365`; IL at
`marshal-ilgen.c:4105`) is a single shape that branches on
`this->delegates != NULL` at run time, so multicast is a loop inside it rather
than a separate wrapper. It reaches the target through
`CEE_MONO_LD_DELEGATE_METHOD_PTR` + `CEE_MONO_CALLI_EXTRA_ARG`, which the
backend lowers at `method-to-llvm/wrappers.cpp:445` and
`method-to-llvm/function-pointers.cpp:414` into a plain `calli` marked
`mark_legacy_call` (`function-pointers.cpp:404`). So the wrapper *is* compiled
by this backend, as ordinary IL, and speaks the legacy convention into
`method_ptr` on purpose.

**`method_ptr` and `invoke_impl` are not two views of one thing.**
`invoke_impl` is keyed by the *delegate*'s signature and answers "how do I get
from an `Invoke` call to the bound method"; `method_ptr` is the bound method's
own published legacy entry. Every fast path above is `invoke_impl` doing a
small register shuffle and tail-jumping through `method_ptr`. That is the
seam the proposal wants to collapse.

## 3. The defect: the call-site half is missing

Before the classic compiler was deleted, `mini_emit_method_call_full ()`
special-cased this. From `git show 57346fc2f10^:mono/mini/calls.c:524-548`:

```c
if (!cfg->llvm_only && (m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) && !strcmp (method->name, "Invoke")) {
        MONO_EMIT_NULL_CHECK (cfg, this_reg, FALSE);

        /* Make a call to delegate->invoke_impl */
        call->inst.inst_basereg = this_reg;
        call->inst.inst_offset = MONO_STRUCT_OFFSET (MonoDelegate, invoke_impl);
        MONO_ADD_INS (cfg->cbb, (MonoInst*)call);

        /* We must emit a dummy use here because the delegate trampoline will
        replace the 'this' argument ... */
        EMIT_NEW_DUMMY_USE (cfg, dummy_use, args [0]);
```

`calls.c` is on `mini-codegen-removal.md`'s tier-1 delete list ("`mini_emit_*`
helpers over `MonoInst`"), and it went with the rest. Nothing in
`mono/llvm/` replaced it — a grep of the whole backend for `invoke_impl`
returns nothing.

Without it, the delegate trampoline's per-delegate caching has no consumer and
the trampoline runs on every call. Confirmed directly: with a gdb breakpoint on
`mono_delegate_trampoline`, a run performing ~82,000 delegate calls recorded
**86,006 firings**.

The cost, four runs of 1,000,000 iterations each, `int F(int,int)` shapes,
`NoInlining` on the direct control:

| | this runtime | mono 6.8 |
|---|---|---|
| direct instance call | 28 / 24 / 28 / 26 ms | 3 / 3 / 2 / 3 ms |
| delegate over instance method | 380 / 379 / 414 / 373 ms | 3 / 3 / 3 / 2 ms |
| delegate over static method | 404 / 358 / 373 / 398 ms | 3 / 3 / 3 / 3 ms |
| delegate over virtual method | 422 / 373 / 415 / 417 ms | 3 / 4 / 4 / 3 ms |
| open instance delegate | 652 / 571 / 709 / 577 ms | 8 / 9 / 9 / 8 ms |
| multicast (100k iterations) | 129 / 131 / 196 / 152 ms | 4 / 1 / 1 / 1 ms |

As the load-independent ratio, **delegate / direct in the same process**: this
runtime **13.5–15.9**, mono 6.8 **1.0**. Multicast is worse still — roughly
1300–1960 ns per call here.

### The fix

In `emit_call ()`'s virtual arm (`call.cpp:1178-1234`), ahead of the
`overridable` test: when the callee's declaring class descends from
`MulticastDelegate` and is named `Invoke`, emit the null check that is already
there, load `MONO_STRUCT_OFFSET (MonoDelegate, invoke_impl)` off the receiver,
and call through it with `mark_legacy_entry_call` — exactly the treatment a
vtable slot already gets, since `invoke_impl` holds legacy entries from the
same sources slots do.

Two details the deleted code carried that must come with it:

- **The receiver must stay live across the call.** The arch stub overwrites the
  delegate register with `delegate->target`, so the activation stops rooting
  the delegate. If that delegate was the last root for a collectible dynamic
  method, the code being executed can be freed underneath. mini's answer was a
  dummy use after the call. The IR equivalent is to keep the delegate as an
  operand of the call — it already is one for the `impl_this` and wrapper
  shapes, but *not* for `impl_nothis`, where the arch stub shifts it out —
  or, failing that, an empty `asm sideeffect` taking it, placed after the
  call. **This needs settling before the change lands**; see §7.
- **The null check comes first**, because a `callvirt` on a null delegate must
  throw before anything dereferences it, and `invoke_impl` is at a nonzero
  offset.

Once the site reads `invoke_impl`, the trampoline fires **once per delegate
object** and the steady-state chain is `caller → invoke_impl (arch stub) →
method_ptr (jump trampoline / legacy stub) → legacy entry thunk → body stub →
body`.

## 4. What emitting alongside the target would save

Counted, not estimated. `(wrapper delegate-invoke)` translations, and delegates
constructed, per program:

| program | methods compiled | invoke wrappers compiled | delegates constructed |
|---|---|---|---|
| `basic` | 150 | 0 | — |
| `objects` | 371 | 1 (struct return) | 10 |
| `generics` | 1026 | 1 | — |
| `exceptions` | 843 | 0 | — |
| `iltests` | 325 | 0 | — |
| `arrays` | 114 | 0 | — |
| `basic-calls` | 74 | 0 | — |
| `devirtualization` | 36 | 0 | — |
| `gshared` | 1442 | 3 (+4 begin/end-invoke) | — |
| `mcs.exe` compiling a LINQ source | 5730 | **0** | 28 |
| that source's own run (`Expression.Compile`, LINQ) | 1207 | **0** | — |

Plus, per *process*, at most six three-instruction arch stubs and one specific
trampoline per `(delegate class, method)` pair.

So the compile-count saving is **zero to three method compiles per program**,
and for the two most realistic workloads measured it is exactly zero. The
proposal's premise — that invoke glue is a recurring compile cost worth
eliminating — does not hold. `Expression.Compile` in particular produces
delegates over dynamic methods whose signatures are all register-sized, so they
take the arch stub and compile no wrapper at all.

That reclassifies this from a performance change to a code-removal change, and
the case has to be made on §5 alone.

**The tax on the other side is not zero.** "Alongside the target" only "costs
no separate compile" if the thunk is emitted for *every* method, since nothing
at compile time says whether a method will ever be a delegate target. mcs
compiles 5730 methods and constructs 28 delegates; `objects.exe` compiles 371
and constructs 10. Two extra thunks per method — the closed and the open form —
means roughly 11,000 extra functions in the mcs run to serve at most 28, each
one an extra `Function` through the pipeline, an extra published stub, and an
extra forwarder `MonoJitInfo` (`runtime.cpp:1091-1114`). That is a **0.5%
hit-rate**.

## 5. What mini dependency it actually drops

Named precisely, all in `mono/mini/arch-amd64.c` (there is no `mini-amd64.c`
any more — the phrase in the original note predates that):

| function | lines | status |
|---|---|---|
| `get_delegate_invoke_impl ()` (`:1930`) | ~65 | **live.** Raw `amd64_*` macro emission: the `impl_this` / `impl_nothis` stubs |
| `mono_arch_get_delegate_invoke_impl ()` (`:2076`) | ~58 | **live.** Eligibility test + the static caches. Sole caller `mini-trampolines.c:1368-1369` |
| `get_delegate_virtual_invoke_impl ()` (`:2001`) | ~37 | **dead.** Reachable only from `mono_create_delegate_virtual_trampoline ()` (`mini-trampolines.c:1405`) ← the `MONO_RGCTX_INFO_DELEGATE_TRAMP_INFO` case (`mini-generic-sharing.c:2544`), whose only producer was `method-to-ir.c` |
| `mono_arch_get_delegate_virtual_invoke_impl ()` (`:2136`) | ~10 | **dead**, same chain |
| `mono_arch_get_delegate_invoke_impls ()` (`:2046`) | ~28 | **dead.** The bulk AOT enumerator; no callers at all |

So: **one live 65-line emitter and its 58-line eligibility wrapper.** The other
~75 lines are already unreachable and are a free deletion today, independent of
this document — they belong in `mini-codegen-removal.md`'s stage 3.

What it does **not** drop:

- `mono_delegate_trampoline ()` itself. It is C, not codegen, and §2 shows two
  shapes an alongside-the-target thunk can never serve, so something still has
  to make the choice.
- The generic `MONO_TRAMPOLINE_DELEGATE` trampoline, which is
  `mono_arch_create_generic_trampoline ()` — shared with every other trampoline
  type and not going anywhere.
- `mono_marshal_get_delegate_invoke ()` and its IL emitter, which multicast and
  open-instance delegates still need.

## 6. The shape, if it is done

**Only after §3.** On top of the call-site fix, and as a constant-factor
follow-up, not before.

The mechanism already exists and is the right precedent:
`create_legacy_entry_thunk ()` (`arch/arch.hpp:125-141`, called from
`runtime.cpp:1007` and `:1021`) already emits an extra function into the body's
module, in the legacy convention, that adapts arguments and calls *the body's
stub* rather than the definition beside it — so a later recompile redirects the
stub and the thunk follows without being rebuilt. `wants_unbox_entry ()`
(`runtime.cpp:343`) is the precedent for gating such a thunk on a property of
the method.

A delegate entry is the same thunk with one more twist in it:

```llvm
; the closed form: receiver is delegate->target
define <legacy cc> @"M...$delegate" (ptr %d, <args>) {
  %target = load ptr, ptr getelementptr(i8, ptr %d, i32 offsetof(MonoDelegate, target))
  %r = musttail call fastcc @"M...$fast" (ptr %target, <args>)   ; through M's body stub
  ret %r
}

; the open form: receiver is absent, arguments shift down one
define <legacy cc> @"M...$delegate_static" (ptr %d, <args>) {
  %r = musttail call fastcc @"M...$fast" (<args>)
  ret %r
}
```

Both are pure functions of `M`'s own signature — which is what makes them
generalize past the arch stub's `param_count <= 4`, register-sized-only,
no-struct-return restrictions. A backend entry point beside
`mono_llvm_jit_method_unbox_entry ()` returns the address; `mono_delegate_trampoline`
asks for it in place of `impl_this` / `impl_nothis` at
`mini-trampolines.c:1069-1071`, after it has settled on the final method
(remoting wrapper, unbox wrapper, synchronized wrapper, resolved override
included).

Emitting them eagerly beside every method is what makes the "no separate
compile" claim true, and §4 says that is a 0.5% hit rate. The alternative —
compiling the thunk on demand when the trampoline first asks — is a separate
compile, which is what the proposal set out to avoid, but is *one small module
per (method, form)* rather than a wrapper translation, and only for methods
that are actually delegate targets. **If this is done at all, do it on
demand.** The eager form buys a few hundred nanoseconds on first invocation and
pays for it in every process that never uses it.

### Alternatives rejected

**(a) Keep the arch stubs, just fix the call site.** This is the
recommendation. It is ~40 lines, it recovers the whole measured gap, and it
changes no convention. Its only cost is that the 65-line amd64 emitter stays.

**(b) Compile the general wrapper eagerly for every delegate class**, the way
`mono_llvm_only` already does at `mini-runtime.c:2226-2231`, and delete the
delegate trampoline, `MonoDelegateTrampInfo`, the domain's
`delegate_trampoline_hash` and the arch stubs together. This is by far the
biggest deletion on offer. Rejected because the wrapper's variant is chosen
from the *delegate instance* (`marshal.c:2365-2389`) — the plain wrapper built
with `del == NULL` handles multicast, instance and static targets but not the
`callvirt` or bound-static shapes. llvmonly covers the gap with
`mini_llvmonly_load_method_delegate ()` and a `MonoFtnDesc`-based `method_ptr`,
i.e. the whole of `llvmonly-runtime.c`, which is on the deletion list. Reviving
that is a much larger project than this one and should be its own document.

**(c) Emit the thunk in fastcc and have the call site call `invoke_impl` in
fastcc.** This is the literal reading of the proposal, and it is the dangerous
one. See §7.

## 7. What can go silently wrong

**The fastcc hidden return pointer, entered through `invoke_impl`.** If the
call site calls `invoke_impl` in fastcc, then the *initial* value of
`invoke_impl` — the delegate trampoline — is entered with fastcc arguments.
`mono_delegate_trampoline ()` recovers the delegate with
`mono_arch_get_this_arg_from_call ()` (`arch-amd64.c:1922`), which reads
`AMD64_ARG_REG1`. Fastcc puts the hidden return pointer at argument **0**
(`hidden-return.hpp`), legacy puts it at argument **1** precisely so the
trampolines can do this. For any delegate whose signature returns a large value
type, `%rdi` would be the sret buffer and the trampoline would treat it as a
`MonoDelegate*`, walk `->target` and `->method`, and dispatch on garbage. The
`g_assert (mono_class_has_parent (…, multicastdelegate_class))` at
`mini-trampolines.c:930` would catch most of it — but the assert is on a
`MonoObject`'s vtable read out of unrelated stack memory, so "most" is not
"all", and what gets through is a call to the wrong method with the right
arguments.

This is the same blocker `fastcc-vtable-slots.md` §6.1 names, and it has the
same fix (place the fastcc hidden return pointer at index 1 when the signature
has a `this`) and the same risk profile. **Nothing in this document should be
done in fastcc before that lands.** The legacy-convention thunk in §6 has no
such problem: it is entered exactly the way the arch stub is today.

**Delegate variance.** `Delegate.CreateDelegate` accepts relaxed signature
matches — `Func<object>` over a method returning `string`, `Action<Derived>`
over one taking `Base` (`mcs/class/corlib/System/Delegate.cs`,
`arg_type_match`). A thunk emitted beside the *target* is built from the
target's signature, while the caller built its site from the *delegate's*. On
amd64 both lower reference types to `ptr`, so the two agree — but that is an
ABI coincidence, not a guarantee, and it is exactly the shape of mistake that
returns a plausible wrong answer rather than crashing. The existing arch stub
makes the same assumption, so this is not a regression; it is a reason not to
widen the assumption's blast radius without a test that exercises variant
delegates over value-type and float signatures.

**The GC liveness hole (§3).** The arch stub overwrites the delegate register
with `delegate->target` and the wrapper's `calli` does not keep the delegate
live either. mini emitted a dummy use for exactly this
(`calls.c:537-546`, upstream bug #667921) and the symptom is a dynamic method's
collectible assembly freed while its code is on the stack — a crash at a random
address, minutes later, with nothing pointing at delegates. Whatever the
call-site fix does here must be deliberate. The cheapest correct answer is to
keep the delegate as an operand of the call in the IR, which it already is
except in the `impl_nothis` shape.

**Choosing the thunk before the method is final.** `mono_delegate_trampoline`
substitutes the method several times before it settles: remoting invoke
(`:950`), unbox wrapper (`:978`), resolved override (`:1027`, `:1033`),
synchronized wrapper (`:1038`). A thunk fetched for the *original* method
dispatches to the wrong body — a wrong call target, not a crash. Fetch it last,
from the same `method` variable the existing code compiles.

**Multicast after `Delegate.Combine`.** `Combine` allocates a *new* delegate,
so its `invoke_impl` starts as the trampoline again and correctly resolves to
the wrapper. But `MulticastDelegate::delegates` can be observed non-null on a
delegate whose `invoke_impl` was already resolved to a single-target stub if
anything ever mutated the array in place. Nothing does today. Worth an
assertion rather than a comment.

## 8. Order

**Stage 1 — restore the `invoke_impl` call site.** `call.cpp`, ~40 lines, with
the receiver-liveness question settled first. This is the whole measured win
and it is independent of everything below. Gate: `-L regression`,
`-R 'runtime/delegate'`, `-L gshared`, and re-run the §3 benchmark — the
delegate/direct ratio should fall from ~14 to near 1.

**Stage 2 — delete the dead virtual-invoke emitters.** `get_delegate_virtual_invoke_impl ()`,
`mono_arch_get_delegate_virtual_invoke_impl ()`,
`mono_arch_get_delegate_invoke_impls ()`, `mono_get_delegate_virtual_invoke_impl{,_name} ()`,
`mono_create_delegate_virtual_trampoline ()`. ~75 lines of already-unreachable
amd64 emission. Belongs in `mini-codegen-removal.md` stage 3, not here.

**Stage 3 — reconsider.** With stage 1 in, measure again and see whether the
remaining chain (`invoke_impl → method_ptr → legacy entry → body`) is worth
collapsing. If it is, §6's on-demand legacy-convention thunk is the shape, and
it is gated behind `fastcc-vtable-slots.md` stage 1 if it is ever to be fastcc.

## 9. What could not be determined

- **Whether stage 1 alone reaches mono 6.8's numbers.** Stock mono was 1.0x on
  every shape; this build's *direct* call is also slower than stock's under
  load, so some of the gap is unrelated to delegates. Only measuring after
  stage 1 answers it.
- **The real-workload frequency of delegate calls.** mcs compiling a small file
  made 271 of them; `objects.exe` made 7. Those are not delegate-heavy
  programs, and no Unity-shaped workload was measured. A game loop with
  per-frame delegate dispatch is where 400 ns/call actually bites, and nothing
  here quantifies that.
- **Whether the open-instance shape can ever be served beside a target.** The
  target is only the *declared* method; which override runs depends on the
  first argument's runtime type. A thunk could do the vtable load itself, but
  for interface methods it would need the IMT key, and getting the slot wrong
  is silent. Not investigated further because §4 removed the motive.
- **Code size of the eager per-method thunks.** Not measured. The 0.5% hit rate
  was enough to reject the eager form without it.

## 10. Nothing was committed

The only deliverable is this document. `DelBench.cs`, `DelBench2.cs`, the
LINQ source, the trace files and the run logs are under
`.claude/scratch/delegate-invoke/`. The worktree is otherwise clean at
`450fcc40b4e`.
