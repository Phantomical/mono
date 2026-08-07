#include "method-to-llvm.hpp"
#include "jit.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/opcodes.h"
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MathExtras.h>

namespace mono {

/*
 * III.2.1  constrained. - (prefix) invoke a member on a value of a variable type
 *
 *   Format        Assembly Format         Description
 *   FE 16 <T>     constrained. thisType   Call a virtual method on a type constrained
 *                                         to be type T
 *
 * Stack Transition:
 *
 *   ..., ptr, arg1, ... argN -> ..., ptr, arg1, ... argN
 *
 * Description:
 *
 *   The constrained. prefix is permitted only on a callvirt instruction. The type of
 *   ptr must be a managed pointer (&) to thisType. The constrained prefix is designed
 *   to allow callvirt instructions to be made in a uniform way independent of whether
 *   thisType is a value type or a reference type.
 *
 *   When callvirt method instruction has been prefixed by constrained thisType the
 *   instruction is executed as follows.
 *
 *   If thisType is a reference type (as opposed to a value type) then ptr is
 *   dereferenced and passed as the 'this' pointer to the callvirt of method
 *
 *   If thisType is a value type and thisType implements method then ptr is passed
 *   unmodified as the 'this' pointer to a call of method implemented by thisType
 *
 *   If thisType is a value type and thisType does not implement method then ptr is
 *   dereferenced, boxed, and passed as the 'this' pointer to the callvirt of method
 *
 *   This last case can only occur when method was defined on System.Object,
 *   System.ValueType, or System.Enum and not overridden by thisType. In this last
 *   case, the boxing causes a copy of the original object to be made, however since
 *   all methods on System.Object, System.ValueType, and System.Enum do not modify
 *   the state of the object, this fact cannot be detected.
 *
 *
 * III.2.2  no. - (prefix) possibly skip a fault check
 *
 *   Format                   Assembly Format                Description
 *   FE 19 <unsigned int8>    no. { typecheck | rangecheck   The specified fault
 *                            | nullcheck }                  check(s) normally
 *                                                           performed as part of the
 *                                                           execution of the
 *                                                           subsequent instruction
 *                                                           can/shall be skipped.
 *
 * Description:
 *
 *   This prefix indicates that the subsequent instruction need not perform the
 *   specified fault check when it is executed. The byte that follows the instruction
 *   code indicates which checks can optionally be skipped. This instruction is not
 *   verifiable.
 *
 *
 * III.2.3  readonly. (prefix) - following instruction returns a controlled-mutability
 * managed pointer
 *
 *   Format     Assembly Format   Description
 *   FE 1E      readonly.         Specify that the subsequent array address operation
 *                                performs no type check at runtime, and that it
 *                                returns a controlled-mutability managed pointer
 *
 * Description:
 *
 *   This prefix can only appear only immediately preceding the ldelema instruction
 *   and calls to the special Address method on arrays. Its effect on the subsequent
 *   operation is twofold.
 *
 *   1. At run-time, no type check operation is performed. (For the value class case
 *      there is never a runtime time check so this is a noop in that case).
 *
 *   2. The verifier treats the result of the address-of operation as a
 *      controlled-mutability managed pointer (§III.1.8.1.2.2).
 *
 *
 * III.2.4  tail. (prefix) - call terminates current method
 *
 *   Format     Assembly Format   Description
 *   FE 14      tail.             Subsequent call terminates current method
 *
 * Description:
 *
 *   The tail. prefix shall immediately precede a call, calli, or callvirt
 *   instruction. It indicates that the current method's stack frame is no longer
 *   required and thus can be removed before the call instruction is executed. Because
 *   the value returned by the call will be the value returned by this method, the
 *   call can be converted into a cross-method jump.
 *
 *   There can also be implementation-specific restrictions that prevent the tail.
 *   prefix from being obeyed in certain cases. While an implementation is free to
 *   ignore the tail. prefix under these circumstances, they should be clearly
 *   documented as they can affect the behavior of programs.
 *
 *   CLI implementations are required to honor tail. call requests where caller and
 *   callee methods can be statically determined to lie in the same assembly; and
 *   where the caller is not in a synchronized region; and where caller and callee
 *   satisfy all conditions listed in the "Verifiability" rules below.
 *
 *
 * III.2.5  unaligned. (prefix) - pointer instruction might be unaligned
 *
 *   Format                   Assembly Format        Description
 *   FE 12 <unsigned int8>    unaligned. alignment   Subsequent pointer instruction
 *                                                   might be unaligned.
 *
 * Stack Transition:
 *
 *   ..., addr -> ..., addr
 *
 * Description:
 *
 *   The unaligned. prefix specifies that addr (an unmanaged pointer (&), or native
 *   int) on the stack might not be aligned to the natural size of the immediately
 *   following ldind, stind, ldfld, stfld, ldobj, stobj, initblk, or cpblk
 *   instruction. That is, for a ldind.i4 instruction the alignment of addr might not
 *   be to a 4-byte boundary.
 *
 *   The value of alignment shall be 1, 2, or 4 and means that the generated code
 *   should assume that addr is byte, double-byte, or quad-byte-aligned, respectively.
 *
 *   The unaligned. and volatile. prefixes can be combined in either order. They
 *   shall immediately precede a ldind, stind, ldfld, stfld, ldobj, stobj, initblk,
 *   or cpblk instruction.
 *
 *
 * III.2.6  volatile. (prefix) - pointer reference is volatile
 *
 *   Format     Assembly Format   Description
 *   FE 13      volatile.         Subsequent pointer reference is volatile.
 *
 * Stack Transition:
 *
 *   ..., addr -> ..., addr
 *
 * Description:
 *
 *   The volatile. prefix specifies that addr is a volatile address (i.e., it can be
 *   referenced externally to the current thread of execution) and the results of
 *   reading that location cannot be cached or that multiple stores to that location
 *   cannot be suppressed. Marking an access as volatile. affects only that single
 *   access; other accesses to the same location shall be marked separately. Access
 *   to volatile locations need not be performed atomically. (See Partition I,
 *   "Memory Model and Optimizations")
 *
 *   The unaligned. and volatile. prefixes can be combined in either order. They
 *   shall immediately precede a ldind, stind, ldfld, stfld, ldobj, stobj, initblk,
 *   or cpblk instruction. Only the volatile. prefix is allowed with the ldsfld and
 *   stsfld instructions.
 */
llvm::Error
MethodLLVMEmitter::emit_prefix (int opcode, uint64_t operand)
{
	switch (opcode) {
	case MONO_CEE_VOLATILE_:
		prefixes.volatile_ = true;
		return llvm::Error::success ();
	case MONO_CEE_UNALIGNED_: {
		uint8_t alignment = static_cast<uint8_t> (operand);

		if (alignment != 1 && alignment != 2 && alignment != 4)
			return invalid_il ("unaligned. alignment shall be 1, 2, or 4");
		prefixes.unaligned = alignment;
		return llvm::Error::success ();
	}
	case MONO_CEE_CONSTRAINED_:
		prefixes.constrained = static_cast<uint32_t> (operand);
		return llvm::Error::success ();
	case MONO_CEE_TAIL_:
		/*
		 * A request an implementation may decline: the call site checks whether
		 * it can be honored and falls back to an ordinary call when not.
		 */
		prefixes.tail = true;
		return llvm::Error::success ();
	case MONO_CEE_READONLY_:
		prefixes.readonly_ = true;
		return llvm::Error::success ();
	case MONO_CEE_NO_:
		/* Permission to skip fault checks, never an obligation. */
		return llvm::Error::success ();
	default:
		llvm::report_fatal_error ("emit_prefix: not a prefix opcode");
	}
}

/// The alignment for the coming memory access: the location's own, unless an
/// unaligned. prefix promised less.
llvm::Align
MethodLLVMEmitter::access_alignment (MonoType *location)
{
	return prefixes.unaligned != 0 ? llvm::Align (prefixes.unaligned)
	                               : type_alignment (location);
}

/// Whether one access of TYPE at ALIGN can be an LLVM atomic instruction, so that
/// the ordering rides the access itself rather than a fence beside it.
///
/// LLVM takes an atomic load or store of a scalar - integer, pointer or floating
/// point - whose size is a whole power-of-two number of bytes, and lowers anything
/// the machine cannot do in one instruction into a call into the atomic runtime
/// library that nothing here defines. So the access also has to fit the target's
/// atomic width and be aligned to at least its own size.
bool
MethodLLVMEmitter::can_access_atomically (llvm::Type *type, llvm::Align align)
{
	if (!type->isIntegerTy () && !type->isPointerTy () && !type->isFloatingPointTy ())
		return false;

	llvm::TypeSize size = module->getDataLayout ().getTypeSizeInBits (type);

	if (size.isScalable ())
		return false;

	uint64_t bits = size.getFixedValue ();

	return bits >= 8 && llvm::isPowerOf2_64 (bits)
	       && bits <= host_max_atomic_bits (*function) && bits <= align.value () * 8;
}

/// One load from ADDRESS as TYPE, honoring the volatile. and unaligned. prefixes on
/// the instruction being emitted.
llvm::Value *
MethodLLVMEmitter::emit_memory_load (MonoIrBuilder &builder, llvm::Type *type, llvm::Value *address,
                                     MonoType *location)
{
	llvm::Align align = access_alignment (location);
	llvm::LoadInst *value = builder.CreateAlignedLoad (type, address, align);

	/*
	 * A volatile read has acquire semantics (I.12.6.7). Acquire on the load
	 * itself orders only that access, which is all III.2.6 asks for; a fence
	 * would be a barrier at this point in the program and order everything
	 * around it too. The load stays volatile either way - I.12.6.7 forbids
	 * removing or coalescing a volatile operation, and atomic does not promise
	 * that on its own.
	 */
	if (prefixes.volatile_) {
		value->setVolatile (true);
		if (can_access_atomically (type, align))
			value->setAtomic (llvm::AtomicOrdering::Acquire);
		else
			builder.CreateFence (llvm::AtomicOrdering::Acquire);
	}

	return value;
}

/// One store of VALUE to ADDRESS, through the write barrier when LOCATION holds a
/// reference, honoring the volatile. and unaligned. prefixes.
///
/// VALUE is what coerce_to_location () produced, so for a value class it is the
/// address of the bytes to copy rather than the bytes themselves.
void
MethodLLVMEmitter::emit_memory_store (MonoIrBuilder &builder, llvm::Value *value,
                                      llvm::Value *address, MonoType *location)
{
	llvm::Align align = access_alignment (location);
	/*
	 * Only a plain scalar store can carry the release itself. A reference goes
	 * through the write barrier, which does the store inside the call, and a
	 * value class is a copy rather than one instruction - neither is an access
	 * an ordering can be attached to, so those order with a fence.
	 */
	bool atomic = prefixes.volatile_ && !mini_type_is_reference (location)
	              && !held_in_memory (location)
	              && can_access_atomically (value->getType (), align);

	/* A volatile write has release semantics (I.12.6.7). */
	if (prefixes.volatile_ && !atomic)
		builder.CreateFence (llvm::AtomicOrdering::Release);

	/*
	 * A reference going into memory the collector may be tracking goes through it,
	 * and the address here could point anywhere - the generic barrier is the one
	 * that tolerates that.
	 */
	if (mini_type_is_reference (location)) {
		builder.CreateCall (wbarrier_decl (), {address, value});
		return;
	}

	if (held_in_memory (location)) {
		MonoClass *klass =
			mono_class_from_mono_type_internal (mini_get_underlying_type (location));

		/*
		 * A struct with references inside cannot just be moved: the collector
		 * has to mark the cards its reference fields land on, which only its
		 * own copy routine knows how to do.
		 */
		if (m_class_has_references (klass))
			builder.CreateCall (value_copy_decl (),
			                    {address, value, builder.getInt32 (1),
			                     class_symbol (klass, "mono_class_")});
		else
			builder.CreateMemCpy (address, align, value,
			                      type_alignment (location),
			                      vtype_size (location, /*native=*/false),
			                      prefixes.volatile_);
		return;
	}

	llvm::StoreInst *store = builder.CreateAlignedStore (value, address, align);

	if (prefixes.volatile_) {
		store->setVolatile (true);
		if (atomic)
			store->setAtomic (llvm::AtomicOrdering::Release);
	}
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::vtype_slot (MonoType *t, bool native)
{
	llvm::Expected<llvm::Type *> type = convert_type (t, native);

	if (!type)
		return type.takeError ();

	MonoIrBuilder entry (entry_block, entry_block->begin ());
	llvm::AllocaInst *slot = entry.CreateAlloca (*type, nullptr, "vt");

	slot->setAlignment (type_alignment (t, native));
	return slot;
}

void
MethodLLVMEmitter::copy_vtype (MonoIrBuilder &builder, llvm::Value *destination,
                               llvm::Value *source, MonoType *t, bool native)
{
	llvm::Align align = type_alignment (t, native);

	builder.CreateMemCpy (destination, align, source, align, vtype_size (t, native));
}

llvm::Error
MethodLLVMEmitter::push_from_location (MonoIrBuilder &builder, llvm::Value *address,
                                       MonoType *t, bool native)
{
	if (!held_in_memory (t)) {
		llvm::Expected<llvm::Type *> type = convert_type (t, native);

		if (!type)
			return type.takeError ();

		llvm::Value *value = emit_memory_load (builder, *type, address, t);

		push_stack (widen_to_stack (builder, value, t), stack_slot_type (t), native);
		return llvm::Error::success ();
	}

	llvm::Expected<llvm::Value *> slot = vtype_slot (t, native);

	if (!slot)
		return slot.takeError ();

	llvm::Align source = prefixes.unaligned != 0 ? llvm::Align (prefixes.unaligned)
	                                             : type_alignment (t, native);

	builder.CreateMemCpy (*slot, type_alignment (t, native), address, source,
	                      vtype_size (t, native), prefixes.volatile_);
	/*
	 * A volatile read has acquire semantics (I.12.6.7). A copy is not one access
	 * an ordering can be attached to, so this one is a fence. I.12.6.7 does not
	 * promise a value class is read atomically anyway.
	 */
	if (prefixes.volatile_)
		builder.CreateFence (llvm::AtomicOrdering::Acquire);

	push_stack (*slot, stack_slot_type (t), native);
	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::push_produced (MonoIrBuilder &builder, llvm::Value *value, MonoType *t,
                                  bool native)
{
	if (!held_in_memory (t)) {
		push_stack (widen_to_stack (builder, value, t), stack_slot_type (t), native);
		return llvm::Error::success ();
	}

	llvm::Expected<llvm::Value *> slot = vtype_slot (t, native);

	if (!slot)
		return slot.takeError ();

	builder.CreateAlignedStore (value, *slot, type_alignment (t, native));
	push_stack (*slot, stack_slot_type (t), native);
	return llvm::Error::success ();
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::materialize (MonoIrBuilder &builder, llvm::Value *value, MonoType *t,
                                bool native)
{
	if (!held_in_memory (t))
		return value;

	llvm::Expected<llvm::Type *> type = convert_type (t, native);

	if (!type)
		return type.takeError ();

	return builder.CreateAlignedLoad (*type, value, type_alignment (t, native));
}

} // namespace mono
