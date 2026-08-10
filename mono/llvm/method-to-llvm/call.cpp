#include "method-to-llvm.hpp"
#include "hidden-return.hpp"
#include "mini-runtime.h"
#include "runtime-error.hpp"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/metadata-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/remoting.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

#include <string_view>

namespace mono {

/// Whether TARGET is corlib's System.Diagnostics.Debugger::Break ().
static bool
is_debugger_break (MonoMethod *target, MonoMethodSignature *sig)
{
	MonoClass *klass = target->klass;

	return sig->param_count == 0 && !sig->hasthis
	       && m_class_get_image (klass) == mono_defaults.corlib
	       && std::string_view (target->name) == "Break"
	       && std::string_view (m_class_get_name (klass)) == "Debugger"
	       && std::string_view (m_class_get_name_space (klass)) == "System.Diagnostics";
}

/// The method TOKEN names, resolved against this method's generic context.
llvm::Expected<MonoMethod *>
MethodLLVMEmitter::resolve_method (uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoGenericContext *context = mono_method_get_context (method);

	if (in_wrapper ()) {
		MonoMethod *target = static_cast<MonoMethod *> (wrapper_data (token));

		if (target == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not name a method");

		if (context == nullptr)
			return target;

		target = mono_class_inflate_generic_method_checked (target, context,
		                                                    metadata_error);
		if (target == nullptr)
			return runtime_error (metadata_error);

		return target;
	}

	MonoMethod *target =
		mono_get_method_checked (m_class_get_image (method->klass), token, nullptr,
	                                 context, metadata_error);

	if (target == nullptr)
		return runtime_error (metadata_error);

	return target;
}

/// The signature TARGET is being called with here, which for a vararg callee
/// names the types the caller chose for the variable part as well as the fixed
/// parameters the callee declared.
llvm::Expected<MonoMethodSignature *>
MethodLLVMEmitter::call_site_signature (MonoMethod *target, uint32_t token)
{
	/*
	 * A wrapper's call carries wrapper data where a token would be, so there is
	 * no memberref to read. What it gets is the signature as declared, whose
	 * sentinel sits past the last parameter and so names no variable part -
	 * which is right, since the IL a wrapper is built from cannot spell one.
	 */
	if (in_wrapper ())
		return mono_method_signature_internal (target);

	ERROR_DECL (metadata_error);
	MonoMethodSignature *sig = mono_method_get_signature_checked (
		target, m_class_get_image (method->klass), token,
		mono_method_get_context (method), metadata_error);

	if (sig == nullptr)
		return runtime_error (metadata_error);

	return sig;
}

/// The buffer a vararg call's variable arguments cross in, filled from ARGS,
/// whose entries are the call-site signature's parameters in order.
///
/// ves_icall_System_ArgIterator_Setup () reads the signature out of the first
/// word and starts the walk at the second, and IntGetNextArg () advances by
/// mono_type_stack_size () with no realignment of its own, so the offsets here
/// are that running sum and nothing else. A float taking four bytes rather than
/// a whole slot is part of that, and getting it wrong would not be caught by
/// the argument that follows arriving as garbage.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::build_sig_cookie (MonoIrBuilder &builder, MonoMethodSignature *sig,
                                     llvm::ArrayRef<llvm::Value *> args)
{
	const llvm::DataLayout &layout = module->getDataLayout ();
	int fixed = vararg_fixed_params (sig);
	std::vector<uint64_t> offsets;
	uint64_t cursor = TARGET_SIZEOF_VOID_P;
	uint64_t size = cursor;

	for (int i = fixed; i < sig->param_count; ++i) {
		llvm::Type *stored = args[i + sig->hasthis]->getType ();

		offsets.push_back (cursor);
		/*
		 * The stride is the stack size the iterator will advance by, but the
		 * buffer still has to be big enough for what actually gets written
		 * into the last slot.
		 */
		size = std::max (size, cursor + layout.getTypeStoreSize (stored));
		cursor += static_cast<uint64_t> (mono_type_stack_size (sig->params[i], nullptr));
		size = std::max (size, cursor);
	}

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *buffer = entry.CreateAlloca (
		llvm::ArrayType::get (builder.getInt8Ty (), size), nullptr, "arglist");

	buffer->setAlignment (llvm::Align (TARGET_SIZEOF_VOID_P));

	/*
	 * ArgIterator names the variable part by index into this signature, so it
	 * has to be the call-site one; the declaration knows only the fixed part.
	 */
	builder.CreateAlignedStore (
		address_symbol (identity_symbol ("mono_sig_", sig), sig), buffer,
		buffer->getAlign ());

	for (int i = fixed; i < sig->param_count; ++i) {
		uint64_t offset = offsets[i - fixed];
		llvm::Value *slot =
			builder.CreateGEP (builder.getInt8Ty (), buffer,
		                           builder.getInt64 (offset));

		builder.CreateAlignedStore (
			args[i + sig->hasthis], slot,
			llvm::commonAlignment (buffer->getAlign (), offset));
	}

	return buffer;
}

/// VALUE as something that can be passed where a call signature asks for DESTINATION.
///
/// Call arguments accept one mismatch that a store refuses: an int32 (or a pointer)
/// where the parameter is int64. Mini permits it on 64-bit only at call sites -
/// check_call_signature takes it, target_type_is_incompatible does not - and the
/// value rides over in the full register, so a constant arrives sign-extended.
/// The eval stack's int32 is signed, so sign-extension is the reading kept here.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::coerce_to_argument (MonoIrBuilder &builder, StackValue value,
                                       MonoType *destination, bool native)
{
	llvm::Expected<llvm::Type *> type = convert_type (destination, native);
	if (!type)
		return type.takeError ();

	llvm::Type *from = value.value->getType ();

	if (from->isIntegerTy () && (*type)->isIntegerTy ()
	    && (*type)->getIntegerBitWidth () > from->getIntegerBitWidth ())
		return builder.CreateSExt (value.value, *type);
	/* An int32 meeting a pointer-typed parameter widens the same way before the cast. */
	if (from->isIntegerTy () && (*type)->isPointerTy ()
	    && from->getIntegerBitWidth () < TARGET_SIZEOF_VOID_P * 8)
		value.value = builder.CreateSExt (value.value,
		                                  builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

	llvm::Expected<llvm::Value *> coerced =
		coerce_to_location (builder, value, destination, native);

	if (!coerced)
		return coerced.takeError ();

	/* Every signature this backend converts takes a value class by value. */
	return materialize (builder, *coerced, destination, native);
}

/// VALUE as the receiver of an instance call.
///
/// A `this` is a pointer in every signature this backend converts, but the eval stack
/// hands one over as a native int often enough to matter: pointer arithmetic, `ldind.i`
/// and the marshalling wrappers' own `ldarg; ldind.i; call instance` all leave a number
/// where the callee declared an address.
llvm::Value *
MethodLLVMEmitter::coerce_to_receiver (MonoIrBuilder &builder, llvm::Value *value)
{
	if (!value->getType ()->isIntegerTy ())
		return value;

	if (value->getType ()->getIntegerBitWidth () < TARGET_SIZEOF_VOID_P * 8)
		value = builder.CreateSExt (value, builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

	return builder.CreateIntToPtr (value, llvm::PointerType::get (context (), 0));
}

/// Take a call's arguments off the evaluation stack, converted to what the signature
/// asks for.
///
/// The receiver of an instance method is argument zero and is not in the parameter list,
/// so it is converted against the pointer every instance signature declares rather than
/// against a MonoType.
llvm::Expected<std::vector<llvm::Value *>>
MethodLLVMEmitter::pop_call_arguments (MonoIrBuilder &builder, MonoMethodSignature *sig,
                                       bool native)
{
	size_t count = sig->param_count + sig->hasthis;

	if (stack.size () < count)
		return unbalanced_stack (count);

	std::vector<llvm::Value *> args (count);

	/* The last parameter is on top, so the stack unwinds into the list backwards. */
	for (size_t i = count; i-- > 0;) {
		StackValue value = get_stack (count - 1 - i);

		if (sig->hasthis && i == 0) {
			args[i] = coerce_to_receiver (builder, value.value);
			continue;
		}

		llvm::Expected<llvm::Value *> converted = coerce_to_argument (
			builder, value, sig->params[i - sig->hasthis], native);

		if (!converted)
			return converted.takeError ();

		args[i] = *converted;
	}

	return args;
}

/// The pointer stored OFFSET bytes into the vtable of the object RECEIVER points at.
llvm::Value *
MethodLLVMEmitter::vtable_entry (MonoIrBuilder &builder, llvm::Value *receiver, int32_t offset)
{
	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), receiver,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		llvm::Align (TARGET_SIZEOF_VOID_P));

	return builder.CreateAlignedLoad (
		ptr, builder.CreateGEP (builder.getInt8Ty (), vtable, builder.getInt32 (offset)),
		llvm::Align (TARGET_SIZEOF_VOID_P));
}

