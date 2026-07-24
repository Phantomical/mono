/**
 * \file
 * pass-dump.hpp - opt-in per-pass LLVM IR dump tool for the tier-1 pipeline.
 *
 * Wires an after-pass callback onto a PassInstrumentationCallbacks so every
 * pass in optimize()'s -O2 pipeline writes the module IR it just produced to
 * disk. This is purely a debugging aid for answering "what did pass X
 * change" - it has no effect unless explicitly turned on.
 */

#ifndef MONO_MINI_LLVM_PASS_DUMP_HPP
#define MONO_MINI_LLVM_PASS_DUMP_HPP

namespace llvm {
class PassInstrumentationCallbacks;
}

namespace mono {

/*
 * If the MONO_LLVM_DUMP_PASS_IR environment variable is set to a directory,
 * registers an after-pass callback on PIC that writes the module IR after
 * each pass to:
 *
 *   <dir>/<sanitized-module-name>/pass-N.il
 *
 * with N counting up from 0 per module (the engine JITs one module per
 * method, so this keeps two methods' dumps from clobbering each other) and
 * each file opening with a `; after pass N: <PassID>` IR comment line before
 * the module IR, so the file both documents which pass produced it and stays
 * IR that llvm-as can parse as-is.
 *
 * If the variable is unset or empty, this registers nothing - the dumper
 * costs nothing beyond a single cached getenv() lookup. Safe to call once per
 * optimize() invocation.
 */
void register_pass_ir_dumper (llvm::PassInstrumentationCallbacks &pic);

} // namespace mono

#endif /* MONO_MINI_LLVM_PASS_DUMP_HPP */
