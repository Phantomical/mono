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

/// Whether target is corlib's System.Diagnostics.Debugger::Break ().
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

/// Resolves token to the method it names, against this method's generic context.
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

/// Returns the signature a call to target uses at this call site.
///
/// This differs from the declaration only at a vararg call site, where it also
/// names the types the caller chose for the variable arguments.
llvm::Expected<MonoMethodSignature *>
MethodLLVMEmitter::call_site_signature (MonoMethod *target, uint32_t token)
{
	ERROR_DECL (metadata_error);
	MonoMethodSignature *sig = mono_method_get_signature_checked (
		target, m_class_get_image (method->klass), token,
		mono_method_get_context (method), metadata_error);

	if (sig == nullptr)
		return runtime_error (metadata_error);

	return sig;
}

/*
 * How a vararg call passes its variable arguments.
 *
 * A vararg signature converts to a function type holding the fixed parameters
 * and one trailing pointer. The variable arguments are not in it. They travel
 * in a buffer in the caller's frame, and that trailing pointer is the address
 * of the buffer:
 *
 *     +0                MonoMethodSignature *   the call-site signature
 *     +sizeof (void *)  the first variable argument
 *     ...
 *
 * Each variable argument follows at the running sum of mono_type_stack_size ()
 * over the ones before it. System.ArgIterator is what reads that. Setup starts
 * its walk at the second word, and IntGetNextArg advances by that same stack
 * size without realigning. The sizes are not uniform - a float takes four
 * bytes, not a whole slot - so an offset that disagrees does not fault. The
 * next argument reads as garbage.
 *
 * The callee reaches the buffer through arglist, which wraps the address in a
 * RuntimeArgumentHandle. The buffer stays in the caller's frame for the whole
 * call.
 */

/// Builds the buffer a vararg call passes its variable arguments in.
///
/// \param args  the call's arguments in order, the this included.
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
		// The stride is the stack size the iterator advances by, but the
		// buffer must still be big enough for what gets written into the
		// last slot.
		size = std::max (size, cursor + layout.getTypeStoreSize (stored));
		cursor += static_cast<uint64_t> (mono_type_stack_size (sig->params[i], nullptr));
		size = std::max (size, cursor);
	}

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *buffer = entry.CreateAlloca (
		llvm::ArrayType::get (builder.getInt8Ty (), size), nullptr, "arglist");

	buffer->setAlignment (llvm::Align (TARGET_SIZEOF_VOID_P));

	// ArgIterator names the variable part by index into this signature, so it
	// must be the call-site one. The declaration knows only the fixed part.
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

/// Coerces value to something that can go where a call signature asks for
/// destination.
///
/// A call argument accepts a mismatch that coerce_to_location () refuses for a
/// store: an int32, or a pointer, where the parameter is int64. This backend
/// widens it only for a call, and only on 64-bit targets. The value then rides
/// in the full register, so a constant arrives sign-extended. The eval stack's
/// int32 is signed, so this reading sign-extends it.
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
	// An int32 meeting a pointer-typed parameter widens the same way before the cast.
	if (from->isIntegerTy () && (*type)->isPointerTy ()
	    && from->getIntegerBitWidth () < TARGET_SIZEOF_VOID_P * 8)
		value.value = builder.CreateSExt (value.value,
		                                  builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

	llvm::Expected<llvm::Value *> coerced =
		coerce_to_location (builder, value, destination, native);

	if (!coerced)
		return coerced.takeError ();

	// Every signature this backend converts takes a value class by value.
	return materialize (builder, *coerced, destination, native);
}

/// Coerces value to the receiver of an instance call.
///
/// A `this` is a pointer in every signature this backend converts. The eval stack
/// can still hand one over as a native int. Pointer arithmetic and `ldind.i` both
/// produce one, and so does the marshalling wrappers' own `ldarg`, `ldind.i`,
/// `call instance` sequence. All three leave a number where the callee declared an
/// address.
llvm::Value *
MethodLLVMEmitter::coerce_to_receiver (MonoIrBuilder &builder, llvm::Value *value)
{
	if (!value->getType ()->isIntegerTy ())
		return value;

	if (value->getType ()->getIntegerBitWidth () < TARGET_SIZEOF_VOID_P * 8)
		value = builder.CreateSExt (value, builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8));

	return builder.CreateIntToPtr (value, llvm::PointerType::get (context (), 0));
}

/// Takes a call's arguments off the evaluation stack, converted to what the signature
/// asks for.
///
/// The receiver of an instance method is argument zero, and it is not in the parameter
/// list. It converts against the pointer every instance signature declares, not against
/// a MonoType.
llvm::Expected<std::vector<llvm::Value *>>
MethodLLVMEmitter::pop_call_arguments (MonoIrBuilder &builder, MonoMethodSignature *sig,
                                       bool native)
{
	size_t count = sig->param_count + sig->hasthis;

	if (stack.size () < count)
		return unbalanced_stack (count);

	std::vector<llvm::Value *> args (count);

	// The last parameter is on top, so the stack unwinds into the list backwards.
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

/// Loads the pointer stored offset bytes into the vtable of the object receiver
/// points at.
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

/// Loads the callee out of target's vtable slot in the object receiver points at.
///
/// A virtual call reads the callee out of the receiver instead of knowing it in
/// advance. It indexes the object's vtable pointer by the slot the method was
/// assigned when its class was laid out.
llvm::Value *
MethodLLVMEmitter::virtual_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                   MonoMethod *target)
{
	return vtable_entry (builder, receiver,
	                     MONO_STRUCT_OFFSET (MonoVTable, vtable)
	                             + mono_method_get_vtable_index (target)
	                                       * TARGET_SIZEOF_VOID_P);
}