/// The address of TARGET's entry in the vtable of the object RECEIVER points at.
///
/// A virtual call reads the callee out of the receiver rather than knowing it: the
/// object's vtable pointer, indexed by the slot the method was assigned when its class
/// was laid out.
llvm::Value *
MethodLLVMEmitter::virtual_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                   MonoMethod *target)
{
	return vtable_entry (builder, receiver,
	                     MONO_STRUCT_OFFSET (MonoVTable, vtable)
	                             + mono_method_get_vtable_index (target)
	                                       * TARGET_SIZEOF_VOID_P);
}

/// The address of TARGET's entry in the IMT of the object RECEIVER points at.
///
/// An interface method has no fixed vtable slot - where an implementation lands depends
/// on the class implementing it - so dispatch goes through the interface method table
/// instead, a small hash table the runtime lays out in the words immediately before
/// each MonoVTable. Its slots are therefore reached at negative offsets from the same
/// base the ordinary slots are.
llvm::Value *
MethodLLVMEmitter::interface_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                     MonoMethod *target)
{
	int32_t slot = static_cast<int32_t> (mono_method_get_imt_slot (target)) - MONO_IMT_SIZE;

	return vtable_entry (builder, receiver, slot * TARGET_SIZEOF_VOID_P);
}

/// Whether a call to TARGET has to be dispatched out of the delegate it is made on
/// rather than out of a vtable slot.
///
/// A delegate's Invoke has no body anyone compiles. The runtime picks an implementation
/// per delegate object - an arch stub that shuffles the receiver and jumps through
/// method_ptr for a single-target delegate, the compiled delegate-invoke wrapper for a
/// multicast or open-instance one, which is the shape method_ptr cannot express - and
/// leaves it in the object's invoke_impl field. Reading that field is what keeps
/// mono_delegate_trampoline to one firing per delegate rather than one per call.
///
/// The field is a legacy entry for every delegate whatever engine runs its target:
/// mono_delegate_ctor fills it in with the delegate trampoline before anything else
/// sees the object, and the interpreter constructs its own delegates through that
/// same function. What stands behind it once the trampoline has fired is whatever
/// the runtime would hand out for the target anyway - which for an interpreted
/// target is the entry into the interpreter, reached through the stub like any
/// other.
static bool
dispatches_through_invoke_impl (MonoMethod *target)
{
	return m_class_get_parent (target->klass) == mono_defaults.multicastdelegate_class
	       && std::string_view (target->name) == "Invoke";
}

/// The implementation the runtime settled on for RECEIVER, a delegate, or TARGET's
/// vtable slot for as long as it has none.
///
/// That slot holds the same delegate trampoline that fills invoke_impl in, so a
/// delegate whose field is still unset dispatches correctly through it - which is what
/// makes the field safe to read without knowing whether anything has written it.
llvm::Value *
MethodLLVMEmitter::delegate_invoke_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                           MonoMethod *target)
{
	llvm::Value *impl = builder.CreateAlignedLoad (
		llvm::PointerType::get (context (), 0),
		builder.CreateGEP (
			builder.getInt8Ty (), receiver,
			builder.getInt32 (MONO_STRUCT_OFFSET (MonoDelegate, invoke_impl))),
		llvm::Align (TARGET_SIZEOF_VOID_P));

	return builder.CreateSelect (builder.CreateIsNull (impl),
	                             virtual_callee (builder, receiver, target), impl);
}

/// Emit something that reads VALUE, so that it is still live here.
///
/// An empty asm with a register constraint is the cheapest way to say it: it emits
/// nothing, and it leaves the value wherever the allocator can still reach it - a
/// callee-saved register or a spill slot, both of which the collector scans.
static void
keep_alive (llvm::IRBuilderBase &builder, llvm::Value *value)
{
	builder.CreateCall (
		llvm::InlineAsm::get (llvm::FunctionType::get (builder.getVoidTy (),
	                                                       { value->getType () }, false),
	                              "", "r", /*hasSideEffects=*/true),
		{ value });
}

/// The address the engine has to resolve for TARGET's own MonoMethod - the runtime's
/// description of the method, as opposed to its code.
llvm::Constant *
MethodLLVMEmitter::method_symbol (MonoMethod *target)
{
	char *name = mono_method_full_name (target, TRUE);
	std::string symbol = identity_symbol (std::string ("mono_method_") + name, target);

	g_free (name);
	record_external (symbol, ExternalSymbol::Kind::Method, target);
	return extern_symbol (symbol);
}

