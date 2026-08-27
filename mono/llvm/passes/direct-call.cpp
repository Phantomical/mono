/**
 * \file
 * \brief What a caller has to know to enter a method without dispatching.
 */

#include "direct-call.hpp"

#include "compile-state.hpp"
#include "method-symbols.hpp"
#include "runtime/naming.hpp"

#include "mini.h"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {

MonoMethod *
nameable (MonoMethod *target)
{
	// A class that leaves an inherited abstract method unimplemented has no
	// body to enter, and no instances either, so the site that raises stands.
	if (target == nullptr || (target->flags & METHOD_ATTRIBUTE_ABSTRACT) != 0)
		return nullptr;

	MonoMethodSignature *sig = mono_method_signature_internal (target);

	if (sig == nullptr)
		return nullptr;

	/*
	 * A virtual generic method's slot holds a trampoline that reads the
	 * asked-for instantiation out of the IMT register. So a site reading the
	 * slot alone enters no one method. A site that carries the key names an
	 * instantiation, and that is the method this answers with.
	 */
	if (sig->generic_param_count != 0) {
		MonoGenericContext *bound = mono_method_get_context (target);

		if (bound == nullptr || bound->method_inst == nullptr)
			return nullptr;
	}

	// An icall, a pinvoke or a runtime-implemented method is declared in the C
	// convention, which is not the shape the site calls with.
	if (implemented_outside_il (target))
		return nullptr;

	/*
	 * A method whose entry needs a context is one no caller enters directly: it
	 * would arrive with the receiver of some other instantiation, or none.
	 */
	if (mono_method_check_context_used (target) != 0)
		return nullptr;

	/*
	 * The runtime puts the wrapper where dispatching finds it. A direct call has
	 * to name the wrapper itself, or the body runs without its lock. The site
	 * calls what comes back with the prototype it already had, and a vararg
	 * wrapper is not that shape - emit_call () refuses the same pair rather than
	 * building one.
	 */
	if ((target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) != 0)
		return sig->call_convention == MONO_CALL_VARARG
		               ? nullptr
		               : mono_marshal_get_synchronized_wrapper (target);

	return target;
}

Function *
entry_for (Module &m, MonoMethod *target, FunctionType *shape, const CompileState &compile)
{
	/*
	 * A placeholder name, the way the translator writes one: the engine reads
	 * the marker and renames this to whatever it publishes the entry under.
	 * Nothing has to agree with it in advance.
	 */
	Function *decl = Function::Create (shape, GlobalValue::ExternalLinkage,
	                                   "mono_direct_" + identity_of (target), &m);

	mark_method_reference (*decl, target);

	/*
	 * decl is not this function's any more. Naming folds it into a declaration
	 * the module already held for the method and erases the loser, so reading
	 * it back - even to see whether it is worth erasing - is a read of freed
	 * memory. What is left standing when the publish fails is a declaration
	 * nothing calls, which no relocation and no code comes of.
	 */
	Function *entry = compile.publish (*decl, target);

	/*
	 * A declaration the module already held carries the shape its own site
	 * calls it with, and this site's shape is what the IL settled here. The two
	 * agree for an override, which has the signature it overrides. Where they
	 * do not, the site keeps its lookup rather than calling one prototype
	 * through the other.
	 */
	if (entry != nullptr && entry->getFunctionType () != shape)
		return nullptr;

	return entry;
}

} // namespace mono
