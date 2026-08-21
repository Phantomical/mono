/**
 * \file
 * \brief The `.mono_lsda` kind values that are not ECMA clause flags, and the
 * stackmap IDs finally markers are planted under.
 *
 * These constants have no dependencies, so both sides of the section can
 * include them. compiler.cpp and passes/ write the section and must not include
 * a mono header. mono_lsda.cpp reads it and needs the mono clause types.
 *
 * Every kind value here sits outside the ECMA MonoExceptionEnum range (NONE=0,
 * FILTER=1, FINALLY=2, FAULT=4), so a marker can never be read as a clause
 * kind.
 */

#ifndef MONO_LLVM_MONO_LSDA_FORMAT_HPP
#define MONO_LLVM_MONO_LSDA_FORMAT_HPP

#include <cstdint>

namespace mono {

/*
 * The entry's landing pad is a cleanup's resume pad rather than an ordinary
 * handler pad.
 *
 * A finally or fault that some other clause protects ends in an invoke of the
 * resume trampoline. That invoke's unwind edge lands on a pad of its own
 * (emit_resume_exit ()), reached only after the cleanup has run. That is where
 * the runtime continues when it goes on to dispatch an enclosing clause for the
 * same throw.
 *
 * So build_ex_info () records the pad against clause_index and uses it as the
 * handler_start of the enclosing entries. It publishes no entry for the marker
 * itself.
 */
constexpr std::uint32_t MONO_LSDA_KIND_RESUME_PAD = 0x10000;

/*
 * The entry is not a protected region. It is one PC range that clause_index's
 * finally handler body occupies, and a clause can have several, one per
 * surviving copy of its body. try_start_off and try_len carry the range.
 * handler_off is unused and zero.
 *
 * Nothing writes this kind. The thread-abort guard is built from the separate
 * `.mono_guards` section instead, which MonoFinallyRangePass
 * (passes/finally-range.cpp) feeds. The value stays reserved, and mono_lsda.cpp
 * still skips an entry carrying it.
 */
constexpr std::uint32_t MONO_LSDA_KIND_FINALLY_BODY = 0x10001;

/*
 * The IDs `llvm.experimental.stackmap` markers are planted under to bracket a
 * finally handler body. The low 32 bits of each are the IL clause index.
 *
 * A stackmap is the one form of marker that survives what codegen does to a
 * finally body. Blocks do not: the optimizer merges a body into whatever it
 * flows through, and BranchFolding merges what is left into a foreign
 * predecessor. By the time the ranges must be recorded, neither an IR block nor
 * a MachineBasicBlock still means "body". An instruction does survive, because
 * passes move and clone instructions rather than rewriting them.
 *
 * The front end plants the pair (method-to-llvm/exceptions.cpp), and
 * MonoFinallyRangePass (passes/finally-range.cpp) walks between them.
 */
constexpr std::uint64_t MONO_LLVM_FINALLY_STACKMAP_ID_BASE = 0xF19A11ULL << 32;
constexpr std::uint64_t MONO_LLVM_FINALLY_END_STACKMAP_ID_BASE = 0xF19A12ULL << 32;
constexpr std::uint64_t MONO_LLVM_FINALLY_STACKMAP_ID_MASK = 0xFFFFFFFFULL;

} // namespace mono

#endif /* MONO_LLVM_MONO_LSDA_FORMAT_HPP */