/// TARGET, or the wrapper that takes and releases its lock if TARGET is
/// [MethodImpl(Synchronized)].
///
/// A synchronized method's monitor is not in its body: the runtime builds a
/// wrapper that enters the monitor, calls the body and exits from a finally, and
/// the flagged method itself is only ever the body. Every reference this front
/// end resolves while it compiles - a direct callee, an escaping code address -
/// therefore has to name the wrapper, because nothing between here and the code
/// will substitute one later.
///
/// A dispatched site must not ask: what the receiver's vtable slot holds is
/// already the wrapper, put there by the runtime, and the IMT key has to stay the
/// method the caller named.
MonoMethod *
MethodLLVMEmitter::synchronized_target (MonoMethod *target)
{
	if (!(target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED))
		return target;

	/* The wrapper's own call to the body it locks, which would call itself. */
	if (method->wrapper_type == MONO_WRAPPER_SYNCHRONIZED)
		return target;

	return mono_marshal_get_synchronized_wrapper (target);
}

/// Whether VALUE is this method's own `this`, straight out of its slot. Used
/// to skip transparent-proxy checks: a body only ever executes on the real
/// object, so its own `this` can never be a proxy. A value that went through a
/// spill on its way here is missed, which costs a check, never correctness.
bool
MethodLLVMEmitter::is_own_this (llvm::Value *value)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (sig == nullptr || !sig->hasthis || args.empty ())
		return false;

	auto *load = llvm::dyn_cast<llvm::LoadInst> (value);
	return load != nullptr && load->getPointerOperand () == args[0].alloca;
}

/// The address of TARGET as something other than a direct call target: what
/// ldftn pushes, what a delegate stores. This is the `$legacy` symbol - the
/// legacy entry the runtime publishes - because the pointer escapes to callers
/// that know nothing of this backend's convention; an indirect call through it is
/// a legacy call.
///
/// Both branches below name that same symbol. A method whose code mini produces
/// is declared against it, since create_method_decl () gives the `$legacy`
/// suffix to exactly the methods that are entered in the legacy convention. Its
/// declaration is therefore the address, and asking for one here has to produce
/// it: a name belongs to one object, and LLVM answers a second claim on it by
/// silently renaming the newcomer, leaving a reference to a symbol the engine
/// never defines.
llvm::Expected<llvm::Constant *>
MethodLLVMEmitter::code_address_symbol (MonoMethod *target)
{
	if (implemented_outside_il (target))
		return create_method_decl (target);

	char *printed = mono_method_full_name (target, FALSE);
	std::string symbol = identity_symbol (printed, target) + "$legacy";

	g_free (printed);

	llvm::Constant *address = extern_symbol (symbol);

	record_external (symbol, ExternalSymbol::Kind::Code, target);
	mark_method_entry (llvm::cast<llvm::GlobalValue> (*address), target, mono::Entry::legacy);
	return address;
}

/*
 * III.2.4  tail. - (prefix) call terminates current method
 *
 *   Format   Assembly Format   Description
 *   FE 14    tail.             Subsequent call terminates current method.
 *
 *   The tail. instruction must immediately precede a call, calli, or callvirt
 *   instruction. It indicates that the current method's stack frame is no longer
 *   required and thus can be removed before the call instruction is executed. Because
 *   the value returned by the call will be the value returned by this method, the call
 *   can be converted into a cross-method jump.
 *
 *   The evaluation stack shall be empty except for the arguments being transferred by
 *   the following call. The instruction following the call instruction shall be a ret.
 *   Thus the only valid code sequence is tail. call (or calli or callvirt) ret.
 *   Correct CIL code shall not branch to the call instruction, but it is permitted to
 *   branch to the ret. The tail. instruction shall not be used to transfer control out
 *   of a try, filter, catch, or finally block.
 *
 *   The current frame cannot be discarded when control is transferred from untrusted
 *   code to trusted code, since this would jeopardize code identity security. Security
 *   checks can therefore cause the tail. to be ignored, leaving a standard call
 *   instruction.
 *
 *   Similarly, in order to allow the exit of a synchronized region to occur after the
 *   call returns, the tail. prefix is ignored when used to exit a method that is
 *   marked synchronized.
 */

/// Whether a value of TYPE certainly comes back in the return registers rather than
/// through a pointer the caller passes in.
///
/// A prototype that spells the hidden pointer out (hidden-return.hpp) returns void
/// and so answers yes, which is the point of spelling it: the forwarded pointer is
/// the caller's own and the jump is one the backend can always make.
///
/// What is left to be careful about is the aggregate that stayed an aggregate.
/// Whether such a return is demoted anyway is CanLowerReturn's answer, taken far
/// below the IR, and a musttail site cannot survive being wrong about it: the
/// backend aborts the process with "failed to perform tail call elimination on a
/// call site marked musttail" rather than break the guarantee. A plain tail site
/// can - it quietly becomes the ordinary call it would otherwise have been.
///
/// So this admits only what is already a single leaf, and leaves every aggregate to
/// the weaker marker. Being wrong in that direction costs a tail call the prefix
/// only ever permitted; being wrong in the other costs the process.
static bool
returns_in_registers (llvm::Type *type)
{
	if (type->isVoidTy () || type->isPointerTy ())
		return true;
	if (type->isIntegerTy ())
		return type->getIntegerBitWidth () <= 64;
	return type->isFloatTy () || type->isDoubleTy ();
}

/// Copy TARGET's return attributes onto CALL, which is about to be marked as a tail
/// call.
///
/// Tail-call eligibility compares the caller's return attributes against the call
/// site's own attribute list (attributesPermitTailCall), and that comparison has no
/// fallback to the called function - unlike the argument attributes, which do fall
/// back. So a caller returning `zeroext i8` whose site says plain `i8` reads as a
/// mismatched ABI. LLVM then drops the tail call silently rather than failing, and
/// the frame the prefix promised to hand away stays on the stack: a deep recursion
/// that should run in constant space overflows instead. The site describes the
/// callee, so saying what the callee actually returns is right either way; where
/// matching_call_abi has proved the two extensions agree, it is what makes the
/// instruction say so.
static void
carry_return_attributes (llvm::CallInst *call, llvm::Function *target)
{
	llvm::AttrBuilder ret_attrs (target->getContext (),
	                             target->getAttributes ().getRetAttrs ());

	call->addRetAttrs (ret_attrs);
}

