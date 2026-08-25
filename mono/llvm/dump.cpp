#include "dump.hpp"

#include <cstdio>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <memory>

using namespace llvm;

namespace mono {

namespace {

constexpr const char *dump_name_key = "mono.dump.name";

/// Collects entry and every local body it reaches through a direct call.
///
/// These are the copies the inliners folded in, and the module publishes no
/// symbol for one, so a reader has them here or not at all.
void
gather_folded_bodies (Function &entry, SmallPtrSetImpl<Function *> &keep)
{
	SmallVector<Function *, 8> pending;

	keep.insert (&entry);
	pending.push_back (&entry);

	while (!pending.empty ()) {
		Function *body = pending.pop_back_val ();

		for (Instruction &instruction : instructions (*body)) {
			const auto *call = dyn_cast<CallBase> (&instruction);
			Function *callee = call != nullptr ? call->getCalledFunction ()
			                                  : nullptr;

			if (callee == nullptr || callee->isDeclaration ()
			    || !callee->hasLocalLinkage ())
				continue;

			if (keep.insert (callee).second)
				pending.push_back (callee);
		}
	}
}

/// Sends each alias of a body this dump dropped to that body's symbol, and
/// erases the alias.
///
/// An alias must point to a definition. The tier-1 profiling instrumentation
/// gives each body it counts a private alias, so a module with a dropped body
/// still holds one, and it is what an alias of a declaration is.
void
drop_aliases_of_dropped_bodies (Module &module)
{
	for (GlobalAlias &alias : make_early_inc_range (module.aliases ())) {
		const auto *target = dyn_cast<GlobalValue> (alias.getAliasee ());

		if (target != nullptr && !target->isDeclaration ())
			continue;

		alias.replaceAllUsesWith (alias.getAliasee ());
		alias.eraseFromParent ();
	}
}

/// Erases the declarations and the globals that nothing in the module names
/// any more.
///
/// Dropping a body drops what its calls and its loads named, and one of those
/// can be the last use of another global. So this repeats until a pass erases
/// nothing.
void
drop_unused_globals (Module &module)
{
	bool erased = true;

	while (erased) {
		erased = false;

		for (Function &function : make_early_inc_range (module))
			if (function.isDeclaration () && function.use_empty ()) {
				function.eraseFromParent ();
				erased = true;
			}

		for (GlobalVariable &global : make_early_inc_range (module.globals ()))
			if (global.use_empty ()) {
				global.eraseFromParent ();
				erased = true;
			}
	}
}

} // namespace

Error
dump_body_module (DumpPoint point, const Module &module, StringRef entry,
                  StringRef name)
{
	if (module.getFunction (entry) == nullptr)
		return Error::success ();

	// A batch shares one module between its members, so the dump costs a copy
	// of the whole module for each method that asks for one. The assembly
	// points already pay a codegen each.
	std::unique_ptr<Module> copy = CloneModule (module);
	Function *body = copy->getFunction (entry);
	SmallPtrSet<Function *, 8> keep;

	gather_folded_bodies (*body, keep);

	for (Function &function : *copy) {
		if (keep.contains (&function) || function.isDeclaration ())
			continue;

		function.deleteBody ();

		// A declaration cannot have local linkage. What the other members
		// folded in is a copy under a name of its own, so widening it here
		// clashes with nothing.
		if (function.hasLocalLinkage ())
			function.setLinkage (GlobalValue::ExternalLinkage);
	}

	drop_aliases_of_dropped_bodies (*copy);
	drop_unused_globals (*copy);

	return with_dump_stream (point, name, [&] (raw_pwrite_stream &out) {
		out << "; *** " << name << " ***\n";
		copy->print (out, /*AAW=*/nullptr);

		return Error::success ();
	});
}

Error
with_dump_stream (DumpPoint point, StringRef name,
                  function_ref<Error (raw_pwrite_stream &)> body)
{
	DumpDestination destination (point, name.str ().c_str ());

	if (destination.stream () == nullptr)
		return Error::success ();

	// Anything already buffered on this stream was printed first, so it has to
	// reach the file first. Only this stream writes past here.
	fflush (destination.stream ());

	raw_fd_ostream out (fileno (destination.stream ()), /*shouldClose=*/false);
	Error result = body (out);

	out.flush ();
	return result;
}

void
set_dump_name (Function &function, StringRef name)
{
	LLVMContext &context = function.getContext ();

	function.setMetadata (dump_name_key,
	                      MDNode::get (context, MDString::get (context, name)));
}

std::string
dump_name_of (const Function &function)
{
	if (MDNode *node = function.getMetadata (dump_name_key))
		if (node->getNumOperands () == 1)
			if (auto *name = dyn_cast<MDString> (node->getOperand (0)))
				return name->getString ().str ();

	return function.getName ().str ();
}

bool
is_published_body (const Function &function)
{
	return !function.isDeclaration () && !function.hasLocalLinkage ();
}

} // namespace mono