/// Loads the callee out of target's IMT slot in the object receiver points at.
///
/// An interface method has no fixed vtable slot: where an implementation lands depends
/// on the class that implements it. Dispatch goes instead through the interface method
/// table, a small hash table the runtime lays out in the words immediately before each
/// MonoVTable. Its slots sit at negative offsets from that same base.
llvm::Value *
MethodLLVMEmitter::interface_callee (MonoIrBuilder &builder, llvm::Value *receiver,
                                     MonoMethod *target)
{
	int32_t slot = static_cast<int32_t> (mono_method_get_imt_slot (target)) - MONO_IMT_SIZE;

	return vtable_entry (builder, receiver, slot * TARGET_SIZEOF_VOID_P);
}

/// Whether a call to target must dispatch through the delegate it is made on,
/// instead of through a vtable slot.
///
/// A delegate's Invoke has no body for anyone to compile. The runtime picks an
/// implementation for each delegate object instead of a vtable slot. A single-target
/// delegate gets an arch stub that shuffles the receiver and jumps through
/// method_ptr. A multicast or open-instance delegate gets the compiled
/// delegate-invoke wrapper instead, because method_ptr cannot express that shape.
/// The runtime stores the choice in the object's invoke_impl field. Reading that
/// field is what limits mono_delegate_trampoline to one firing per delegate, not
/// one per call.
///
/// Every delegate carries this field, whatever engine runs its target.
/// mono_delegate_ctor () fills it in with the delegate trampoline before anything
/// else can see the object. The interpreter builds its own delegates through that
/// same function. Once the trampoline has fired, invoke_impl holds whatever the
/// runtime hands out for the target. For an interpreted target, that is the entry
/// into the interpreter, reached through the stub like any other.
static bool
dispatches_through_invoke_impl (MonoMethod *target)
{
	return m_class_get_parent (target->klass) == mono_defaults.multicastdelegate_class
	       && std::string_view (target->name) == "Invoke";
}

/// Loads the implementation the runtime settled on for receiver, a delegate, or
/// target's vtable slot until it has one.
///
/// That slot holds the same delegate trampoline that fills invoke_impl in. A
/// delegate whose field is still unset therefore dispatches correctly through it.
/// That is what makes the field safe to read without knowing whether anything has
/// written it.
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

/// Emits something that reads value, so it stays live here.
///
/// An empty asm with a register constraint is the cheapest way to do this. It
/// emits no code, and it leaves the value wherever the allocator can still reach
/// it: a callee-saved register or a spill slot, both of which the collector scans.
static void
keep_alive (llvm::IRBuilderBase &builder, llvm::Value *value)
{
	builder.CreateCall (
		llvm::InlineAsm::get (llvm::FunctionType::get (builder.getVoidTy (),
	                                                       { value->getType () }, false),
	                              "", "r", /*hasSideEffects=*/true),
		{ value });
}

/// Names the address the engine must resolve for target's own MonoMethod, the
/// runtime's description of the method rather than its code.
llvm::Constant *
MethodLLVMEmitter::method_symbol (MonoMethod *target)
{
	/*
	 * A shared body's own MonoMethod is the shared one, and naming that is
	 * right: it is the method that is running. Any other open method stands for
	 * a different MonoMethod in each instantiation, so a site that has not been
	 * given method_operand () refuses here.
	 */
	if (target != method && depends_on_context (target))
		cannot_share ("a reference to an open method");

	char *name = mono_method_full_name (target, TRUE);
	std::string symbol = identity_symbol (std::string ("mono_method_") + name, target);

	g_free (name);
	record_external (symbol, ExternalSymbol::Kind::Method, target);
	return extern_symbol (symbol);
}

/// Returns target, or the wrapper that takes and releases its lock when target
/// carries [MethodImpl(Synchronized)].
///
/// A synchronized method's monitor is not in its body. The runtime builds a
/// wrapper that enters the monitor, calls the body, and exits through a finally.
/// The flagged method itself is always the body. Every reference this front end
/// resolves while it compiles must name the wrapper. That includes a direct
/// callee and an escaping code address. Nothing between here and the code will
/// substitute the wrapper later.
///
/// A dispatched call site must not ask this. The receiver's vtable slot already
/// holds the wrapper, put there by the runtime, and the IMT key must stay the
/// method the caller named.
MonoMethod *
MethodLLVMEmitter::synchronized_target (MonoMethod *target)
{
	if (!(target->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED))
		return target;

	// This is the wrapper's own call to the body it locks. Without this
	// exemption, wrapping target again here makes the wrapper call itself.
	if (method->wrapper_type == MONO_WRAPPER_SYNCHRONIZED)
		return target;

	return mono_marshal_get_synchronized_wrapper (target);
}