/// How a tail.-prefixed call at this site may be marked: not at all, as a plain tail
/// call, or as a musttail one.
///
/// The two markers differ in what happens when the backend cannot form the jump.
/// musttail is a demand, and an unmet one aborts the process; tail is a permission,
/// and an unmet one is silently the ordinary call the site would have been. So the
/// tests below split in two. Everything up to the last pair is a question of whether
/// a jump is *correct* here at all, and answering no means leaving the site alone.
/// The last pair only decides which of the two markers a correct site may carry -
/// getting that wrong in the weaker direction costs a tail call the prefix never
/// obliged us to make, which is what declining would have cost anyway.
llvm::CallInst::TailCallKind
MethodLLVMEmitter::should_tail_call (MonoMethodSignature *callee_sig, MonoMethod *callee_method,
                                     llvm::FunctionType *callee_type,
                                     llvm::Type *callee_hidden)
{
	if (!prefixes.tail)
		return llvm::CallInst::TCK_None;

	/*
	 * A tail call keeps the caller's own prototype, so only a direct call to
	 * another method this backend compiles qualifies: an indirect target or a
	 * runtime-implemented one is a legacy call, lowered to a different prototype
	 * after the fact.
	 */
	if (callee_method == nullptr || implemented_outside_il (callee_method))
		return llvm::CallInst::TCK_None;

	/*
	 * A filter body is a function of its own over the parent's frame. Returning
	 * from it answers the filter, not the method, so there is no frame here to
	 * hand away.
	 */
	if (filter_mode)
		return llvm::CallInst::TCK_None;

	/* This frame owes an LMF pop on the way out, so it cannot be discarded. */
	if (method->save_lmf || lmf_slot != nullptr)
		return llvm::CallInst::TCK_None;

	/*
	 * A vararg call hands the callee a cookie buffer allocated in this frame,
	 * which the callee walks for the whole of its own execution.
	 */
	if (callee_sig->call_convention == MONO_CALL_VARARG)
		return llvm::CallInst::TCK_None;

	/*
	 * The ret the prefix promises has to follow at once so it can be folded into
	 * this instruction, must not be a branch target with an entry state of its own,
	 * and the arguments must be all the evaluation stack holds.
	 */
	const unsigned char *cursor = code + ip;

	if (ip >= code_size || mono_opcode_value (&cursor, code + code_size) != MONO_CEE_RET)
		return llvm::CallInst::TCK_None;
	if (blocks.find (ip) != blocks.end ())
		return llvm::CallInst::TCK_None;
	if (stack.size () != static_cast<size_t> (callee_sig->param_count) + callee_sig->hasthis)
		return llvm::CallInst::TCK_None;

	/*
	 * That ret is this method's own, so the two returns have to be the same LLVM
	 * type. An ordinary call would have widened its result onto the evaluation
	 * stack and let the ret narrow it back on the way out; folding the two together
	 * leaves nowhere for that to happen. Where the return travels through a hidden
	 * pointer the type is in the pointer's attribute rather than the prototype, and
	 * agreeing on it is what says the pointer this frame was entered with is one
	 * the callee may fill in.
	 */
	if (callee_type->getReturnType () != function->getReturnType ()
	    || callee_hidden != hidden_return_type (function))
		return llvm::CallInst::TCK_None;

	/*
	 * A protected call has to be an invoke, which cannot be a tail call - and
	 * III.2.4 forbids tail. inside a protected region anyway.
	 */
	if (innermost_try (offset) >= 0)
		return llvm::CallInst::TCK_None;

	/*
	 * Both markers carry the same promise: the callee touches nothing of this
	 * frame, which is what lets the frame go before the callee runs. So nothing
	 * that could point into it may travel in an argument - a value type's this,
	 * managed pointers, unmanaged pointers, function pointers. An indirect
	 * target's this is a pointer to nobody-knows-what, so it gets the same
	 * treatment a value type's would. Aggregates need no test: on this convention
	 * they pass as first-class values, and only the legacy ABI ever hands over a
	 * pointer to one.
	 */
	if (callee_sig->hasthis
	    && (callee_method == nullptr || m_class_is_valuetype (callee_method->klass)))
		return llvm::CallInst::TCK_None;

	for (int i = 0; i < callee_sig->param_count; ++i) {
		MonoType *param = callee_sig->params[i];

		if (param->byref || param->type == MONO_TYPE_PTR || param->type == MONO_TYPE_FNPTR)
			return llvm::CallInst::TCK_None;
	}

	/* The transition into native code saves state a tail call would skip. */
	if (callee_sig->pinvoke
	    || (callee_method != nullptr && (callee_method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)))
		return llvm::CallInst::TCK_None;

	/*
	 * A guarantee is only worth demanding where the backend can always keep it:
	 * where the jump changes nothing about the frame's argument area - identical
	 * prototypes, down to the extension attributes that say how narrow integers
	 * fill their registers - and where the return does not arrive through a pointer
	 * this frame invented, which switches tail-call elimination off outright.
	 *
	 * That set is the one III.2.4 makes mandatory, so demanding it is what turns
	 * the spec's obligation into something that fails loudly rather than quietly.
	 * Outside it the prefix is still worth marking: an argument area that has to be
	 * rebuilt is one the backend will often still rebuild in place, and where it
	 * will not, the site is exactly the ordinary call declining would have left.
	 */
	if (returns_in_registers (callee_type->getReturnType ())
	    && matching_call_abi (callee_sig, callee_type, callee_hidden))
		return llvm::CallInst::TCK_MustTail;

	return llvm::CallInst::TCK_Tail;
}

/// Whether a call to CALLEE_SIG could replace this method's own frame: the same
/// LLVM prototype, down to the extension attributes that say how narrow integers
/// fill their registers and the type a hidden return pointer points at - compared
/// positionally, because a this is just a leading pointer to the ABI.
bool
MethodLLVMEmitter::matching_call_abi (MonoMethodSignature *callee_sig,
                                      llvm::FunctionType *callee_type,
                                      llvm::Type *callee_hidden)
{
	if (function->getFunctionType () != callee_type
	    || hidden_return_type (function) != callee_hidden)
		return false;

	auto extensions = [] (MonoMethodSignature *s) {
		llvm::SmallVector<llvm::Attribute::AttrKind, 8> exts;

		if (s->hasthis)
			exts.push_back (llvm::Attribute::None);
		for (int i = 0; i < s->param_count; ++i)
			exts.push_back (integer_extension (s->params[i]));
		return exts;
	};

	MonoMethodSignature *caller_sig = mono_method_signature_internal (method);

	if (integer_extension (caller_sig->ret) != integer_extension (callee_sig->ret))
		return false;

	return extensions (caller_sig) == extensions (callee_sig);
}

