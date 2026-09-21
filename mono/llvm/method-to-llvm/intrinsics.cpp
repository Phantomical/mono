/**
 * \file
 * \brief Selecting the built-in lowering a call or a body gets.
 */

#include "intrinsics.hpp"

#include "method-to-llvm.hpp"
#include "hidden-return.hpp"
#include "mini-runtime.h"

#include "../runtime/options.hpp"

#include "mono/metadata/class-init.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/gc-internals.h"
#include "mono/metadata/image.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/reflection-internals.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Alignment.h>

#include <iterator>
#include <string_view>
#include <vector>

namespace mono {

/// The emitters the tables below point at. MethodLLVMEmitter befriends this
/// struct, so an emitter added here reaches that class's own emitters.
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
		if (!is_array_shape_builtin (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_rank (builder);
	}

	static BuiltinResult array_length (MethodLLVMEmitter &emitter,
	                                   llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		if (!is_array_shape_builtin (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_total_length (builder);
	}

	static BuiltinResult array_dimension (MethodLLVMEmitter &emitter,
	                                      llvm::IRBuilder<> &builder,
	                                      const BuiltinCall &call)
	{
		if (!is_array_shape_builtin (call.callee, call.sig))
			return std::nullopt;

		return emitter.emit_array_dimension (builder, call.callee, false);
	}

	static BuiltinResult array_lower_bound (MethodLLVMEmitter &emitter,
	                                        llvm::IRBuilder<> &builder,
	                                        const BuiltinCall &call)
	{
		if (!is_array_shape_builtin (call.callee, call.sig))
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

	static BuiltinResult array_clear (MethodLLVMEmitter &emitter,
	                                  llvm::IRBuilder<> &builder, const BuiltinCall &)
	{
		MonoClass *element = emitter.array_clear_element_class ();

		if (element == nullptr)
			return std::nullopt;

		return emitter.emit_array_clear (builder, element);
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

	static BuiltinResult thread_memory_barrier (MethodLLVMEmitter &emitter,
	                                            llvm::IRBuilder<> &builder,
	                                            const BuiltinCall &)
	{
		return emitter.emit_thread_memory_barrier (builder);
	}

	static BuiltinResult thread_spin_wait_nop (MethodLLVMEmitter &emitter,
	                                           llvm::IRBuilder<> &builder,
	                                           const BuiltinCall &)
	{
		return emitter.emit_thread_spin_wait_nop (builder);
	}

	/// Fold RuntimeHelpers.IsReferenceOrContainsReferences<T> for concrete T.
	static BuiltinResult is_reference_or_contains_references (
		MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder, const BuiltinCall &call)
	{
		MonoGenericContext *ctx = mono_method_get_context (call.callee);

		if (ctx == nullptr || ctx->method_inst == nullptr
		    || ctx->method_inst->type_argc != 1)
			return std::nullopt;

		MonoType *t = mini_get_underlying_type (ctx->method_inst->type_argv[0]);
		MonoClass *klass = mono_class_from_mono_type_internal (t);

		if (emitter.depends_on_context (klass))
			return std::nullopt;

		bool has_references;

		if (MONO_TYPE_IS_REFERENCE (t))
			has_references = true;
		else if (MONO_TYPE_IS_PRIMITIVE (t))
			has_references = false;
		else {
			mono_class_init_internal (klass);
			has_references = m_class_has_references (klass) != 0;
		}

		return emitter.push_produced (builder, builder.getInt8 (has_references ? 1 : 0),
		                              call.sig->ret);
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
	 * runtime-invoke wrapper is how it gets there.
	 *
	 * Remoting wrappers retain the wrapped method's class and name. Do not
	 * expand calls inside the with-check wrapper itself because they must
	 * preserve its proxy dispatch.
	 */
	static BuiltinResult get_type (MethodLLVMEmitter &emitter, llvm::IRBuilder<> &builder,
	                               const BuiltinCall &call)
	{
#ifndef DISABLE_REMOTING
		if (call.caller->wrapper_type == MONO_WRAPPER_RUNTIME_INVOKE)
			return std::nullopt;
		if (call.caller->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK)
			return std::nullopt;
#endif

		return emitter.emit_get_type (builder,
		                              call.constrained != nullptr && !call.box_receiver);
	}

	// InternalGetHashCode is reached only from Object.GetHashCode (). Use the
	// address formula for non-moving collectors and the cached hash for moving
	// collectors, matching tier 0's collector check.
	static BuiltinResult get_hash_code (MethodLLVMEmitter &emitter,
	                                    llvm::IRBuilder<> &builder,
	                                    const BuiltinCall &call)
	{
		if (!hash_code_fast_path ())
			return std::nullopt;

		if (!mono_gc_is_moving ())
			return emitter.emit_hash_code_pointer_fast_path (builder, call.sig);

		return emitter.emit_hash_code_fast_path (builder, call.callee, call.sig);
	}

	/// ByReference<T> is a contract with the JIT, not code. Its IL bodies only
	/// throw, and the JIT must substitute the real semantics itself. The
	/// struct is one interior pointer. The constructor stores it, and the
	/// getter loads it.
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

	/// Implements System.Numerics.Vector.get_IsHardwareAccelerated.
	static BuiltinResult vector_is_hardware_accelerated (MethodLLVMEmitter &,
	                                                     llvm::IRBuilder<> &builder,
	                                                     MonoMethod *)
	{
		builder.CreateRet (builder.getInt8 (1));
		return llvm::Error::success ();
	}
};

namespace {

/// What an entry's method does with a receiver.
enum class Receiver {
	any,
	one,
	none,
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
	// inliner has already inlined a forwarded parameter into a constant.
	{ "GetLength", 1, Receiver::one, BuiltinEmitters::array_dimension },
	{ "GetLowerBound", 1, Receiver::one, BuiltinEmitters::array_lower_bound },

	{ "Clear", 3, Receiver::none, BuiltinEmitters::array_clear },
};

const BuiltinMethod string_methods[] = {
	{ "get_Length", 0, Receiver::one, BuiltinEmitters::string_length },
	{ "FastAllocateString", 1, Receiver::none, BuiltinEmitters::string_alloc },
};

const BuiltinMethod object_methods[] = {
	{ "GetType", 0, Receiver::one, BuiltinEmitters::get_type },
	{ "InternalGetHashCode", 1, Receiver::none, BuiltinEmitters::get_hash_code },
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

// Interlocked.MemoryBarrier () forwards to Thread.MemoryBarrier ().
const BuiltinMethod thread_methods[] = {
	{ "MemoryBarrier", 0, Receiver::none, BuiltinEmitters::thread_memory_barrier },
	{ "SpinWait_nop", 0, Receiver::none, BuiltinEmitters::thread_spin_wait_nop },
};

const BuiltinMethod debugger_methods[] = {
	{ "Break", 0, Receiver::none, BuiltinEmitters::debugger_break },
};

const BuiltinMethod runtime_helpers_methods[] = {
	{ "IsReferenceOrContainsReferences", 0, Receiver::none,
	  BuiltinEmitters::is_reference_or_contains_references },
};

/// ByReference`1's row takes every member the class has. One with no lowering
/// is refused rather than left to run IL that only throws.
const BuiltinBody core_bodies[] = {
	{ { nullptr, "System", "ByReference`1" }, {}, any_signature, false, nullptr,
	  BuiltinEmitters::byreference },
};

/// System.Numerics.Vector.get_IsHardwareAccelerated, whichever assembly
/// declares the class - corlib's own managed getter still returns false, the
/// same as a standalone System.Numerics.Vectors package's would. The
/// attribute check prevents an unrelated type with the same qualified name
/// from matching. This entry bypasses carries_builtins () because an
/// assembly outside corlib is not known in advance.
const BuiltinBody vector_hwaccel_body = {
	{ any_assembly, "System.Numerics", "Vector" }, "get_IsHardwareAccelerated",
	"", false, nullptr, BuiltinEmitters::vector_is_hardware_accelerated,
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

const std::vector<BuiltinBody> &
body_table ()
{
	static const std::vector<BuiltinBody> entries = [] {
		std::vector<BuiltinBody> made (std::begin (core_bodies),
		                               std::end (core_bodies));

		made.insert (made.end (), simd_bodies ().begin (), simd_bodies ().end ());
		made.insert (made.end (), simd_numerics_bodies ().begin (),
		             simd_numerics_bodies ().end ());
		made.insert (made.end (), simd_vector_t_bodies ().begin (),
		             simd_vector_t_bodies ().end ());
		made.insert (made.end (), simd_x86_bodies ().begin (), simd_x86_bodies ().end ());
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
		{ { nullptr, "System.Threading", "Thread" }, &mono_defaults.thread_class,
		  thread_methods },
		{ { nullptr, "System.Diagnostics", "Debugger" }, nullptr, debugger_methods },
		{ { nullptr, "System.Runtime.CompilerServices", "RuntimeHelpers" }, nullptr,
		  runtime_helpers_methods },
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
		for (const BuiltinBody &entry : body_table ())
			if (entry.klass.assembly != nullptr)
				made.push_back (entry.klass.assembly);

		return made;
	} ();

	return names;
}

/// Treat classes from `name` as belonging to `canonical` during builtin lookup.
struct AssemblyAlias {
	std::string_view name;
	/// Null for corlib.
	const char *canonical;
};

constexpr AssemblyAlias assembly_aliases[] = {
	// Older versions define Vector and Vector<T> instead of forwarding them to corlib.
	{ "System.Numerics.Vectors", nullptr },
};

/// Return the assembly identity used to match an image against a ClassKey.
/// Corlib and its aliases use a null identity.
std::optional<const char *>
assembly_identity (MonoImage *image)
{
	if (image == mono_defaults.corlib)
		return nullptr;

	const char *from = mono_image_get_name (image);

	if (from == nullptr)
		return std::nullopt;

	for (const AssemblyAlias &alias : assembly_aliases)
		if (alias.name == from)
			return alias.canonical;

	return from;
}

/// Whether any entry can name a method in image.
///
/// A call this registry might answer asks it first, before any name is
/// compared.
bool
carries_builtins (MonoImage *image)
{
	std::optional<const char *> identity = assembly_identity (image);

	if (!identity)
		return false;
	if (*identity == nullptr)
		return true;

	for (std::string_view assembly : builtin_assemblies ())
		if (assembly == *identity)
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
	if (key.assembly != nullptr && std::string_view (key.assembly) == any_assembly)
		return true;

	std::optional<const char *> identity = assembly_identity (m_class_get_image (klass));

	if (!identity)
		return false;
	if (key.assembly == nullptr)
		return *identity == nullptr;

	return *identity != nullptr && std::string_view (*identity) == key.assembly;
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

/// Whether t arrives as a vector, which is what convert_vtype () makes of a
/// class the loader marked simd_type.
bool
travels_as_a_vector (MonoType *t)
{
	if (t->byref)
		return false;

	MonoClass *klass = mono_class_from_mono_type_internal (t);

	return klass != nullptr && m_class_is_simd_type (klass);
}

/// Whether sig's parameters match what params encodes.
bool
names_params (std::string_view params, MonoMethodSignature *sig)
{
	if (params.size () != (size_t) sig->param_count)
		return false;

	for (size_t i = 0; i < params.size (); ++i) {
		switch (params[i]) {
		case 'V':
			if (!travels_as_a_vector (sig->params[i]))
				return false;
			break;
		case 'S':
			if (travels_as_a_vector (sig->params[i]))
				return false;
			break;
		default:
			return false;
		}
	}

	return true;
}

constexpr std::string_view x86_intrinsics_name_space = "System.Runtime.Intrinsics.X86";

/// Return the IntrinsicAttribute class from corlib.
MonoClass *
intrinsic_attribute_class ()
{
	static MonoClass *cached;

	if (cached)
		return cached;

	cached = mono_class_from_name (mono_defaults.corlib, "System.Runtime.CompilerServices",
	                                "IntrinsicAttribute");
	return cached;
}

/// Whether klass has IntrinsicAttribute.
bool
class_has_intrinsic_attribute (MonoClass *klass)
{
	MonoClass *attr_klass = intrinsic_attribute_class ();

	if (attr_klass == nullptr)
		return false;

	MonoCustomAttrInfo *cinfo = mono_custom_attrs_from_class (klass);

	if (cinfo == nullptr)
		return false;

	bool result = mono_custom_attrs_has_attr (cinfo, attr_klass);

	if (!cinfo->cached)
		mono_custom_attrs_free (cinfo);

	return result;
}

/// Whether entry is the row for method.
bool
names_body (const BuiltinBody &entry, MonoMethod *method)
{
	if (!names_class (entry.klass, method->klass))
		return false;
	if (!entry.name.empty () && entry.name != std::string_view (method->name))
		return false;
	if (entry.params != any_signature) {
		MonoMethodSignature *sig = mono_method_signature_internal (method);

		if (sig == nullptr || !names_params (entry.params, sig))
			return false;
	}

	// Only the x86 intrinsic tables require IntrinsicAttribute. The other
	// builtin tables contain classes that do not carry the attribute.
	if (std::string_view (entry.klass.name_space) != x86_intrinsics_name_space)
		return true;

	return class_has_intrinsic_attribute (method->klass);
}

/// Whether method has System.Runtime.CompilerServices.IntrinsicAttribute.
/// Standalone assemblies define their own attribute because corlib's type is
/// internal, so compare its qualified name rather than its type identity.
bool
method_has_intrinsic_attribute (MonoMethod *method)
{
	ERROR_DECL (error);
	MonoCustomAttrInfo *cinfo = mono_custom_attrs_from_method_checked (method, error);

	if (!is_ok (error) || cinfo == nullptr) {
		mono_error_cleanup (error);
		return false;
	}

	bool found = false;

	for (int i = 0; i < cinfo->num_attrs; ++i) {
		MonoClass *attr_klass = cinfo->attrs [i].ctor != nullptr ? cinfo->attrs [i].ctor->klass : nullptr;

		if (attr_klass != nullptr
		    && std::string_view (m_class_get_name_space (attr_klass)) == "System.Runtime.CompilerServices"
		    && std::string_view (m_class_get_name (attr_klass)) == "IntrinsicAttribute") {
			found = true;
			break;
		}
	}

	if (!cinfo->cached)
		mono_custom_attrs_free (cinfo);

	return found;
}

} // namespace

const BuiltinBody *
builtin_body_for (MonoMethod *method)
{
	// Check the method name first because reading custom attributes requires a
	// metadata lookup.
	if (names_body (vector_hwaccel_body, method) && method_has_intrinsic_attribute (method))
		return &vector_hwaccel_body;

	if (!carries_builtins (m_class_get_image (method->klass)))
		return nullptr;

	for (const BuiltinBody &entry : body_table ()) {
		if (entry.enabled != nullptr && !entry.enabled ())
			continue;
		if (names_body (entry, method))
			return &entry;
	}

	return nullptr;
}

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
	const BuiltinBody *entry = builtin_body_for (method);

	if (entry == nullptr)
		return std::nullopt;

	return entry->emit (emitter, builder, method);
}

bool
builtin_body_replaces_il (MonoMethod *method)
{
	const BuiltinBody *entry = builtin_body_for (method);

	return entry != nullptr && !entry->il_agrees;
}

bool
is_vector_hardware_accelerated_getter (MonoMethod *method)
{
	return builtin_body_for (method) == &vector_hwaccel_body;
}

} // namespace mono
