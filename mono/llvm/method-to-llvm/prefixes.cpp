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
 * Record a prefix for the instruction that follows. A prefix emits no code of its
 * own. It sets a flag the next instruction reads, and emit_instruction () clears
 * the set once that instruction is done. ECMA-335 III.2 defines all six.
 *
 * Three of them are read elsewhere. constrained. names the receiver type of the
 * next callvirt, which is what lets one site serve a value type and a reference
 * type alike, and the call site resolves it. tail. asks for the caller's frame to
 * be reused, which should_tail_call () grants only for the shapes that can take
 * it. readonly. drops the array type check on the ldelema that follows, and only a
 * reference element type has one to drop.
 *
 * unaligned. and volatile. are read here, by the two memory accessors below.
 *
 * no. is permission to skip a fault check rather than an obligation, so this
 * records nothing and every check stays.
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
		// A request the call site can decline. It falls back to an ordinary
		// call when the shape does not allow a tail call.
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

/// Whether one access of this type at this alignment can be an LLVM atomic
/// instruction, so that the ordering rides the access rather than a fence beside it.
///
/// LLVM takes an atomic load or store of a scalar whose size is a whole power-of-two
/// number of bytes. A scalar is an integer, a pointer or a floating point value.
/// Anything the machine cannot do in one instruction lowers into a call into the
/// atomic runtime library, which nothing here defines. So the access must also fit
/// the target's atomic width and be aligned to at least its own size.
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

/// One load from an address, read as the given type, honoring the volatile. and
/// unaligned. prefixes on the instruction this emits.
llvm::Value *
MethodLLVMEmitter::emit_memory_load (MonoIrBuilder &builder, llvm::Type *type, llvm::Value *address,
                                     MonoType *location)
{
	llvm::Align align = access_alignment (location);
	llvm::LoadInst *value = builder.CreateAlignedLoad (type, address, align);

	/*
	 * A volatile read has acquire semantics (I.12.6.7). Acquire on the load
	 * itself orders only that access, which is all III.2.6 asks for. A fence is
	 * a barrier at this point in the program and orders everything around it
	 * too. The load stays volatile either way, because I.12.6.7 forbids removing
	 * or coalescing a volatile operation and atomic alone does not promise that.
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

/// One store of a value to an address, through the write barrier when the location
/// holds a reference, honoring the volatile. and unaligned. prefixes.
///
/// The value is what coerce_to_location () produced. For a value class it is
/// therefore the address of the bytes to copy rather than the bytes themselves.
void
MethodLLVMEmitter::emit_memory_store (MonoIrBuilder &builder, llvm::Value *value,
                                      llvm::Value *address, MonoType *location)
{
	llvm::Align align = access_alignment (location);
	/*
	 * Only a plain scalar store can carry the release itself. A reference goes
	 * through the write barrier, which does the store inside the call, and a
	 * value class is a copy rather than one instruction. Neither is a single
	 * access that can hold an ordering, so both order with a fence.
	 */
	bool atomic = prefixes.volatile_ && !mini_type_is_reference (location)
	              && !held_in_memory (location)
	              && can_access_atomically (value->getType (), align);

	/* A volatile write has release semantics (I.12.6.7). */
	if (prefixes.volatile_ && !atomic)
		builder.CreateFence (llvm::AtomicOrdering::Release);

	/*
	 * A reference entering memory the collector can be tracking goes through the
	 * barrier. The address here can point anywhere, and the generic barrier is
	 * the one that tolerates that.
	 */
	if (mini_type_is_reference (location)) {
		builder.CreateCall (wbarrier_decl (), {address, value});
		return;
	}

	if (held_in_memory (location)) {
		MonoClass *klass =
			mono_class_from_mono_type_internal (mini_get_underlying_type (location));

		/*
		 * A struct with references inside cannot move as plain bytes. The collector
		 * must mark the cards its reference fields land on, and only its own
		 * copy routine knows how to do that.
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