/// The honored form of a tail. call: a marked call feeding a ret directly, which is
/// the shape LLVM turns into a jump. The IL ret that should_tail_call verified comes
/// next is consumed here, since this ret is its translation.
///
/// DECLARATION is the callee's own declaration, which is where the site's return
/// attributes come from even when the call goes through a pointer rather than to it.
/// DESCRIBE_SITE says the rest of what the site is; a dispatched call has to say the
/// same things here that it would on the ordinary path, since a jump that lost its
/// key would dispatch on nothing.
///
/// The marker is what the jump is made of, not a hint about one. The backend never
/// turns an *unmarked* call in tail position into a sibling call, at any
/// optimization level, so a site left plain is a site that keeps its frame. That is
/// why KIND is worth setting even when it is only the weaker of the two: the
/// alternative is not a jump that might happen anyway, it is no jump at all.
llvm::Error
MethodLLVMEmitter::emit_tail_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
                                   llvm::ArrayRef<llvm::Value *> args,
                                   llvm::CallInst::TailCallKind kind, size_t arg_slots,
                                   llvm::Function *declaration,
                                   llvm::function_ref<void (llvm::CallBase *)> describe_site)
{
	auto *target = llvm::dyn_cast<llvm::Function> (callee.getCallee ());
	llvm::Type *hidden = target != nullptr ? hidden_return_type (target) : nullptr;
	llvm::SmallVector<llvm::Value *, 8> operands (args.begin (), args.end ());

	/*
	 * The pointer this frame was entered with, not a slot of its own: it points
	 * into an ancestor frame, so it is still there once this one is gone. A local
	 * would be dead the moment the jump happened, and X86 refuses one.
	 * should_tail_call () has already agreed the two ends mean the same type by it.
	 */
	unsigned at = hidden_return_index (operands.size () + 1);

	if (hidden != nullptr)
		operands.insert (operands.begin () + at, hidden_return_pointer (function));

	llvm::CallInst *call = builder.CreateCall (callee, operands);

	if (hidden != nullptr)
		call->addParamAttrs (at, llvm::AttrBuilder (
					        context (),
					        hidden_return_attributes (context (), hidden)));

	carry_return_attributes (call, declaration);
	describe_site (call);
	call->setTailCallKind (kind);
	pop_stack (arg_slots);

	if (call->getType ()->isVoidTy ())
		builder.CreateRetVoid ();
	else
		builder.CreateRet (call);

	ip += 1;
	return llvm::Error::success ();
}

/*
 * III.3.37  jmp - jump to method
 *
 *   Format     Assembly Format   Description
 *   27 <T>     jmp method        Exit current method and jump to the specified method.
 *
 * Stack Transition:
 *
 *   ... -> ...
 *
 * Description:
 *
 *   Transfer control to the method specified by method, which is a metadata token
 *   (either a methodref or methoddef (See Partition II). The current arguments are
 *   transferred to the destination method.
 *
 *   The evaluation stack shall be empty when this instruction is executed. The
 *   calling convention, number and type of arguments at the destination address
 *   shall match that of the current method.
 *
 *   The jmp instruction cannot be used to transferred control out of a try, filter,
 *   catch, fault or finally block; or out of a synchronized region. If this is done,
 *   results are undefined. See Partition I.
 *
 * Exceptions:
 *
 *   None.
 *
 * Correctness:
 *
 *   Correct CIL code obeys the control flow restrictions specified above.
 *
 * Verifiability:
 *
 *   The jmp instruction is never verifiable.
 */
llvm::Error
MethodLLVMEmitter::emit_jmp (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	MonoMethod *callee_method = synchronized_target (*target);

	if (!stack.empty ())
		return unbalanced_stack (0);
	if (innermost_try (offset) >= 0)
		return invalid_il ("jmp cannot transfer control out of a protected block");
	/* A legacy target's call lowers to a different prototype than this method's. */
	if (implemented_outside_il (callee_method))
		return unsupported_il ("jmp to a runtime-implemented method");
	if (method->save_lmf)
		return unsupported_il ("jmp out of a frame that keeps an LMF");

	llvm::Expected<llvm::Function *> declaration = create_method_decl (callee_method);
	if (!declaration)
		return declaration.takeError ();

	MonoMethodSignature *sig = mono_method_signature_internal (callee_method);

	if (sig == nullptr)
		return invalid_il ("the jmp target has no signature");
	/* Neither end has an arglist to forward: the cookie is not in `args`. */
	if (sig->call_convention == MONO_CALL_VARARG
	    || mono_method_signature_internal (method)->call_convention == MONO_CALL_VARARG)
		return unsupported_il ("jmp across a vararg signature");
	llvm::Type *hidden = hidden_return_type (*declaration);

	if (!matching_call_abi (sig, (*declaration)->getFunctionType (), hidden))
		return invalid_il ("the jmp target's signature does not match this method's");

	/*
	 * The arguments transfer as they currently are - anything starg wrote goes
	 * with them - so they reload from their slots rather than from the incoming
	 * parameter values.
	 */
	std::vector<llvm::Value *> values;

	for (size_t i = 0; i < args.size (); ++i) {
		const Entry &argument = args[i];
		llvm::Expected<llvm::Type *> type = convert_type (argument.type);

		if (!type)
			return type.takeError ();
		values.push_back (builder.CreateAlignedLoad (*type, argument.alloca,
		                                             type_alignment (argument.type)));
	}

	/* The return goes where this method's own caller asked for it. */
	unsigned at = hidden_return_index (values.size () + 1);

	if (hidden != nullptr)
		values.insert (values.begin () + at, hidden_return_pointer (function));

	emit_profiler_frame_handover (builder, callee_method);

	llvm::CallInst *call = builder.CreateCall (*declaration, values);

	if (hidden != nullptr)
		call->addParamAttrs (at, llvm::AttrBuilder (
					        context (),
					        hidden_return_attributes (context (), hidden)));

	/*
	 * jmp releases this frame by definition, so the jump is the point rather than
	 * an optimization - but musttail is still only demandable where the backend can
	 * always keep it. matching_call_abi has settled the prototype above, so all that
	 * is left to ask is whether the return is an aggregate this convention still
	 * hands back by value. Where it is the site weakens to a plain tail call, which
	 * the backend jumps through where it can and quietly does not where it cannot.
	 */
	carry_return_attributes (call, *declaration);
	call->setTailCallKind (returns_in_registers ((*declaration)->getReturnType ())
	                               ? llvm::CallInst::TCK_MustTail
	                               : llvm::CallInst::TCK_Tail);

	if (call->getType ()->isVoidTy ())
		builder.CreateRetVoid ();
	else
		builder.CreateRet (call);

	return llvm::Error::success ();
}

/// Refuse a call the image should never have contained, the way mini refuses it:
/// not by failing the translation, but by compiling the method with a throw where
/// the call would have been, so that a body reached some other way still runs.
///
/// The call site is left holding a result nothing can read - control does not come
/// back from the throw - and the instructions after it are translated into a block
/// nothing branches to.
llvm::Error
MethodLLVMEmitter::emit_bad_image_call (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	size_t operands = sig->param_count + sig->hasthis;

	if (stack.size () < operands)
		return unbalanced_stack (operands);

	pop_stack (operands);
	emit_throw_corlib_exception (builder, "BadImageFormatException");
	builder.SetInsertPoint (llvm::BasicBlock::Create (context (), "bad_image", function));

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	MonoType *slot = stack_slot_type (sig->ret);

	if (held_in_memory (slot)) {
		llvm::Expected<llvm::Value *> home = vtype_slot (slot);

		if (!home)
			return home.takeError ();

		push_stack (*home, slot);
		return llvm::Error::success ();
	}

	llvm::Expected<llvm::Type *> type = convert_type (slot);

	if (!type)
		return type.takeError ();

	push_stack (llvm::PoisonValue::get (*type), slot);
	return llvm::Error::success ();
}

