/**
 * \file
 * pass-dump.cpp - opt-in per-pass LLVM IR dump tool for the tier-1 pipeline.
 *
 * See pass-dump.hpp for the env var and file-naming contract. The one tricky
 * part is recovering the whole module from whatever IR unit a given pass ran
 * over: function/CGSCC/loop passes only hand the after-pass callback that
 * narrower unit, not the module it lives in. unwrap_module () below walks
 * back up to the parent module the same way LLVM's own IR-printing
 * instrumentation does (unwrapIR/unwrapModule in
 * llvm/lib/Passes/StandardInstrumentations.cpp).
 */

#include "pass-dump.hpp"

#include <cctype>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <llvm/ADT/Any.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LazyCallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mono {

namespace {

/* Any holds one of a handful of concrete IR-unit pointer types depending on
 * which pass kind just ran; this pulls out the one we ask for, or nullptr if
 * IR isn't holding that type. */
template <typename IRUnit>
const IRUnit *
unwrap_ir (const Any &ir)
{
	const IRUnit *const *ptr = llvm::any_cast<const IRUnit *> (&ir);
	return ptr ? *ptr : nullptr;
}

/* Module and function passes are the common case; CGSCC and loop passes need
 * a hop up to their enclosing function first. */
const Module *
unwrap_module (const Any &ir)
{
	if (const Module *m = unwrap_ir<Module> (ir))
		return m;
	if (const Function *f = unwrap_ir<Function> (ir))
		return f->getParent ();
	if (const LazyCallGraph::SCC *scc = unwrap_ir<LazyCallGraph::SCC> (ir))
		return scc->begin ()->getFunction ().getParent ();
	if (const Loop *l = unwrap_ir<Loop> (ir))
		return l->getHeader ()->getParent ()->getParent ();
	return nullptr;
}

/* Module names come from method names (translator.cpp's "jit-module-%s"),
 * which can contain generic/operator punctuation that isn't safe to use
 * verbatim as a directory name. Keep only what's safe and fold the rest to
 * '_'. */
std::string
sanitize_for_path (StringRef name)
{
	std::string out;
	out.reserve (name.size ());
	for (char c : name)
		out.push_back ((isalnum ((unsigned char) c) || c == '_' || c == '-' || c == '.')
		                    ? c
		                    : '_');
	return out.empty () ? "module" : out;
}

struct ModuleDumpState {
	std::string dir;
	unsigned next_index = 0;
};

/*
 * Owns the per-module dump state (which directory it writes to, how many
 * passes have been dumped so far) across the lifetime of one
 * PassInstrumentationCallbacks. register_pass_ir_dumper () heap-allocates one
 * of these and hands the after-pass callback a shared_ptr to it, since the
 * callback fires from inside mpm.run () - well after
 * register_pass_ir_dumper () itself has returned.
 *
 * The mutex isn't load-bearing today (the tiered worker is a single dedicated
 * thread, so only one optimize () - and thus one PassIrDumper - is ever live
 * at a time), but it's free insurance against that assumption changing later.
 */
class PassIrDumper {
public:
	explicit PassIrDumper (std::string base_dir) : base_dir_ (std::move (base_dir)) {}

	void after_pass (StringRef pass_id, const Any &ir);

private:
	std::string base_dir_;
	std::mutex mutex_;
	std::unordered_map<const Module *, ModuleDumpState> modules_;
};

void
PassIrDumper::after_pass (StringRef pass_id, const Any &ir)
{
	const Module *module = unwrap_module (ir);
	if (!module)
		return;

	std::lock_guard<std::mutex> guard (mutex_);

	auto it = modules_.find (module);
	if (it == modules_.end ()) {
		std::string dir = base_dir_ + "/" + sanitize_for_path (module->getName ());
		llvm::sys::fs::create_directories (dir);
		it = modules_.emplace (module, ModuleDumpState { std::move (dir), 0 }).first;
	}

	ModuleDumpState &state = it->second;
	std::string path = state.dir + "/pass-" + std::to_string (state.next_index) + ".il";

	std::error_code ec;
	raw_fd_ostream out (path, ec);
	if (ec) {
		/* Best-effort: a write failure in this debugging tool shouldn't take
		 * down the compile it's observing. */
		return;
	}

	out << "; after pass " << state.next_index << ": " << pass_id << "\n";
	module->print (out, nullptr);

	state.next_index++;
}

} // namespace

void
register_pass_ir_dumper (PassInstrumentationCallbacks &pic)
{
	/* Read once and cache: optimize () calls this on every tier-1 compile, and
	 * the whole point of gating on the env var is that an unset var costs
	 * nothing beyond this one lookup. */
	static const std::string dump_dir = [] () -> std::string {
		const char *env = std::getenv ("MONO_LLVM_DUMP_PASS_IR");
		return env ? env : "";
	} ();

	if (dump_dir.empty ())
		return;

	auto dumper = std::make_shared<PassIrDumper> (dump_dir);
	pic.registerAfterPassCallback (
	    [dumper] (StringRef pass_id, Any ir, const PreservedAnalyses &) {
		    dumper->after_pass (pass_id, ir);
	    });
}

} // namespace mono
