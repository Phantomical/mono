/**
 * \file
 * mono_lsda_format.hpp - the constants both sides of codegen must agree on: the
 * `kind` column values of the `.mono_lsda` section that are NOT ECMA clause
 * flags, and the stackmap IDs mono plants markers under.
 *
 * Split out of mono_lsda.hpp because the two sides of the section are compiled
 * against different worlds: the WRITER (MonoLSDAStreamer and the machine passes
 * feeding it, passes/) is pure LLVM and must not pull in mini.h, while the
 * READER (mono_lsda.cpp) needs the mono clause types. These constants are the
 * only thing both sides must agree on beyond the byte layout, so they live here
 * with no dependencies at all.
 *
 * Every value here is deliberately OUTSIDE the ECMA MonoExceptionEnum range
 * (NONE=0, FILTER=1, FINALLY=2, FAULT=4), so a marker entry can never be
 * mistaken for a clause kind.
 */

#ifndef __MONO_MINI_LLVM_MONO_LSDA_FORMAT_HPP__
#define __MONO_MINI_LLVM_MONO_LSDA_FORMAT_HPP__

#include <cstdint>

namespace mono {

/*
 * The entry's landing pad is a cleanup's RESUME pad rather than an ordinary
 * handler pad.
 *
 * A finally/fault that some other clause protects ends in an invoke of the resume
 * trampoline whose unwind edge lands on a pad of its own (emit_resume_unwind),
 * reached only after the cleanup has run. That is where the runtime has to
 * continue when it goes on to dispatch an enclosing clause for the same throw, so
 * build_ex_info () picks these entries out by their kind, records the pad against
 * clause_index, and uses it as the handler_start of the enclosing entries it
 * synthesises - it publishes no entry of its own for them.
 */
constexpr std::uint32_t MONO_LSDA_KIND_RESUME_PAD = 0x10000;

/*
 * The entry is not a protected region at all: it is one PC range clause_index's
 * FINALLY handler BODY occupies, recorded by MonoFinallyRangePass
 * (passes/finally-range.cpp). A clause can have several, one per surviving copy
 * of its body.
 *
 * try_start_off/try_len carry the range; handler_off is unused and zero. The
 * runtime's thread-abort guard is the only consumer: it asks whether a stopped
 * frame is inside a finally, which no dispatch entry can answer (a finally whose
 * protected region has no unwinding call has no dispatch entry at all).
 */
constexpr std::uint32_t MONO_LSDA_KIND_FINALLY_BODY = 0x10001;

/*
 * The IDs mono plants `llvm.experimental.stackmap` markers under.
 *
 * A stackmap is the one thing that survives everything codegen does to a finally
 * handler body. Blocks do not: the optimizer merges a body into whatever it flows
 * through, and BranchFolding merges what is left back into a foreign predecessor,
 * so neither an IR block nor a MachineBasicBlock still means "body" by the time
 * the ranges have to be recorded. An instruction does - passes move and clone
 * instructions rather than rewriting them - so the body is bracketed by a pair of
 * markers and MonoFinallyRangePass (passes/finally-range.cpp) walks between
 * them.
 *
 * The low 32 bits of a finally marker's ID are the IL clause index.
 */
constexpr std::uint64_t MONO_LLVM_THIS_SLOT_STACKMAP_ID = 0;
constexpr std::uint64_t MONO_LLVM_FINALLY_STACKMAP_ID_BASE = 0xF19A11ULL << 32;
constexpr std::uint64_t MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE = 0xF19A12ULL << 32;
constexpr std::uint64_t MONO_LLVM_FINALLY_STACKMAP_ID_MASK = 0xFFFFFFFFULL;

} // namespace mono

#endif /* __MONO_MINI_LLVM_MONO_LSDA_FORMAT_HPP__ */