/// String.Length, read straight out of the object the way mini and the
/// interpreter both read it.
///
/// The accessor's body would be a field load either way, so this is worth
/// little on its own. What it is for is the debugger: a step into `s.Length`
/// that enters a one-line corlib property is a stop in code the user never
/// wrote, and one the other engines do not offer.
llvm::Error
MethodLLVMEmitter::emit_string_length (MonoIrBuilder &builder)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	StackValue receiver = get_stack (0);

	if (stack_type (receiver.type) != ObjectRef)
		return invalid_il (llvm::Twine ("a string was expected, not operand type ")
		                   + describe (receiver.type, stack_type (receiver.type)));

	emit_null_check (builder, receiver.value);

	llvm::Value *slot =
		builder.CreateGEP (builder.getInt8Ty (), receiver.value,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoString, length)));
	llvm::Value *length =
		builder.CreateAlignedLoad (builder.getInt32Ty (), slot, llvm::Align (4));

	pop_stack (1);
	push_stack (length, mono_get_int32_type ());
	return llvm::Error::success ();
}

/*
 * III.3.19  call - call a method
 *
 *   Format     Assembly Format   Description
 *   28 <T>     call method       Call method described by method.
 *
 * Stack Transition:
 *
 *   ..., arg0, arg1 ... argN -> ..., retVal (not always returned)
 *
 * Description:
 *
 *   The call instruction calls the method indicated by the descriptor method. method is
 *   a metadata token (a methodref, methoddef, or methodspec; see Partition II) that
 *   indicates the method to call, and the number, type, and order of the arguments that
 *   have been placed on the stack to be passed to that method, as well as the calling
 *   convention to be used. (See Partition I for a detailed description of the CIL
 *   calling sequence.) The call instruction can be immediately preceded by a tail.
 *   prefix to specify that the current method state should be released before
 *   transferring control (see §III.2.3).
 *
 *   The metadata token carries sufficient information to determine whether the call is
 *   to a static method, an instance method, a virtual method, or a global function. In
 *   all of these cases the destination address is determined entirely from the metadata
 *   token. (Contrast this with the callvirt instruction for calling virtual methods,
 *   where the destination address also depends upon the runtime type of the instance
 *   reference pushed before the callvirt.)
 *
 * Exceptions:
 *
 *   System.SecurityException is thrown if system security does not grant the caller
 *   access to the called method. The security check can occur when the CIL is converted
 *   to native code rather than at runtime.
 *
 *   System.MethodAccessException is thrown when there is an invalid attempt to access a
 *   non-public method.
 *
 *
 * III.4.2  callvirt - call a method associated, at runtime, with an object
 *
 *   Format     Assembly Format     Description
 *   6F <T>     callvirt method     Call a method associated with an object.
 *
 * Stack Transition:
 *
 *   ..., obj, arg1, ... argN -> ..., returnVal (not always returned)
 *
 * Description:
 *
 *   The callvirt instruction calls a late-bound method on an object. That is, the
 *   method is chosen based on the runtime type of obj rather than the compile-time
 *   class visible in the method pointer. callvirt can be used to call both virtual and
 *   instance methods.
 *
 *   The callvirt instruction can be immediately preceded by a tail. prefix to specify
 *   that the current method state should be released before transferring control.
 *
 * Exceptions:
 *
 *   System.NullReferenceException is thrown if obj is null.
 *
 *   System.MissingMethodException is thrown if a non-static method with the indicated
 *   name and signature could not be found in the class of obj or any of its
 *   superclasses. This is typically detected when CIL is converted to native code,
 *   rather than at runtime.
 */
