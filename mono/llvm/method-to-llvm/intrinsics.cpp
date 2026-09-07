/**
 * \file
 * \brief Selecting the built-in lowering a call or a body gets.
 */

#include "intrinsics.hpp"

#include "method-to-llvm.hpp"
#include "hidden-return.hpp"

#include "mono/metadata/class-internals.h"
#include "mono/metadata/image.h"
#include "mono/metadata/metadata.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Alignment.h>

#include <string_view>
#include <vector>

namespace mono {

/**
 * The emitters the tables below point at. MethodLLVMEmitter befriends this
 * struct, so an emitter added here reaches that class's own emitters.
 */
struct BuiltinEmitters {
	static BuiltinResult unsafe_mov (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		return emitter.emit_unsafe_mov (builder, call.sig);
	}

	static BuiltinResult generic_access (MethodLLVMEmitter &emitter,
	                                     llvm::IRBuilder<> &builder,
	                                     const BuiltinCall &call)
	{
		std::optional<ArrayGenericAccess> access =
			array_generic_access_for (call.callee, call.sig);

		if (!access)
			return std::nullopt;

		return emitter.emit_array_generic_access (builder, call.sig, *access);
	}

	static BuiltinResult array_rank (MethodLLVMEmitter &emitter,
	                                 llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		if (!answers_array_shape (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_rank (builder);
	}

	static BuiltinResult array_length (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		if (!answers_array_shape (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_total_length (builder);
	}

	static BuiltinResult array_dimension (MethodLLVMEmitter &emitter,
	                                      llvm::IRBuilder<> &builder,
	                                      const BuiltinCall &call)
	{
		if (!answers_array_shape (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_dimension (builder, call.callee, false);
	}

	static BuiltinResult array_lower_bound (MethodLLVMEmitter &emitter,
	                                        llvm::IRBuilder<> &builder,
	                                        const BuiltinCall &call)
	{
		if (!answers_array_shape (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_dimension (builder, call.callee, true);
	}

	static BuiltinResult string_length (MethodLLVMEmitter &emitter,
	                                    llvm::IRBuilder<> &builder, const BuiltinCall &)
	{
		return emitter.emit_string_length (builder);
	}

	static BuiltinResult string_alloc (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		return emitter.emit_string_alloc_call (builder, call.sig);
	}

	static BuiltinResult math (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                           const BuiltinCall &call)
	{
		std::optional<MathIntrinsic> arithmetic =
			math_intrinsic_for (call.callee, call.sig);

		if (!arithmetic)
			return std::nullopt;

		return emitter.emit_math_call (builder, *arithmetic, call.sig);
	}

	static BuiltinResult buffer_copy (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		std::optional<BufferCopy> copy = buffer_copy_for (call.callee, call.sig);

		if (!copy)
			return std::nullopt;

		return emitter.emit_buffer_copy (builder, *copy, call.sig);
	}

	static BuiltinResult cor_element_type (MethodLLVMEmitter &emitter,
	                                       llvm::IRBuilder<> &builder,
	                                       const BuiltinCall &call)
	{
		if (!answers_cor_element_type (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_cor_element_type (builder, call.sig);
	}

	static BuiltinResult element_type (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		if (!answers_element_type (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_element_type (builder, call.callee, call.sig);
	}

	static BuiltinResult monitor_enter (MethodLLVMEmitter &emitter,
	                                    llvm::IRBuilder<> &builder,
	                                    const BuiltinCall &call)
	{
		std::optional<MonoJitICallId> fast =
			monitor_enter_fast_icall (call.callee, call.sig);

		if (!fast)
			return std::nullopt;

		return emitter.emit_monitor_fast_path (builder, call.callee, call.sig, *fast);
	}

	static BuiltinResult monitor_exit (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		std::optional<MonoJitICallId> fast =
			monitor_exit_fast_icall (call.callee, call.sig);

		if (!fast)
			return std::nullopt;

		return emitter.emit_monitor_fast_path (builder, call.callee, call.sig, *fast);
	}

	static BuiltinResult managed_thread_id (MethodLLVMEmitter &emitter,
	                                        llvm::IRBuilder<> &builder,
	                                        const BuiltinCall &call)
	{
		if (!is_current_managed_thread_id (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_current_managed_thread_id (builder, call.sig);
	}

	/// Debugger.Break () has an empty body and a comment where the code goes:
	/// the JIT gives the call its meaning, and that meaning is the one the break
	/// instruction has. An embedder can say no through mono_set_break_policy.
	static BuiltinResult debugger_break (MethodLLVMEmitter &emitter,
	                                     llvm::IRBuilder<> &builder,
	                                     const BuiltinCall &call)
	{
		if (!mini_should_insert_breakpoint (call.caller))
			return llvm::Error::success ();

		return emitter.emit_user_break (builder);
	}

	/**
	 * object.GetType () is an internal call, so it is published as a
	 * managed-to-native wrapper and reached through a remoting with-check
	 * wrapper. The receiver's vtable carries the answer, so the site becomes
	 * one load.
	 *
	 * Reflection on a proxy gives GetType () a meaning of its own, and the
	 * runtime-invoke wrapper is how it gets there. The interpreter refuses the
	 * same site (mono/interp/transform/intrinsics.cpp).
	 */
	static BuiltinResult get_type (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               const BuiltinCall &call)
	{
#ifndef DISABLE_REMOTING
		if (call.caller->wrapper_type == MONO_WRAPPER_RUNTIME_INVOKE)
			return std::nullopt;
#endif

		return emitter.emit_get_type (builder,
		                              call.constrained != nullptr && !call.box_receiver);
	}

	/**
	 * ByReference<T> is a contract with the JIT, not code. Its IL bodies only
	 * throw, and the JIT must substitute the real semantics itself. The struct
	 * is one interior pointer. The constructor stores it, and the getter loads
	 * it.
	 */
	static BuiltinResult byreference (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, MonoMethod *method)
	{
		llvm::Align align (TARGET_SIZEOF_VOID_P);
		std::string_view name = method->name;
		auto argument = [&] (unsigned i) {
			return emitter.function->getArg (
				natural_parameter_index (i, emitter.function));
		};

		if (name == ".ctor") {
			builder.CreateAlignedStore (argument (1), argument (0), align);
			builder.CreateRetVoid ();
			return llvm::Error::success ();
		}
		if (name == "get_Value") {
			builder.CreateRet (builder.CreateAlignedLoad (
				llvm::PointerType::get (emitter.context (), 0), argument (0),
				align));
			return llvm::Error::success ();
		}

		return emitter.unsupported_il ("an unrecognized ByReference member");
	}
};

namespace {

/// The param_count of an entry that matches whatever arity the site has.
constexpr int any_params = -1;

/// What an entry's method does with a receiver.
enum class Receiver {
	any,
	one,
	none,
};

/// Where an entry's class comes from.
struct ClassKey {
	/// The assembly's name, or null for corlib.
	const char *assembly;
	const char *name_space;
	const char *name;
};

/// One method the registry answers a call to.
struct BuiltinMethod {
	std::string_view name;
	/// The arity this entry is written for, or any_params.
	int param_count;
	Receiver receiver;
	/// Writes what the call compiles to, or answers nothing to leave the call
	/// standing. An emitter that answers nothing must have emitted nothing,
	/// because the entry behind it is asked next.
	BuiltinResult (*emit) (MethodLLVMEmitter &, llvm::IRBuilder<> &, const BuiltinCall &);
};

/// One class the registry has methods under.
struct BuiltinClass {
	ClassKey klass;
	/// The mono_defaults slot the class sits in, which makes the match a
	/// pointer compare. Null for a class matched by name.
	MonoClass *const *known;
	llvm::ArrayRef<BuiltinMethod> methods;
};

/// One method the backend writes the whole body of.
struct BuiltinBody {
	ClassKey klass;
	/// Whether the method's own IL computes what this body computes. False
	/// keeps the method out of every engine that runs the IL.
	bool il_agrees;
	BuiltinResult (*emit) (MethodLLVMEmitter &, llvm::IRBuilder<> &, MonoMethod *);
};

const BuiltinMethod array_methods[] = {
	{ "UnsafeMov", any_params, Receiver::any, BuiltinEmitters::unsafe_mov },

	// The body corlib gives these two is a call to an icall that reads the
	// element size off the array and moves that many bytes. The element type is
	// known here, so the site becomes an address and one access.
	{ "GetGenericValueImpl", 2, Receiver::one, BuiltinEmitters::generic_access },
	{ "SetGenericValueImpl", 2, Receiver::one, BuiltinEmitters::generic_access },

	{ "get_Rank", 0, Receiver::one, BuiltinEmitters::array_rank },
	{ "GetRank", 0, Receiver::one, BuiltinEmitters::array_rank },
	{ "get_Length", 0, Receiver::one, BuiltinEmitters::array_length },

	// Whatever dimension the site names. lower_array_shapes () leaves the ones
	// it cannot read on the accessor, and it reads the dimension where an
	// inliner has already folded a forwarded parameter into a constant.
	{ "GetLength", 1, Receiver::one, BuiltinEmitters::array_dimension },
	{ "GetLowerBound", 1, Receiver::one, BuiltinEmitters::array_lower_bound },
};

const BuiltinMethod string_methods[] = {
	{ "get_Length", 0, Receiver::one, BuiltinEmitters::string_length },
	{ "FastAllocateString", 1, Receiver::none, BuiltinEmitters::string_alloc },
};

const BuiltinMethod object_methods[] = {
	{ "GetType", 0, Receiver::one, BuiltinEmitters::get_type },
};

const BuiltinMethod runtime_imports_methods[] = {
	{ "Memcpy", 3, Receiver::none, BuiltinEmitters::buffer_copy },
	{ "Memmove", 3, Receiver::none, BuiltinEmitters::buffer_copy },
};

// Each of these reads what the icall reads, and Array.GetValue () asks both
// once for every element.
const BuiltinMethod type_handle_methods[] = {
	{ "GetCorElementType", 1, Receiver::none, BuiltinEmitters::cor_element_type },
	{ "GetElementType", 1, Receiver::none, BuiltinEmitters::element_type },
};

const BuiltinMethod monitor_methods[] = {
	{ "Enter", 1, Receiver::none, BuiltinEmitters::monitor_enter },
	{ "Enter", 2, Receiver::none, BuiltinEmitters::monitor_enter },
	{ "Exit", 1, Receiver::none, BuiltinEmitters::monitor_exit },
};

const BuiltinMethod environment_methods[] = {
	{ "get_CurrentManagedThreadId", 0, Receiver::none,
	  BuiltinEmitters::managed_thread_id },
};

const BuiltinMethod debugger_methods[] = {
	{ "Break", 0, Receiver::none, BuiltinEmitters::debugger_break },
};

const BuiltinBody body_table[] = {
	{ { nullptr, "System", "ByReference`1" }, false, BuiltinEmitters::byreference },
};

/// The entries System.Math and System.MathF share, one for each name
/// math_intrinsic_for () can answer.
llvm::ArrayRef<BuiltinMethod>
math_methods ()
{
	static const std::vector<BuiltinMethod> entries = [] {
		std::vector<BuiltinMethod> made;

		for (const MathBuiltin &name : math_builtins ())
			made.push_back ({ name.name, name.param_count, Receiver::none,
			                  BuiltinEmitters::math });

		return made;
	} ();

	return entries;
}

const std::vector<BuiltinClass> &
class_table ()
{
	static const std::vector<BuiltinClass> entries = {
		{ { nullptr, "System", "Array" }, &mono_defaults.array_class, array_methods },
		{ { nullptr, "System", "String" }, &mono_defaults.string_class,
		  string_methods },
		{ { nullptr, "System", "Object" }, &mono_defaults.object_class,
		  object_methods },
		{ { nullptr, "System", "Math" }, nullptr, math_methods () },
		{ { nullptr, "System", "MathF" }, nullptr, math_methods () },
		{ { nullptr, "System.Runtime", "RuntimeImports" }, nullptr,
		  runtime_imports_methods },
		{ { nullptr, "System", "RuntimeTypeHandle" }, nullptr, type_handle_methods },
		{ { nullptr, "System.Threading", "Monitor" }, nullptr, monitor_methods },
		{ { nullptr, "System", "Environment" }, nullptr, environment_methods },
		{ { nullptr, "System.Diagnostics", "Debugger" }, nullptr, debugger_methods },
	};

	return entries;
}

/// The classes above, under the name each one is declared with.
const llvm::StringMap<llvm::SmallVector<const BuiltinClass *, 1>> &
class_index ()
{
	static const llvm::StringMap<llvm::SmallVector<const BuiltinClass *, 1>> index = [] {
		llvm::StringMap<llvm::SmallVector<const BuiltinClass *, 1>> made;

		for (const BuiltinClass &entry : class_table ())
			made[entry.klass.name].push_back (&entry);

		return made;
	} ();

	return index;
}

/// The assemblies other than corlib that the tables above name.
const std::vector<std::string_view> &
builtin_assemblies ()
{
	static const std::vector<std::string_view> names = [] {
		std::vector<std::string_view> made;

		for (const BuiltinClass &entry : class_table ())
			if (entry.klass.assembly != nullptr)
				made.push_back (entry.klass.assembly);
		for (const BuiltinBody &entry : body_table)
			if (entry.klass.assembly != nullptr)
				made.push_back (entry.klass.assembly);

		return made;
	} ();

	return names;
}

/// Whether any entry can name a method in image.
///
/// Every call the translator writes asks this first, which is why corlib is a
/// pointer compare.
bool
carries_builtins (MonoImage *image)
{
	if (image == mono_defaults.corlib)
		return true;

	const char *from = mono_image_get_name (image);

	if (from == nullptr)
		return false;

	for (std::string_view assembly : builtin_assemblies ())
		if (assembly == from)
			return true;

	return false;
}

/// Whether klass is the class key names.
bool
names_class (const ClassKey &key, MonoClass *klass)
{
	if (std::string_view (m_class_get_name (klass)) != key.name)
		return false;
	if (std::string_view (m_class_get_name_space (klass)) != key.name_space)
		return false;
	if (key.assembly == nullptr)
		return m_class_get_image (klass) == mono_defaults.corlib;

	const char *from = mono_image_get_name (m_class_get_image (klass));

	return from != nullptr && std::string_view (from) == key.assembly;
}

/// The entries under klass, empty for a class the registry has none for.
llvm::ArrayRef<BuiltinMethod>
methods_of (MonoClass *klass)
{
	auto found = class_index ().find (m_class_get_name (klass));

	if (found == class_index ().end ())
		return {};

	for (const BuiltinClass *entry : found->second) {
		if (entry->known != nullptr) {
			if (klass == *entry->known)
				return entry->methods;

			continue;
		}

		if (names_class (entry->klass, klass))
			return entry->methods;
	}

	return {};
}

const BuiltinBody *
body_of (MonoMethod *method)
{
	if (!carries_builtins (m_class_get_image (method->klass)))
		return nullptr;

	for (const BuiltinBody &entry : body_table)
		if (names_class (entry.klass, method->klass))
			return &entry;

	return nullptr;
}

} // namespace

BuiltinResult
emit_builtin_call (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
                   const BuiltinCall &call)
{
	MonoClass *klass = call.callee->klass;

	if (!carries_builtins (m_class_get_image (klass)))
		return std::nullopt;

	std::string_view name (call.callee->name);

	for (const BuiltinMethod &entry : methods_of (klass)) {
		if (entry.name != name)
			continue;
		if (entry.param_count != any_params
		    && entry.param_count != call.sig->param_count)
			continue;
		if (entry.receiver != Receiver::any
		    && (entry.receiver == Receiver::one) != (bool) call.sig->hasthis)
			continue;

		if (BuiltinResult answer = entry.emit (emitter, builder, call))
			return answer;
	}

	return std::nullopt;
}

BuiltinResult
emit_builtin_body (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
                   MonoMethod *method)
{
	const BuiltinBody *entry = body_of (method);

	if (entry == nullptr)
		return std::nullopt;

	return entry->emit (emitter, builder, method);
}

bool
builtin_body_replaces_il (MonoMethod *method)
{
	const BuiltinBody *entry = body_of (method);

	return entry != nullptr && !entry->il_agrees;
}

} // namespace mono
