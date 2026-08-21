#include "builtins.hpp"
#include "domain-method.hpp"
#include "mini.h"
#include "mono/metadata/appdomain.h"
#include "mono/metadata/marshal.h"
#include <llvm/Support/DynamicLibrary.h>
#include <vector>

#undef PIC

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/RuntimeLibcalls.h>

#include <unwind.h>
#include <initializer_list>

namespace mono {
namespace {

template<typename T, typename C>
void
append (std::vector<T> &vec, const C &collection)
{
	vec.insert (vec.end (), collection.begin (), collection.end ());
}

/// The personality function for the landing pads we generate. Mono uses its own
/// custom unwinder, so this is never called by us.
_Unwind_Reason_Code
jit_personality (int, _Unwind_Action, _Unwind_Exception_Class, struct _Unwind_Exception *,
                 struct _Unwind_Context *)
{
	return _URC_CONTINUE_UNWIND;
}

/*
 * What a body's entry counter calls when it runs out.
 *
 * It takes the record rather than the method because the record pairs the
 * method with the domain the code was compiled for. The calling thread's
 * domain is not reliable: AppDomain:InvokeInDomain moves it before the call
 * arrives.
 */
void
mono_llvm_jit_tier2_promote (MonoDomainMethod *dm)
{
	dm->promote ();
}

MonoObject *
mono_llvm_load_error_exception (MonoErrorBoxed *failure)
{
	ERROR_DECL (error);
	mono_error_set_from_boxed (error, failure);
	return (MonoObject *) mono_error_convert_to_exception (error);
}

void
get_runtime_builtins (std::vector<MonoBuiltin> &builtins)
{
	std::initializer_list<MonoBuiltin> array = {
		{"mono_domain_get", (void *) &mono_domain_get},
		{"mono_llvm_jit_tier2_promote", (void *) &mono_llvm_jit_tier2_promote},
		{"mono_marshal_set_last_error", (void *) &mono_marshal_set_last_error},
		{"mono_gc_wbarrier_generic_store_internal",
	         (void *) &mono_gc_wbarrier_generic_store_internal},
		{"mono_gc_wbarrier_value_copy_internal",
	         (void *) &mono_gc_wbarrier_value_copy_internal},

		/*
		 * The throw path. These are mono's own throw trampolines. Each
		 * captures the register state and enters mono_handle_exception (),
		 * which finds a handler with a two-pass search over the MonoJitInfo
		 * published per method - the native unwinder is never involved.
		 *
		 * The corlib variant takes the exception's type-def index as an
		 * argument and reads the throw site out of the return address.
		 * resume_unwind is what a
		 * finally or fault calls when it was entered by unwinding and has
		 * run out.
		 */
		{"mono_llvm_throw_exception", mono_get_throw_exception ()},
		{"mono_llvm_rethrow_exception", mono_get_rethrow_exception ()},
		{"mono_llvm_throw_corlib_exception",
	         (void *) mono_find_jit_icall_info (
			 MONO_JIT_ICALL_mono_llvm_throw_corlib_exception_abs_trampoline)
	                 ->func},
		{"mono_llvm_resume_unwind",
	         (void *) mono_find_jit_icall_info (
			 MONO_JIT_ICALL_mono_llvm_resume_unwind_trampoline)
	                 ->func},

		/*
		 * What a stand-in body calls to build the exception it throws in
		 * place of a method that failed to compile. A metadata load that
		 * failed is one such failure. At a thunk, where a miss cannot go
		 * back to the caller, any other failure becomes one as well.
		 */
		{"mono_llvm_load_error_exception", (void *) &mono_llvm_load_error_exception},

		/*
		 * The personality routine a landing pad names. Generated code never
		 * calls it. The unwinder does, on the way through a frame that has a
		 * handler.
		 */
		{"mono_personality", (void *) &jit_personality},

		/*
		 * Not a runtime libcall as far as RuntimeLibcallsInfo is concerned -
		 * amd64 has no MEMCMP libcall - so get_libcall_builtins () does not
		 * cover it, but MergeICmps builds calls to it at the IR level.
		 */
		{"memcmp", (void *) &memcmp},
	};

	append (builtins, array);
}

void
get_libcall_builtins (std::vector<MonoBuiltin> &builtins, const llvm::Triple &triple)
{
	llvm::DenseSet<llvm::StringRef> skip (builtins.size ());
	for (const auto &builtin : builtins)
		skip.insert (builtin.name);

	/* Without this the search below only sees libraries LLVM itself opened. */
	llvm::sys::DynamicLibrary::LoadLibraryPermanently (nullptr);

	llvm::RTLIB::RuntimeLibcallsInfo info (triple);
	for (auto impl : llvm::RTLIB::libcall_impls ()) {
		if (!info.isAvailable (impl))
			continue;

		auto name = llvm::RTLIB::RuntimeLibcallsInfo::getLibcallImplName (impl);
		if (name.empty () || skip.contains (name))
			continue;

		if (void *addr = llvm::sys::DynamicLibrary::SearchForAddressOfSymbol (name.data ()))
			builtins.push_back ({name, addr});
	}
}

} // namespace

std::vector<MonoBuiltin>
MonoBuiltin::get_platform_builtins (const llvm::Triple &triple)
{
	std::vector<MonoBuiltin> builtins;
	get_runtime_builtins (builtins);
	get_libcall_builtins (builtins, triple);
	return builtins;
}

} // namespace mono