llvm::Error
MethodLLVMEmitter::emit_call (MonoIrBuilder &builder, uint32_t token, bool is_virtual)
{
	llvm::Expected<MonoMethod *> target = resolve_method (token);
	if (!target)
		return target.takeError ();

	/*
	 * The subject is the callee the token names, taken before a constrained.
	 * prefix resolves it to an override below: naming a method you cannot see is
	 * the thing being refused, and which implementation the receiver would have
	 * dispatched to does not change that.
	 */
	if (checks_accessibility () && !mono_method_can_access_method (method, *target)) {
		if (llvm::Error error = emit_method_access_failure (builder, *target))
			return error;
	}

	MonoMethod *callee_method = *target;
	MonoClass *constrained = nullptr;
	bool direct_this = false;
	bool box_receiver = false;

	if (prefixes.constrained != 0) {
		/*
		 * The prefix is only defined ahead of callvirt (III.2.1). The one
		 * later use of constrained. call - static virtual interface members -
		 * cannot appear in metadata this runtime accepts.
		 */
		if (!is_virtual)
			return invalid_il ("constrained. on a plain call");

		llvm::Expected<MonoType *> ctype = element_type_from_token (prefixes.constrained);
		if (!ctype)
			return ctype.takeError ();
		constrained = mono_class_from_mono_type_internal (*ctype);

		/*
		 * A value type that implements the method takes the call directly, with
		 * the managed pointer as this. One that does not - the method lives on
		 * Object, ValueType or Enum - would have its receiver boxed first.
		 */
		if (m_class_is_valuetype (constrained)) {
			ERROR_DECL (resolve_error);

			/*
			 * mono_class_get_virtual_method () lays the vtable out and then
			 * indexes it without looking at whether that worked, so a class
			 * whose vtable cannot be built - one that leaves an inherited
			 * abstract method unimplemented, say - reaches vtable[slot] with
			 * vtable NULL, and reports nothing through the MonoError either.
			 * Build it here instead and surface the failure as the type load
			 * the program is owed.
			 */
			mono_class_setup_vtable (constrained);
			if (mono_class_has_failure (constrained)) {
				mono_error_set_for_class_failure (resolve_error, constrained);
				return runtime_error (resolve_error);
			}

			MonoMethod *impl = mono_class_get_virtual_method (
				constrained, callee_method, FALSE, resolve_error);

			if (!is_ok (resolve_error))
				return runtime_error (resolve_error);
			if (impl != nullptr && impl->klass == constrained) {
				callee_method = impl;
				direct_this = true;
			} else {
				/*
				 * The value type does not override the method - it lives
				 * on Object, ValueType or Enum - so the receiver is boxed
				 * and the call dispatches on the box.
				 */
				box_receiver = true;
			}
		}
	}

	MonoMethodSignature *sig = mono_method_signature_internal (callee_method);

	if (sig == nullptr)
		return invalid_il ("the called method has no signature");

	/*
	 * A vararg call site brings its own signature, and it is that one the
	 * arguments on the evaluation stack were pushed against: the callee's fixed
	 * parameters, a sentinel, then whatever types the caller chose. The
	 * declaration knows nothing of the variable part, so everything below that
	 * counts arguments has to work from this instead.
	 */
	bool vararg = sig->call_convention == MONO_CALL_VARARG;

	if (vararg) {
		llvm::Expected<MonoMethodSignature *> site =
			call_site_signature (callee_method, token);

		if (!site)
			return site.takeError ();
		sig = *site;
	}

	/*
	 * An abstract method has no body for a plain call to enter. A default interface
	 * method reaching another member of its own interface is the one place that
	 * spelling is legal - it dispatches on the receiver the way callvirt would -
	 * and anywhere else the image is bad.
	 */
	if (!is_virtual && (callee_method->flags & METHOD_ATTRIBUTE_ABSTRACT)) {
		if (!mono_class_is_interface (method->klass))
			return emit_bad_image_call (builder, sig);

		is_virtual = true;
	}

	if (is_virtual && !sig->hasthis)
		return invalid_il ("callvirt needs an instance method");
	if (is_virtual && sig->generic_param_count != 0 && !callee_method->is_inflated)
		return invalid_il ("callvirt on an open generic method");

	/* A creator hands back what it built rather than filling in a this. */
	if (callee_method->string_ctor)
		return emit_creator_call (builder, callee_method, sig);

	if (m_class_get_rank (callee_method->klass) > 0
	    && (callee_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
	    && (callee_method->iflags & METHOD_IMPL_ATTRIBUTE_NATIVE))
		return emit_array_accessor_call (builder, callee_method, sig);

	if (callee_method->klass == mono_defaults.array_class
	    && std::string_view (callee_method->name) == "UnsafeMov")
		return emit_unsafe_mov (builder, sig);

	if (callee_method->klass == mono_defaults.string_class
	    && std::string_view (callee_method->name) == "get_Length"
	    && sig->hasthis && sig->param_count == 0)
		return emit_string_length (builder);

	/*
	 * Debugger.Break () has an empty body and a comment where the code would be:
	 * the JIT is what gives the call its meaning, and the meaning is the one the
	 * break instruction has. An embedder can say no through mono_set_break_policy.
	 */
	if (is_debugger_break (callee_method, sig)) {
		if (!mini_should_insert_breakpoint (method))
			return llvm::Error::success ();
		return emit_user_break (builder);
	}

	/*
	 * The boxed receiver replaces the managed pointer in its stack slot, below the
	 * explicit arguments, before the arguments are collected.
	 */
	if (box_receiver) {
		size_t depth = sig->param_count;

		if (stack.size () < depth + 1)
			return unbalanced_stack (depth + 1);

		MonoType *vtype = m_class_get_byval_arg (constrained);
		StackValue &receiver = stack[stack.size () - 1 - depth];
		/* The receiver is the managed pointer the prefix promises, which is
		 * already what a value class is boxed from. */
		llvm::Value *value = receiver.value;

		if (!held_in_memory (vtype)) {
			llvm::Expected<llvm::Type *> slot = convert_type (vtype);
			if (!slot)
				return slot.takeError ();

			value = builder.CreateAlignedLoad (*slot, receiver.value,
			                                   type_alignment (vtype));
		}

		/* Boxing a nullable yields a boxed T, or null - the box opcode's rule
		 * holds here too, and it is what makes the receiver's type observable
		 * as T and a receiver without a value throw. */
		llvm::Expected<llvm::Value *> boxed =
			mono_class_is_nullable (constrained)
				? box_nullable (builder, constrained, { value, vtype })
				: box_value (builder, constrained, vtype, value);

		if (!boxed)
			return boxed.takeError ();

		receiver.value = *boxed;
		receiver.type = mono_get_object_type ();
	}

	/*
	 * Whether the callee is settled here rather than looked up off the receiver.
	 * Everything below that names a different method than the IL did depends on
	 * it: a site that still dispatches gets whatever the runtime put in the slot.
	 */
	bool devirtualized = !is_virtual
	                     || direct_this
	                     || !(callee_method->flags & METHOD_ATTRIBUTE_VIRTUAL)
	                     || (callee_method->flags & METHOD_ATTRIBUTE_FINAL);

#ifndef DISABLE_REMOTING
	/*
	 * A receiver of a MarshalByRefObject-derived class - or of Object itself,
	 * whose non-virtual methods a proxy also answers - may be a transparent
	 * proxy, and calling the body directly would run it locally, in the wrong
	 * domain or context. A direct call to such a method therefore goes through
	 * the with-check wrapper, which runs the body only after proving the
	 * receiver real, and remotes the call otherwise. Dispatched calls need
	 * none of this: a proxy's vtable already routes every slot to remoting.
	 *
	 * A receiver that is this method's own `this` is exempt - a body only ever
	 * executes on the real object. So are the remoting wrappers themselves:
	 * their calls are made after the check has already decided locality, and
	 * re-wrapping the with-check wrapper's own direct call would make the
	 * wrapper call itself.
	 */
	bool in_remoting_wrapper =
		method->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE
		|| method->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK
		|| method->wrapper_type == MONO_WRAPPER_XDOMAIN_INVOKE
		|| method->wrapper_type == MONO_WRAPPER_XDOMAIN_DISPATCH;

	if (devirtualized && sig->hasthis && !direct_this && !box_receiver
	    && !in_remoting_wrapper
	    && callee_method->wrapper_type == MONO_WRAPPER_NONE
	    && (mono_class_is_marshalbyref (callee_method->klass)
	        || callee_method->klass == mono_defaults.object_class)
	    && stack.size () > sig->param_count
	    && !is_own_this (stack[stack.size () - 1 - sig->param_count].value)) {
		/* The wrapper forwards a fixed argument list, which loses the arglist. */
		if (vararg)
			return unsupported_il ("a vararg call that may reach a proxy");

		ERROR_DECL (wrap_error);
		MonoMethod *checked =
			mono_marshal_get_remoting_invoke_with_check (callee_method, wrap_error);

		if (!is_ok (wrap_error))
			return runtime_error (wrap_error);
		callee_method = checked;
		sig = mono_method_signature_internal (callee_method);
	}
#endif

	/*
	 * Asked after the remoting check, so that a synchronized method on a
	 * remotable class locks inside the with-check wrapper rather than around it:
	 * the wrapper's own call to the body is where the lock belongs.
	 */
	if (devirtualized) {
		MonoMethod *locked = synchronized_target (callee_method);

		if (locked != callee_method) {
			/* The wrapper forwards a fixed argument list, losing the arglist. */
			if (vararg)
				return unsupported_il ("a vararg call to a synchronized method");

			callee_method = locked;
			sig = mono_method_signature_internal (callee_method);
		}
	}

	/*
	 * An internal call is published as the marshalling wrapper the runtime
	 * builds around it, so naming that wrapper here reaches the same code
	 * without crossing the legacy boundary to get there. A site that still
	 * dispatches has to keep the method the IL named: the slot index and the
	 * IMT key are computed from it.
	 */
	if (devirtualized && !vararg) {
		MonoMethod *wrapped = icall_wrapper_target (callee_method);

		if (wrapped != callee_method) {
			callee_method = wrapped;
			sig = mono_method_signature_internal (callee_method);
		}
	}

	llvm::Expected<llvm::Function *> declaration = create_method_decl (callee_method);
	if (!declaration)
		return declaration.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	/* A reference-typed constrained receiver arrives as a pointer to the reference. */
	if (constrained != nullptr && !direct_this && !box_receiver)
		(*args)[0] =
			builder.CreateAlignedLoad (llvm::PointerType::get (context (), 0),
		                                   (*args)[0], llvm::Align (TARGET_SIZEOF_VOID_P));

	/*
	 * The variable arguments leave the argument list for the cookie buffer, whose
	 * address takes their place as the one trailing parameter every vararg
	 * declaration carries.
	 */
	if (vararg) {
		llvm::Expected<llvm::Value *> cookie = build_sig_cookie (builder, sig, *args);

		if (!cookie)
			return cookie.takeError ();

		args->resize (vararg_fixed_params (sig) + sig->hasthis);
		args->push_back (*cookie);
	}

	llvm::FunctionCallee callee = *declaration;
	llvm::Type *hidden = hidden_return_type (*declaration);
	/*
	 * A slot always holds a legacy entry, and the legacy convention has a hidden
	 * return pointer of its own in a place the runtime's trampolines fixed. So a
	 * dispatched site is built in the signature's own terms and left to
	 * LegacyAbiPass, which is what puts the pointer where that convention wants it.
	 */
	llvm::FunctionType *slot_type =
		hidden != nullptr ? natural_prototype ((*declaration)->getFunctionType (), hidden)
	                          : (*declaration)->getFunctionType ();
	bool keyed = false;
	bool through_slot = false;
	/* The delegate an Invoke is dispatched out of; null for every other call. */
	llvm::Value *delegate = nullptr;

	if (is_virtual) {
		/*
		 * The receiver has to be there whether or not the callee is reached through
		 * it - an instance call on null throws before it dispatches.
		 */
		emit_null_check (builder, (*args)[0]);

		/*
		 * Only a method that can still be overridden has to be looked up. A final or
		 * non-virtual one is already the answer, and a callvirt on it is a null check
		 * with a direct call behind it - as is one a constrained. prefix already
		 * resolved to the value type's own implementation.
		 */
		bool overridable = !direct_this && (callee_method->flags & METHOD_ATTRIBUTE_VIRTUAL)
		                   && !(callee_method->flags & METHOD_ATTRIBUTE_FINAL);

		bool is_interface = mono_class_is_interface (callee_method->klass);
		bool generic_virtual = sig->generic_param_count != 0;

		if (dispatches_through_invoke_impl (callee_method)) {
			delegate = (*args)[0];
			callee = llvm::FunctionCallee (
				slot_type,
				delegate_invoke_callee (builder, delegate, callee_method));
			through_slot = true;
		} else if (overridable && (is_interface || generic_virtual)) {
			/*
			 * Several interface methods can hash to the same IMT slot, in which
			 * case what the slot holds is a thunk that picks the real target by
			 * looking at which method was asked for. The caller supplies that key
			 * in a register set aside for it, and the nest attribute is how
			 * unmodified LLVM is talked into filling it: nest pins an argument to
			 * %r10, which is exactly MONO_ARCH_IMT_REG on amd64. The key travels
			 * as one extra trailing argument that the target, once reached, never
			 * looks at - trailing because a hidden return pointer is only legal
			 * at parameter 0 or 1, and a leading key would push it past both.
			 *
			 * A virtual generic method dispatches the same way even off a class:
			 * its slot can never hold one instantiation's code, so what sits
			 * there is a trampoline that reads the asked-for inflated method out
			 * of that same register to pick the instantiation.
			 */
			keyed = true;

			llvm::Value *code =
				is_interface
					? interface_callee (builder, (*args)[0], callee_method)
					: virtual_callee (builder, (*args)[0], callee_method);
			std::vector<llvm::Type *> params (slot_type->param_begin (),
			                                  slot_type->param_end ());

			params.push_back (llvm::PointerType::get (context (), 0));
			callee = llvm::FunctionCallee (
				llvm::FunctionType::get (slot_type->getReturnType (), params,
			                                 slot_type->isVarArg ()),
				code);
			args->push_back (method_symbol (callee_method));
			through_slot = true;
		} else if (overridable && mono_method_get_vtable_index (callee_method) >= 0) {
			callee = llvm::FunctionCallee (
				slot_type,
				virtual_callee (builder, (*args)[0], callee_method));
			through_slot = true;
		}
	}

	/* What the site says about its callee, whether or not it becomes a jump. */
	auto describe_site = [&] (llvm::CallBase *site) {
		if (keyed)
			site->addParamAttr (site->arg_size () - 1, llvm::Attribute::Nest);

		/*
		 * A dispatched call goes through whatever pointer the runtime left for
		 * it, in a vtable slot or in the delegate, and that is always a legacy
		 * entry - those are shared with every caller that is not generated code.
		 */
		if (through_slot)
			mark_legacy_entry_call (site, callee_method, sig);
	};

	llvm::CallInst::TailCallKind tail_kind = should_tail_call (
		sig, callee_method, callee.getFunctionType (), through_slot ? nullptr : hidden);

	/*
	 * A dispatched site can hand its frame over like any other - the key rides a
	 * register of its own that a jump leaves alone, and a legacy entry is reached
	 * by a jump as readily as by a call - but it can never demand one. Its
	 * arguments are still to be lowered into the legacy convention, and a site
	 * whose argument area that rebuilds is not one the backend can jump through;
	 * musttail across the two conventions is not even well-formed IR. So the
	 * marker stays the permission, which LegacyAbiPass drops again where the
	 * lowering turns out to need this frame.
	 */
	if (through_slot && tail_kind == llvm::CallInst::TCK_MustTail)
		tail_kind = llvm::CallInst::TCK_Tail;

	/*
	 * An instrumented method has to report its exit in front of a site the frame
	 * does not come back from, so it can only honour the marker that is a
	 * guarantee. Under the weaker one the backend is free to leave an ordinary
	 * call, and the report would then land before the callee's own entry instead
	 * of after its exit.
	 */
	if (tail_kind == llvm::CallInst::TCK_Tail
	    && (instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE)
	        || instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL)))
		tail_kind = llvm::CallInst::TCK_None;

	if (tail_kind != llvm::CallInst::TCK_None) {
		emit_profiler_frame_handover (builder, callee_method);
		return emit_tail_call (builder, callee, *args, tail_kind,
		                       sig->param_count + sig->hasthis, *declaration,
		                       describe_site);
	}

	llvm::Value *result = emit_protected_call (builder, callee, *args, describe_site);

	/*
	 * invoke_impl usually holds an arch stub that puts delegate->target in the
	 * receiver's place on its way through, so this activation stops rooting the
	 * delegate the moment the call is entered. Were that the last reference to a
	 * delegate over a dynamic method, the code running underneath could be
	 * collected. Holding it past the call is what keeps it there.
	 */
	if (delegate != nullptr)
		keep_alive (builder, delegate);

	pop_stack (sig->param_count + sig->hasthis);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	return push_produced (builder, result, sig->ret);
}

} // namespace mono