/// Whether value is this method's own `this`, read straight out of its slot.
///
/// A caller uses this to skip a transparent-proxy check. A body only ever runs
/// on the real object, so its own `this` can never be a proxy. Missing a value
/// that passed through a spill on its way here only costs an extra check, never
/// correctness.
bool
MethodLLVMEmitter::is_own_this (llvm::Value *value)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);

	if (sig == nullptr || !sig->hasthis || args.empty ())
		return false;

	auto *load = llvm::dyn_cast<llvm::LoadInst> (value);
	return load != nullptr && load->getPointerOperand () == args[0].alloca;
}

/// Names target's entry as a value: an address to push or to store, rather than
/// a call target in this module.
///
/// The address is the method's published entry, which is the one every caller
/// reaches. A pointer taken here stays correct when a later compile replaces
/// the body.
llvm::Expected<llvm::Constant *>
MethodLLVMEmitter::code_address_symbol (MonoMethod *target)
{
	if (implemented_outside_il (target))
		return create_method_decl (target);

	char *printed = mono_method_full_name (target, FALSE);
	// This needs a placeholder name of its own, not the one create_method_decl ()
	// uses. bind_symbols () renames both to the method's stub symbol, but it
	// leaves a definition alone. So a method taking its own address would find
	// its own body under that other name and push that, not the stub in front of
	// it. Every later compile supersedes that body, and a delegate built over it
	// stays on the tier it was made in.
	std::string symbol = identity_symbol (printed, target) + "$stub";

	g_free (printed);

	llvm::Constant *address = extern_symbol (symbol);

	record_external (symbol, ExternalSymbol::Kind::Code, target);
	mark_method_reference (llvm::cast<llvm::GlobalValue> (*address), target);
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

/// Whether a value of type certainly comes back in the return registers, rather
/// than through a pointer the caller passes in.
///
/// A prototype that spells the hidden pointer out (hidden-return.hpp) returns
/// void, so this returns true. That is the point of spelling it out: the
/// forwarded pointer is the caller's own, and the jump is one the backend can
/// always make.
///
/// What still needs care is the aggregate that stayed an aggregate. Whether
/// such a return gets demoted anyway is CanLowerReturn's answer, taken far
/// below the IR, and a musttail site cannot survive being wrong about it. The
/// backend then aborts the process with "failed to perform tail call
/// elimination on a call site marked musttail", instead of breaking the
/// guarantee. A plain tail site can be wrong about it: the backend then
/// quietly compiles it as the ordinary call the site was always allowed to be.
///
/// So this admits only what is already a single leaf, and leaves every
/// aggregate to the weaker marker. Being wrong in that direction only costs a
/// tail call the prefix ever permitted. Being wrong in the other direction
/// costs the process.
static bool
returns_in_registers (llvm::Type *type)
{
	if (type->isVoidTy () || type->isPointerTy ())
		return true;
	if (type->isIntegerTy ())
		return type->getIntegerBitWidth () <= 64;
	return type->isFloatTy () || type->isDoubleTy ();
}

/// Copies target's return attributes onto call, which is about to be marked as a
/// tail call.
///
/// Tail-call eligibility compares the caller's return attributes against the call
/// site's own attribute list (attributesPermitTailCall). That comparison has no
/// fallback to the called function, unlike the argument attributes, which do fall
/// back. So a caller returning `zeroext i8` whose site says plain `i8` reads as a
/// mismatched ABI. LLVM then drops the tail call silently instead of failing, and
/// the frame the prefix promised to hand away stays on the stack. A deep recursion
/// that must run in constant space overflows instead. The site describes the
/// callee, so saying what the callee returns is right either way. Where
/// matching_call_abi has proved the two extensions agree, this is what makes the
/// instruction say so.
static void
carry_return_attributes (llvm::CallInst *call, llvm::Function *target)
{
	llvm::AttrBuilder ret_attrs (target->getContext (),
	                             target->getAttributes ().getRetAttrs ());

	call->addRetAttrs (ret_attrs);
}

/// How a tail.-prefixed call at this site can be marked: not at all, as a plain
/// tail call, or as a musttail one.
///
/// The two markers differ in what happens when the backend cannot form the jump.
/// musttail is a demand, and an unmet one aborts the process. tail is a
/// permission, and an unmet one is silently the ordinary call the site
/// otherwise compiles as. So the tests below split in two. Everything up to the
/// last pair asks whether a jump is *correct* here at all, and a no means
/// leaving the site alone. The last pair only decides which of the two markers
/// a correct site can carry. Getting that wrong in the weaker direction only
/// costs a tail call the prefix never obliged us to make, the same cost as
/// declining outright.
llvm::CallInst::TailCallKind
MethodLLVMEmitter::should_tail_call (MonoMethodSignature *callee_sig, MonoMethod *callee_method,
                                     llvm::FunctionType *callee_type,
                                     llvm::Type *callee_hidden)
{
	if (!prefixes.tail)
		return llvm::CallInst::TCK_None;

	// A tail call keeps the caller's own prototype, so only a direct call to
	// another method this backend compiles qualifies. An indirect target or a
	// runtime-implemented one crosses into C, and is lowered to a different
	// prototype after the fact.
	if (callee_method == nullptr || implemented_outside_il (callee_method))
		return llvm::CallInst::TCK_None;

	// A filter body is a function of its own over the parent's frame. Returning
	// from it answers the filter, not the method, so there is no frame here to
	// hand away.
	if (filter_mode)
		return llvm::CallInst::TCK_None;

	// This frame owes an LMF pop on the way out, so it cannot be discarded.
	if (method->save_lmf || lmf_slot != nullptr)
		return llvm::CallInst::TCK_None;

	// The cookie buffer a vararg call passes sits in this frame, and the callee
	// reads it for the whole call.
	if (callee_sig->call_convention == MONO_CALL_VARARG)
		return llvm::CallInst::TCK_None;

	// The ret the prefix promises must follow at once, so it can be folded into
	// this instruction. It must not be a branch target with an entry state of
	// its own, and the arguments must be all the evaluation stack holds.
	const unsigned char *cursor = code + ip;

	if (ip >= code_size || mono_opcode_value (&cursor, code + code_size) != MONO_CEE_RET)
		return llvm::CallInst::TCK_None;
	if (blocks.find (ip) != blocks.end ())
		return llvm::CallInst::TCK_None;
	if (stack.size () != static_cast<size_t> (callee_sig->param_count) + callee_sig->hasthis)
		return llvm::CallInst::TCK_None;

	// That ret is this method's own, so the two returns must be the same LLVM
	// type. An ordinary call widens its result onto the evaluation stack and lets
	// the ret narrow it back on the way out. Folding the two together leaves
	// nowhere for that to happen. Where the return travels through a hidden
	// pointer, the type sits in the pointer's attribute rather than in the
	// prototype. Agreeing on it says the pointer this frame was entered with is
	// one the callee can fill in.
	if (callee_type->getReturnType () != function->getReturnType ()
	    || callee_hidden != hidden_return_type (function))
		return llvm::CallInst::TCK_None;

	// A protected call must be an invoke, which cannot be a tail call, and
	// III.2.4 forbids tail. inside a protected region anyway.
	if (innermost_try (offset) >= 0)
		return llvm::CallInst::TCK_None;

	// Both markers carry the same promise: the callee touches nothing of this
	// frame, which is what lets the frame go before the callee runs. So no
	// argument can carry anything that can point into it: a value type's this,
	// managed pointers, unmanaged pointers, function pointers. An indirect
	// target's this is a pointer to nobody-knows-what, so it gets the same
	// treatment as a value type's this. Aggregates need no test: on this
	// convention they pass as first-class values, and only the C lowering ever
	// hands over a pointer to one.
	if (callee_sig->hasthis
	    && (callee_method == nullptr || m_class_is_valuetype (callee_method->klass)))
		return llvm::CallInst::TCK_None;

	for (int i = 0; i < callee_sig->param_count; ++i) {
		MonoType *param = callee_sig->params[i];

		if (param->byref || param->type == MONO_TYPE_PTR || param->type == MONO_TYPE_FNPTR)
			return llvm::CallInst::TCK_None;
	}

	// The transition into native code saves state that a tail call skips.
	if (callee_sig->pinvoke
	    || (callee_method != nullptr && (callee_method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)))
		return llvm::CallInst::TCK_None;

	// A guarantee is only worth demanding where the backend can always keep it.
	// That means the jump changes nothing about the frame's argument area:
	// identical prototypes, down to the extension attributes that say how narrow
	// integers fill their registers. It also means the return does not arrive
	// through a pointer this frame invented, which switches tail-call
	// elimination off outright.
	//
	// That set is the one III.2.4 makes mandatory, so demanding it turns the
	// spec's obligation into something that fails loudly instead of quietly.
	// Outside that set, the prefix is still worth marking. An argument area that
	// must be rebuilt is often one the backend rebuilds in place anyway. Where it
	// does not, the site becomes the ordinary call that declining also leaves behind.
	if (returns_in_registers (callee_type->getReturnType ())
	    && matching_call_abi (callee_sig, callee_type, callee_hidden))
		return llvm::CallInst::TCK_MustTail;

	return llvm::CallInst::TCK_Tail;
}

/// Whether a call to callee_sig can replace this method's own frame.
/// The two must share the same LLVM prototype. That includes matching
/// extension attributes for narrow integers, and the same type for a hidden
/// return pointer. The comparison is positional, because a this is a leading
/// pointer to the ABI.
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

/// Emits the honored form of a tail. call: a marked call feeding a ret directly,
/// which is the shape LLVM turns into a jump. The IL ret that should_tail_call
/// verified comes next is consumed here, since this ret is its translation.
///
/// \param declaration    the callee's own declaration, which is where the site's
///                       return attributes come from even when the call goes
///                       through a pointer rather than to it.
/// \param describe_site  says the rest of what the site is. A dispatched call
///                       must say the same things here that it says on the
///                       ordinary path, because a jump that lost its key
///                       dispatches on nothing.
///
/// The marker is what the jump is made of, not a hint about one. The backend
/// never turns an *unmarked* call in tail position into a sibling call, at any
/// optimization level, so a site left plain keeps its frame. That is why kind
/// is worth setting even when it is only the weaker of the two markers: the
/// alternative is not a jump that can happen anyway. It is no jump at all.
llvm::Error
MethodLLVMEmitter::emit_tail_call (MonoIrBuilder &builder, llvm::FunctionCallee callee,
                                   llvm::ArrayRef<llvm::Value *> args,
                                   llvm::CallInst::TailCallKind kind, size_t arg_slots,
                                   llvm::Function *declaration,
                                   llvm::function_ref<void (llvm::CallBase *)> describe_site,
                                   bool natural)
{
	auto *target = llvm::dyn_cast<llvm::Function> (callee.getCallee ());

	if (target == nullptr && natural)
		target = declaration;

	llvm::Type *hidden = target != nullptr ? hidden_return_type (target) : nullptr;
	llvm::SmallVector<llvm::Value *, 8> operands (args.begin (), args.end ());

	// The pointer this frame was entered with, not a slot of its own: it points
	// into an ancestor frame, so it is still there once this one is gone. A
	// local dies the moment the jump happens, and X86 refuses one.
	// should_tail_call () has already confirmed the two ends agree on its type.
	unsigned at = hidden != nullptr ? hidden_return_index (placed_parameter_count (target)) : 0;

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
	// A call into C lowers to a different prototype than this method's.
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
	// Neither end has an arglist to forward: the cookie is not in `args`.
	if (sig->call_convention == MONO_CALL_VARARG
	    || mono_method_signature_internal (method)->call_convention == MONO_CALL_VARARG)
		return unsupported_il ("jmp across a vararg signature");
	llvm::Type *hidden = hidden_return_type (*declaration);

	if (!matching_call_abi (sig, (*declaration)->getFunctionType (), hidden))
		return invalid_il ("the jmp target's signature does not match this method's");

	// The arguments transfer as they currently are, so anything starg wrote goes
	// with them. They reload from their slots, not from the incoming parameter
	// values.
	std::vector<llvm::Value *> values;

	for (size_t i = 0; i < args.size (); ++i) {
		const Entry &argument = args[i];
		llvm::Expected<llvm::Type *> type = convert_type (argument.type);

		if (!type)
			return type.takeError ();
		values.push_back (builder.CreateAlignedLoad (*type, argument.alloca,
		                                             type_alignment (argument.type)));
	}

	// The return goes where this method's own caller asked for it.
	unsigned at = hidden_return_index (values.size () + 1);

	if (hidden != nullptr)
		values.insert (values.begin () + at, hidden_return_pointer (function));

	emit_profiler_frame_handover (builder, callee_method);

	llvm::CallInst *call = builder.CreateCall (*declaration, values);

	if (hidden != nullptr)
		call->addParamAttrs (at, llvm::AttrBuilder (
					        context (),
					        hidden_return_attributes (context (), hidden)));

	// jmp releases this frame by definition, so the jump is the point, not an
	// optimization. musttail is still only demandable where the backend can
	// always keep it, though. matching_call_abi has settled the prototype
	// above, so the one thing left to ask is whether the return is an
	// aggregate this convention still hands back by value. Where it is, the
	// site weakens to a plain tail call, which the backend jumps through where
	// it can, and otherwise quietly does not.
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

/// Refuses a call the image must never have contained. It does not fail the
/// translation. Instead it compiles the method with a throw in the call's place,
/// so a body reached some other way still runs.
///
/// The call site is left holding a result nothing can read, since control does
/// not come back from the throw, and the instructions after it are translated
/// into a block nothing branches to.
llvm::Error
MethodLLVMEmitter::emit_bad_image_call (MonoIrBuilder &builder, MonoMethodSignature *sig)
{
	size_t operands = sig->param_count + sig->hasthis;

	if (stack.size () < operands)
		return unbalanced_stack (operands);

	pop_stack (operands);
	emit_throw_corlib_exception (builder, "BadImageFormatException");
	builder.SetInsertPoint (create_cold_block ("bad_image"));

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

/// Reads String.Length straight out of the object, the same way the
/// interpreter reads it.
///
/// The accessor's body is a field load either way, so this saves little on its
/// own. It exists for the debugger: a step into `s.Length` that enters a
/// one-line corlib property is a stop in code the user never wrote, one the
/// interpreter does not have to make either.
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

/// Reads the System.Type an object's vtable carries, which is what
/// object.GetType () answers.
///
/// `receiver_by_reference` says the receiver on the stack is a managed pointer
/// to the reference rather than the reference, which is the shape a
/// `constrained.` prefix on a reference type leaves.
llvm::Error
MethodLLVMEmitter::emit_get_type (MonoIrBuilder &builder, bool receiver_by_reference)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Align align (TARGET_SIZEOF_VOID_P);
	StackValue receiver = get_stack (0);
	llvm::Value *object = receiver.value;

	if (receiver_by_reference)
		object = builder.CreateAlignedLoad (ptr, object, align);
	else if (stack_type (receiver.type) != ObjectRef)
		return invalid_il (llvm::Twine ("an object was expected, not operand type ")
		                   + describe (receiver.type, stack_type (receiver.type)));

	emit_null_check (builder, object);

	llvm::Value *vtable = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), object,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoObject, vtable))),
		align);
	/*
	 * mono_class_create_runtime_vtable () fills in `type` before it publishes
	 * the vtable, so an object that exists has one. RuntimeType is the
	 * exception: its own vtable takes `type` after the memory barrier, which
	 * leaves a window where the field is null. The interpreter reads the field
	 * unguarded and this follows it. A guard costs every site, and the window
	 * it covers is one managed code cannot run in.
	 *
	 * A transparent proxy answers with the type it stands for. Its vtable is a
	 * copy of the real class's, and mono_class_proxy_vtable () overwrites
	 * `type` with the interface for an interface proxy.
	 */
	llvm::Value *type = builder.CreateAlignedLoad (
		ptr,
		builder.CreateGEP (builder.getInt8Ty (), vtable,
	                           builder.getInt32 (MONO_STRUCT_OFFSET (MonoVTable, type))),
		align, "obj_type");

	pop_stack (1);
	push_stack (type, m_class_get_byval_arg (mono_defaults.systemtype_class));
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

	// The subject is the callee the token names, taken before a constrained.
	// prefix resolves it to an override below. Naming a method you cannot see
	// is the thing being refused, and it makes no difference which
	// implementation the receiver dispatches to.
	if (checks_accessibility () && !mono_method_can_access_method (method, *target)) {
		if (llvm::Error error = emit_method_access_failure (builder, *target))
			return error;
	}

	MonoMethod *callee_method = *target;
	MonoClass *constrained = nullptr;
	bool direct_this = false;
	bool box_receiver = false;

	if (prefixes.constrained != 0) {
		// The prefix is only defined ahead of callvirt (III.2.1). Its one other
		// use, ahead of a plain call to a static virtual interface member,
		// cannot appear in metadata this runtime accepts.
		if (!is_virtual)
			return invalid_il ("constrained. on a plain call");

		llvm::Expected<MonoType *> ctype = element_type_from_token (prefixes.constrained);
		if (!ctype)
			return ctype.takeError ();
		constrained = mono_class_from_mono_type_internal (*ctype);

		// A value type that implements the method takes the call directly, with
		// the managed pointer as this. One that does not implement it, because
		// the method lives on Object, ValueType, or Enum, gets its receiver
		// boxed first.
		if (m_class_is_valuetype (constrained)) {
			ERROR_DECL (resolve_error);

			// mono_class_get_virtual_method () lays the vtable out and then
			// indexes it without checking whether that worked. A class whose
			// vtable cannot be built, one that leaves an inherited abstract
			// method unimplemented, for example, reaches vtable[slot] with
			// vtable NULL, and reports nothing through the MonoError either.
			// Build it here instead, and surface the failure as the type load
			// the program is owed.
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
				// The value type does not override the method, because it
				// lives on Object, ValueType, or Enum. So the receiver is
				// boxed, and the call dispatches on the box.
				box_receiver = true;
			}
		}
	}

	MonoMethodSignature *sig = mono_method_signature_internal (callee_method);

	if (sig == nullptr)
		return invalid_il ("the called method has no signature");

	// A vararg call site brings its own signature. The arguments on the
	// evaluation stack were pushed against that signature: the callee's fixed
	// parameters, a sentinel, then whatever types the caller chose. The
	// declaration knows nothing of the variable part, so everything below that
	// counts arguments must work from this signature instead.
	bool vararg = sig->call_convention == MONO_CALL_VARARG;

	if (vararg) {
		llvm::Expected<MonoMethodSignature *> site =
			call_site_signature (callee_method, token);

		if (!site)
			return site.takeError ();
		sig = *site;
	}

	// An abstract method has no body for a plain call to enter. A default
	// interface method reaching another member of its own interface is the one
	// place that spelling is legal: it dispatches on the receiver the way
	// callvirt does. Anywhere else, the image is bad.
	if (!is_virtual && (callee_method->flags & METHOD_ATTRIBUTE_ABSTRACT)) {
		if (!mono_class_is_interface (method->klass))
			return emit_bad_image_call (builder, sig);

		is_virtual = true;
	}

	// A method with no this has no receiver to dispatch on, so the site can only
	// be an ordinary call. The stack transition above says the same: without a
	// this there is no obj under the arguments. Code generators emit this shape,
	// and Harmony and MonoMod write patch bodies with it, so a refusal rejects
	// assemblies mini runs.
	if (is_virtual && !sig->hasthis)
		is_virtual = false;

	if (is_virtual && sig->generic_param_count != 0 && !callee_method->is_inflated)
		return invalid_il ("callvirt on an open generic method");

	// A string constructor returns what it built rather than filling in a this.
	if (callee_method->string_ctor)
		return emit_string_constructor_call (builder, callee_method, sig);

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

	// Debugger.Break () has an empty body and a comment where the code goes: the
	// JIT gives the call its meaning, and that meaning is the one the break
	// instruction has. An embedder can say no through mono_set_break_policy.
	if (is_debugger_break (callee_method, sig)) {
		if (!mini_should_insert_breakpoint (method))
			return llvm::Error::success ();
		return emit_user_break (builder);
	}

	// The boxed receiver replaces the managed pointer in its stack slot, below
	// the explicit arguments, before the arguments are collected.
	if (box_receiver) {
		size_t depth = sig->param_count;

		if (stack.size () < depth + 1)
			return unbalanced_stack (depth + 1);

		MonoType *vtype = m_class_get_byval_arg (constrained);
		StackValue &receiver = stack[stack.size () - 1 - depth];
		// The receiver is the managed pointer the prefix promises, which is
		// already what a value class is boxed from.
		llvm::Value *value = receiver.value;

		if (!held_in_memory (vtype)) {
			llvm::Expected<llvm::Type *> slot = convert_type (vtype);
			if (!slot)
				return slot.takeError ();

			value = builder.CreateAlignedLoad (*slot, receiver.value,
			                                   type_alignment (vtype));
		}

		// Boxing a nullable yields a boxed T, or null: the box opcode's rule
		// holds here too. It is what makes the receiver's type observable as
		// T, and a receiver without a value throw.
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
	 * object.GetType () is an internal call, so it is published as a
	 * managed-to-native wrapper and reached through a remoting with-check
	 * wrapper. The receiver's vtable carries the answer, so the site becomes
	 * one load.
	 *
	 * Asked after the constrained. prefix settles the receiver, so the value
	 * under it is an object whichever spelling the site used.
	 *
	 * Reflection on a proxy gives GetType () a meaning of its own, and the
	 * runtime-invoke wrapper is how it gets there. The interpreter refuses the
	 * same site (mono/interp/transform/intrinsics.cpp).
	 */
	if (callee_method->klass == mono_defaults.object_class
	    && std::string_view (callee_method->name) == "GetType"
	    && sig->hasthis && sig->param_count == 0
#ifndef DISABLE_REMOTING
	    && method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE
#endif
	)
		return emit_get_type (builder, constrained != nullptr && !box_receiver);

	// Whether the callee is settled here rather than looked up off the
	// receiver. Everything below that names a different method than the IL did
	// depends on it: a site that still dispatches gets whatever the runtime
	// put in the slot.
	bool devirtualized = !is_virtual
	                     || direct_this
	                     || !(callee_method->flags & METHOD_ATTRIBUTE_VIRTUAL)
	                     || (callee_method->flags & METHOD_ATTRIBUTE_FINAL);

#ifndef DISABLE_REMOTING
	// A receiver of a MarshalByRefObject-derived class, or of Object itself,
	// whose non-virtual methods a proxy also answers, can be a transparent
	// proxy. Calling the body directly then runs it locally, in the wrong
	// domain or context. A direct call to such a method therefore goes through
	// the with-check wrapper, which runs the body only after proving the
	// receiver real, and remotes the call otherwise. Dispatched calls need
	// none of this: a proxy's vtable already routes every slot to remoting.
	//
	// A receiver that is this method's own `this` is exempt: a body only ever
	// runs on the real object. So are the remoting wrappers themselves. Their
	// calls happen after the check has already decided locality, and
	// re-wrapping the with-check wrapper's own direct call makes the wrapper
	// call itself.
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
		// The wrapper forwards a fixed argument list, which loses the arglist.
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

	// Asked after the remoting check, so a synchronized method on a
	// remotable class locks inside the with-check wrapper, not around it. The
	// wrapper's own call to the body is where the lock belongs.
	if (devirtualized) {
		MonoMethod *locked = synchronized_target (callee_method);

		if (locked != callee_method) {
			// The wrapper forwards a fixed argument list, losing the arglist.
			if (vararg)
				return unsupported_il ("a vararg call to a synchronized method");

			callee_method = locked;
			sig = mono_method_signature_internal (callee_method);
		}
	}

	// An internal call is published as the marshalling wrapper the runtime
	// builds around it, so naming that wrapper here reaches the same code
	// without crossing into C to get there. A site that still
	// dispatches must keep the method the IL named: the slot index and the
	// IMT key are computed from it.
	if (devirtualized && !vararg) {
		MonoMethod *wrapped = icall_wrapper_target (callee_method);

		if (wrapped != callee_method) {
			callee_method = wrapped;
			sig = mono_method_signature_internal (callee_method);
		}
	}

	bool callee_by_context = calls_through_context (callee_method);
	llvm::Expected<llvm::Function *> declaration =
		create_method_decl (callee_method, callee_by_context);
	if (!declaration)
		return declaration.takeError ();

	llvm::Expected<std::vector<llvm::Value *>> args = pop_call_arguments (builder, sig);
	if (!args)
		return args.takeError ();

	// A reference-typed constrained receiver arrives as a pointer to the reference.
	if (constrained != nullptr && !direct_this && !box_receiver)
		(*args)[0] =
			builder.CreateAlignedLoad (llvm::PointerType::get (context (), 0),
		                                   (*args)[0], llvm::Align (TARGET_SIZEOF_VOID_P));

	// The variable arguments leave the argument list for the cookie buffer.
	if (vararg) {
		llvm::Expected<llvm::Value *> cookie = build_sig_cookie (builder, sig, *args);

		if (!cookie)
			return cookie.takeError ();

		args->resize (vararg_fixed_params (sig) + sig->hasthis);
		args->push_back (*cookie);
	}

	llvm::FunctionCallee callee = *declaration;
	llvm::Type *hidden = hidden_return_type (*declaration);
	// A slot holds the method's stub, which is entered exactly as the
	// declaration says, hidden return pointer and all. Nothing lowers the
	// site afterwards, so the prototype it is built against is the
	// declaration's own.
	llvm::FunctionType *slot_type = (*declaration)->getFunctionType ();
	bool keyed = false;
	bool through_slot = false;
	// The delegate an Invoke is dispatched out of. Null for every other call.
	llvm::Value *delegate = nullptr;

	if (is_virtual) {
		// The receiver must be there whether or not the callee is reached
		// through it: an instance call on null throws before it dispatches.
		emit_null_check (builder, (*args)[0]);

		// Only a method that can still be overridden needs a lookup. A final
		// or non-virtual one is already the answer, and a callvirt on it is a
		// null check with a direct call behind it, as is one a constrained.
		// prefix already resolved to the value type's own implementation.
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
			// Several interface methods can hash to the same IMT slot. When
			// that happens, the slot holds a thunk that picks the real target
			// by looking at which method was asked for. The caller supplies
			// that key in a register set aside for it, and the nest attribute
			// is how unmodified LLVM fills it: nest pins an argument to %r10,
			// exactly MONO_ARCH_IMT_REG on amd64. The key travels as one extra
			// trailing argument that the target, once reached, never reads.
			// It trails because a hidden return pointer is only legal at
			// parameter 0 or 1, and a leading key pushes it past both.
			//
			// A virtual generic method dispatches the same way even off a
			// class: its slot can never hold one instantiation's code. What
			// sits there instead is a trampoline that reads the asked-for
			// inflated method out of that same register to pick the
			// instantiation.
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
			llvm::Expected<llvm::Value *> key =
				method_operand (builder, callee_method);

			if (!key)
				return key.takeError ();

			args->push_back (*key);
			through_slot = true;
		} else if (overridable && mono_method_get_vtable_index (callee_method) >= 0) {
			callee = llvm::FunctionCallee (
				slot_type,
				virtual_callee (builder, (*args)[0], callee_method));
			through_slot = true;
		}
	}

	/*
	 * A dispatched site has already found the instantiation's own code, through
	 * the receiver. A direct one has not: the callee this body named is open,
	 * and each instantiation compiles it for itself, so the entry to call comes
	 * out of the context like any other piece of metadata.
	 */
	if (!through_slot && pass_context_to (*declaration, *args))
		keyed = true;

	bool fetched = !through_slot && callee_by_context;

	if (fetched) {
		llvm::Expected<llvm::Value *> code = rgctx_fetch (
			builder, MONO_RGCTX_INFO_GENERIC_METHOD_CODE, callee_method);

		if (!code)
			return code.takeError ();

		callee = llvm::FunctionCallee (callee.getFunctionType (), *code);
	}

	// What the site says about its callee, whether or not it becomes a jump.
	auto describe_site = [&] (llvm::CallBase *site) {
		if (keyed)
			site->addParamAttr (site->arg_size () - 1, llvm::Attribute::Nest);
	};

	llvm::CallInst::TailCallKind tail_kind =
		should_tail_call (sig, callee_method, callee.getFunctionType (), hidden);

	// A dispatched site can hand its frame over like any other. The key rides
	// a register of its own, which a jump leaves alone. A stub is reached
	// by a jump as readily as by a call. But it never demands one. A keyed
	// site cannot demand one: the key is an argument this frame's own
	// prototype does not have, and musttail insists the two match. A plain
	// vtable site has no such key, so demanding one is possible in principle,
	// but this code downgrades it the same way. Widening that needs its own
	// evidence, since a musttail the backend cannot form aborts the process
	// instead of declining.
	//
	// A callee read out of the context is downgraded for a second reason: the
	// read is a call of its own, so the site no longer sits where a guaranteed
	// jump could be formed.
	if ((through_slot || fetched) && tail_kind == llvm::CallInst::TCK_MustTail)
		tail_kind = llvm::CallInst::TCK_Tail;

	// An instrumented method must report its exit in front of a site the
	// frame does not come back from, so it can only honour the marker that is
	// a guarantee. Under the weaker one, the backend is free to leave an
	// ordinary call, and the report then lands before the callee's own entry
	// instead of after its exit.
	if (tail_kind == llvm::CallInst::TCK_Tail
	    && (instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE)
	        || instrumented (MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL)))
		tail_kind = llvm::CallInst::TCK_None;

	if (tail_kind != llvm::CallInst::TCK_None) {
		emit_profiler_frame_handover (builder, callee_method);
		return emit_tail_call (builder, callee, *args, tail_kind,
		                       sig->param_count + sig->hasthis, *declaration,
		                       describe_site, /*natural=*/true);
	}

	llvm::Value *result = emit_protected_call (
		builder, callee, *args, describe_site, hidden,
		hidden_return_index (placed_parameter_count ((*declaration))));

	// invoke_impl usually holds an arch stub that puts delegate->target in the
	// receiver's place on its way through, so this activation stops rooting
	// the delegate the moment the call is entered. If that is the last
	// reference to a delegate over a dynamic method, the collector can
	// reclaim the code running underneath. Holding it past the call is what
	// keeps it there.
	if (delegate != nullptr)
		keep_alive (builder, delegate);

	pop_stack (sig->param_count + sig->hasthis);

	if (sig->ret->type == MONO_TYPE_VOID && !sig->ret->byref)
		return llvm::Error::success ();

	return push_produced (builder, result, sig->ret);
}

} // namespace mono
