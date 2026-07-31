#include "method-to-llvm.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/opcodes.h"
#include <llvm/IR/Instructions.h>
#include <llvm/Support/ErrorHandling.h>

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
		 * A request an implementation may decline outside the same-assembly
		 * cases it must honor; declining everywhere is a legal starting point.
		 */
		return llvm::Error::success ();
	case MONO_CEE_READONLY_:
		/*
		 * Skips a covariance check ldelema does not emit and loosens the
		 * verifier's view of the result; neither changes what is generated.
		 */
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

/// One load from ADDRESS as TYPE, honoring the volatile. and unaligned. prefixes on
/// the instruction being emitted.
llvm::Value *
MethodLLVMEmitter::emit_memory_load (MonoIrBuilder &builder, llvm::Type *type, llvm::Value *address,
                                     MonoType *location)
{
	llvm::LoadInst *value =
		builder.CreateAlignedLoad (type, address, access_alignment (location));

	/* A volatile read has acquire semantics (I.12.6.7). */
	if (prefixes.volatile_) {
		value->setVolatile (true);
		builder.CreateFence (llvm::AtomicOrdering::Acquire);
	}

	return value;
}

/// One store of VALUE to ADDRESS, through the write barrier when LOCATION holds a
/// reference, honoring the volatile. and unaligned. prefixes.
void
MethodLLVMEmitter::emit_memory_store (MonoIrBuilder &builder, llvm::Value *value,
                                      llvm::Value *address, MonoType *location)
{
	/* A volatile write has release semantics (I.12.6.7). */
	if (prefixes.volatile_)
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

	/*
	 * A struct with references inside cannot just be stored either: the collector
	 * has to mark the cards its reference fields land on. Its barrier copies from
	 * memory to memory, so the value takes a detour through a stack slot to have
	 * an address at all. A struct without references keeps the plain store.
	 */
	MonoClass *klass = location->byref
	                           ? nullptr
	                           : mono_class_from_mono_type_internal (location);

	if (klass != nullptr && m_class_is_valuetype (klass) && m_class_has_references (klass)) {
		MonoIrBuilder entry (entry_block, entry_block->begin ());
		llvm::AllocaInst *temp = entry.CreateAlloca (value->getType ());

		temp->setAlignment (type_alignment (location));
		builder.CreateAlignedStore (value, temp, temp->getAlign ());
		builder.CreateCall (value_copy_decl (), {address, temp, builder.getInt32 (1),
		                                         class_symbol (klass, "mono_class_")});
		return;
	}

	llvm::StoreInst *store =
		builder.CreateAlignedStore (value, address, access_alignment (location));

	if (prefixes.volatile_)
		store->setVolatile (true);
}

} // namespace mono
