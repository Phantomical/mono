/**
 * \file
 * \brief The dump points MONO_JIT_DUMP turns on, and where their output goes.
 *
 * Both engines print through here, so one variable selects what to see and one
 * filter selects which methods to see it for. A point is off unless
 * MONO_JIT_DUMP names it, and reading the variable happens once.
 */

#ifndef MONO_MINI_JIT_DUMP_HPP
#define MONO_MINI_JIT_DUMP_HPP

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodHeader MonoMethodHeader;

namespace mono {

/**
 * A stage of a compile that can print what it holds.
 *
 * MONO_JIT_DUMP names the ones it wants, separated by `;` or `,`, and `all`
 * names every one. The name a point carries here is the name the variable
 * takes and the name of the directory its files land in.
 */
enum class DumpPoint : uint32_t {
	il = 1u << 0,        ///< `il`: the method's CIL.
	mint = 1u << 1,      ///< `mint`: the bytecode the interpreter runs.
	unopt_ir = 1u << 2,  ///< `unopt-ir`: the IR the translator wrote.
	tier1_ir = 1u << 3,  ///< `tier1-ir`: that IR after the tier-1 pipeline.
	tier2_ir = 1u << 4,  ///< `tier2-ir`: after the tier-2 pipeline.
	tier1_asm = 1u << 5, ///< `tier1-asm`: the code tier 1 emits.
	tier2_asm = 1u << 6, ///< `tier2-asm`: the code tier 2 emits.
};

/// Whether MONO_JIT_DUMP asked for this point, whatever the method is.
///
/// Answers before there is a name to test, which is what a caller needs when
/// producing the name itself costs something.
bool dump_point_enabled (DumpPoint point);

/// Whether MONO_JIT_DUMP asked for any point at all.
///
/// Building the name a dump is filed under costs a full name, so a compile that
/// prints nothing asks this before paying for one.
bool any_dump_point_enabled ();

/**
 * Whether this point is on and MONO_JIT_DUMP_FILTER names this method.
 *
 * The filter is matched as a substring, and every point matches it against the
 * same string: `Class:Method (argtypes)@0xADDR`, which is what
 * `dump_name ()` builds. An unset filter takes every method.
 */
bool dumping (DumpPoint point, const char *name);

/// The string every dump point names a method by, and matches the filter
/// against. The caller owns the result.
std::string dump_name (MonoMethod *method);

/**
 * Where one dump goes: stdout, or a file under MONO_JIT_DUMP_DIR.
 *
 * With no directory set this hands back stdout. With one set it creates
 * `<dir>/<point>/` and opens `<method>.<extension>` inside it. A name already
 * taken gets a counted suffix, so two threads dumping at once, and a method
 * compiled more than once, each get a file of their own.
 *
 * `stream ()` is null when the file did not open. The reason goes to stderr and
 * the caller prints nothing.
 *
 * **Hold one for the whole of a dump.** Promotions compile on several worker
 * threads, so two methods reach a point at once. Going to stdout, this holds a
 * lock for as long as it lives, which is what keeps one method's dump in one
 * piece. A caller that opens and closes one for each line gets a dump the other
 * thread interleaves with. Going to a directory, each dump has a file to itself
 * and takes no lock.
 */
class DumpDestination {
public:
	DumpDestination (DumpPoint point, const char *name);
	~DumpDestination ();

	DumpDestination (const DumpDestination &) = delete;
	DumpDestination &operator= (const DumpDestination &) = delete;

	FILE *stream () const { return stream_; }

	/// Whether the dump goes to a file of its own rather than to stdout.
	/// A caller that prints a heading to separate its dump from the next one
	/// can leave it out when this is true.
	bool is_file () const { return owned_; }

private:
	FILE *stream_ = nullptr;
	bool owned_ = false;
	std::unique_lock<std::mutex> shared_stream_;
};

/**
 * Prints a method's CIL to \p out, inside the class and the signature it is
 * declared with.
 *
 * The shape is ilasm's, so that a reader can tell an argument from a local
 * without going to the metadata. It is a dump rather than a source file: the
 * body is the disassembly mono prints, which names tokens and does not resolve
 * them.
 */
void dump_il (FILE *out, MonoMethod *method, MonoMethodHeader *header);

} // namespace mono

#endif
