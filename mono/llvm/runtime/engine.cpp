#include "engine.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorHandling.h>

#include <atomic>

#include <glib.h>

namespace mono {

EngineKind
selected_engine ()
{
	static const EngineKind kind = [] {
		const char *asked = g_getenv ("MONO_LLVM_JIT_ENGINE");

		if (asked != nullptr && llvm::StringRef (asked) == "new")
			return EngineKind::backend;
		return EngineKind::legacy;
	}();

	return kind;
}

void
claim_engine (EngineKind kind)
{
	static std::atomic<int> claimed { -1 };
	int want = (int) kind;
	int seen = -1;

	if (claimed.compare_exchange_strong (seen, want))
		return;
	if (seen == want)
		return;

	llvm::report_fatal_error ("mono: both llvm engines were started in one process",
	                          /*GenCrashDiag=*/false);
}

} // namespace mono
