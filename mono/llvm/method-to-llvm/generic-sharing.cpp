/**
 * \file
 * \brief What a body shared between reference instantiations reads out of its context.
 *
 * Every reference instantiation of a method compiles to the same instructions,
 * because a reference is one pointer whatever it points at: field offsets, array
 * element sizes and vtable slot indices all come out the same. What differs is
 * the metadata the body names - the MonoClass it casts to, the vtable it
 * allocates from, the statics block it reads. A shared body fetches each of
 * those from the runtime generic context of the instantiation that entered it.
 */

#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "sidetables.hpp"
#include "../hidden-return.hpp"
#include "../passes/rgctx-fetch.hpp"

#include "mini.h"
#include "mini-runtime.h"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <string>
#include <vector>

namespace mono {

char SharingRefusal::ID = 0;

void
MethodLLVMEmitter::cannot_share (const llvm::Twine &what)
{
	if (sharing_refusal.empty ())
		sharing_refusal = what.str ();
}

bool
MethodLLVMEmitter::depends_on_context (MonoClass *klass)
{
	/*
	 * A generic type definition carries its own type parameters, which are not
	 * this method's: typeof (List<>) is one object however the body it appears
	 * in was instantiated. mono_class_check_context_used () counts them all the
	 * same, so this returns false for a GTD instead of inflating its own
	 * parameters against this method's context and turning List<> into
	 * List<T>.
	 */
	if (mono_class_is_gtd (klass))
		return false;

	return sharing () && mono_class_check_context_used (klass) != 0;
}

bool
MethodLLVMEmitter::depends_on_context (MonoClassField *field)
{
	return depends_on_context (field->parent);
}

bool
MethodLLVMEmitter::depends_on_context (MonoMethod *target)
{
	MonoGenericContext *own = mini_method_get_context (target);

	if (own != nullptr && mono_generic_context_check_used (own) != 0)
		return sharing ();

	return depends_on_context (target->klass);
}

bool
MethodLLVMEmitter::calls_through_context (MonoMethod *target)
{
	if (!depends_on_context (target))
		return false;

	/*
	 * A wrapper is generated around one instantiation and carries no token the
	 * context can resolve, so mono's own entry for a method's code refuses
	 * one. The caller compiles concrete instead, and gets the wrapper it named.
	 */
	if (target->wrapper_type != MONO_WRAPPER_NONE) {
		cannot_share ("a call to a wrapper around an open method");
		return false;
	}

	return true;
}

bool
MethodLLVMEmitter::takes_context_argument () const
{
	return sharing () && mono_method_needs_static_rgctx_invoke (method, TRUE);
}

/*
 * Two sources, split by whether the method has a receiver - upstream's own
 * split, and what keeps virtual and interface dispatch out of this entirely.
 *
 * An object's vtable is its instantiation's, so an instance method of a shared
 * class reads its context out of `this` and needs nothing from its caller. A
 * static method, a value type's, a default interface method and any method with
 * type parameters of its own have no such receiver. Each of those is entered
 * with the context in a register instead, which the instantiation's own context
 * stub writes.
 */
void
MethodLLVMEmitter::open_sharing (MonoIrBuilder &builder)
{
	if (!sharing ())
		return;

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Align align (TARGET_SIZEOF_VOID_P);

	if (takes_context_argument ()) {
		rgctx = function->getArg (function->arg_size () - 1);

		/*
		 * A copy in the frame, for the stack walk described below. The value
		 * itself stays the argument: a register is what every fetch reads, and
		 * the slot is only there to be found from outside.
		 */
		llvm::AllocaInst *home = builder.CreateAlloca (ptr, nullptr, "rgctx.home");

		home->setAlignment (align);
		builder.CreateAlignedStore (rgctx, home, align);
		pin_context_slot (builder, home);
		return;
	}

	if (args.empty () || !mono_method_signature_internal (method)->hasthis)
		return;

	/*
	 * Read once, in the prologue, so that one value dominates every fetch below
	 * it. A null receiver faults on this load instead of on whatever
	 * instruction in the body first uses it. The two differ only for a body
	 * that reads the context without ever touching `this`, and a reference
	 * type's instance method is reached through callvirt, which has already
	 * checked.
	 */
	llvm::Value *self = builder.CreateAlignedLoad (ptr, args[0].alloca, align);

	rgctx = load_vtable (builder, self);
	pin_context_slot (builder, args[0].alloca);
}

/*
 * A shared body's jit info names the shared method, so a stack walk that wants
 * the instantiation a frame is running as reads what the frame was entered
 * with - the receiver, or the context itself where there is no receiver. That
 * is what MonoGenericJitInfo describes and this pins.
 *
 * The marker does both halves. The intrinsic is neither a load nor a store, so
 * mem2reg leaves the slot in memory. Codegen then resolves its operand against
 * the laid-out frame, which is the register and displacement jinfo.cpp reads
 * back out of the stackmap section.
 */
void
MethodLLVMEmitter::pin_context_slot (MonoIrBuilder &builder, llvm::Value *slot)
{
	emit_stackmap_marker (builder, { builder.getInt64 (rgctx_stackmap_id),
	                                builder.getInt32 (0), slot });
	pinned_receiver = true;
}

/// Returns the icall that fills a slot. A slot the class owns is filled from
/// the class vtable, and one the method owns from the MRGCTX its caller passed.
/// mini_get_rgctx_entry_slot () marks a slot it put in the MRGCTX with
/// MONO_RGCTX_SLOT_MAKE_MRGCTX, which is what tells the two apart.
static MonoJitICallId
fill_icall_for (uint32_t slot)
{
	return MONO_RGCTX_SLOT_IS_MRGCTX (slot) ? MONO_JIT_ICALL_mono_fill_method_rgctx
	                                        : MONO_JIT_ICALL_mono_fill_class_rgctx;
}

/*
 * Where the runtime keeps a slot, as the byte offsets a load chain walks
 * through from the value the fill icall takes, the slot itself last.
 *
 * fill_runtime_generic_context () (mini-generic-sharing.c) holds the entries in
 * a chain of arrays. Each array links to the next through its first usable
 * element, and every array is larger than the one in front of it. Which array
 * holds an entry therefore follows from its index alone.
 *
 * A class rgctx hangs off the vtable the caller passes. An MRGCTX is the first
 * array itself, behind the two fields it opens with.
 */
static std::vector<uint32_t>
walk_to_slot (uint32_t index, bool mrgctx)
{
	std::vector<uint32_t> offsets;
	uint32_t pointer = TARGET_SIZEOF_VOID_P;
	uint32_t opening =
		mrgctx ? MONO_SIZEOF_METHOD_RUNTIME_GENERIC_CONTEXT / pointer : 0;

	if (!mrgctx)
		offsets.push_back (
			MONO_STRUCT_OFFSET (MonoVTable, runtime_generic_context));

	uint32_t first = 0;
	uint32_t size = mono_class_rgctx_get_array_size (0, mrgctx) - opening;

	for (int level = 0;; ++level) {
		// Only the first array of an MRGCTX has the opening fields in front
		// of it, and the link is the element behind them.
		uint32_t at = level == 0 ? opening : 0;

		if (index < first + size - 1) {
			offsets.push_back ((index - first + 1 + at) * pointer);
			break;
		}

		offsets.push_back (at * pointer);
		first += size - 1;
		size = mono_class_rgctx_get_array_size (level + 1, mrgctx);
	}

	return offsets;
}

/// What a fetch site tells RgctxFetchPass: the operand the context arrives in,
/// and the walk that reaches the slot from it.
static std::string
fetch_spec (const llvm::Function *fill, uint32_t index, bool mrgctx)
{
	std::string spec =
		"ctx=" + std::to_string (natural_parameter_index (0, fill)) + ",walk=";
	const char *separator = "";

	for (uint32_t offset : walk_to_slot (index, mrgctx)) {
		spec += separator + std::to_string (offset);
		separator = ":";
	}

	return spec;
}

static MonoJumpInfoType
patch_kind_for (MonoRgctxInfoType info_type)
{
	switch (info_type) {
	case MONO_RGCTX_INFO_STATIC_DATA:
	case MONO_RGCTX_INFO_KLASS:
	case MONO_RGCTX_INFO_VTABLE:
	case MONO_RGCTX_INFO_CAST_CACHE:
	case MONO_RGCTX_INFO_REFLECTION_TYPE:
		return MONO_PATCH_INFO_CLASS;
	case MONO_RGCTX_INFO_METHOD:
	case MONO_RGCTX_INFO_GENERIC_METHOD_CODE:
		return MONO_PATCH_INFO_METHODCONST;
	case MONO_RGCTX_INFO_CLASS_FIELD:
		return MONO_PATCH_INFO_FIELD;
	default:
		g_assert_not_reached ();
	}
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::rgctx_fetch (MonoIrBuilder &builder, MonoRgctxInfoType info_type,
                                void *data)
{
	if (rgctx == nullptr) {
		cannot_share ("a body with no receiver to read its generic context from");
		return llvm::ConstantPointerNull::get (llvm::PointerType::get (context (), 0));
	}

	MonoJumpInfo patch {};
	MonoJumpInfoRgctxEntry lookup {};

	patch.type = patch_kind_for (info_type);
	patch.data.target = data;

	/*
	 * A method with type parameters of its own owns its entries, because the
	 * class rgctx behind a vtable cannot resolve them. Everything else puts
	 * them in the class's, which is what an instantiation's vtable carries.
	 */
	if (mini_method_needs_mrgctx (method)) {
		lookup.d.method = method;
		lookup.in_mrgctx = TRUE;
	} else {
		lookup.d.klass = method->klass;
		lookup.in_mrgctx = FALSE;
	}

	lookup.data = &patch;
	lookup.info_type = info_type;

	uint32_t slot = (uint32_t) mini_get_rgctx_entry_slot (&lookup);
	llvm::Expected<llvm::Function *> fill = icall_wrapper_decl (fill_icall_for (slot));

	if (!fill)
		return fill.takeError ();

	uint32_t index = (uint32_t) MONO_RGCTX_SLOT_INDEX (slot);
	bool mrgctx = MONO_RGCTX_SLOT_IS_MRGCTX (slot) != FALSE;

	/*
	 * Filling a slot can create a vtable and run a class initializer, so this
	 * is a call rather than a load: it has to happen where the IL asked for the
	 * metadata, not where the body was entered. Every fetch after the first
	 * finds the slot filled, and RgctxFetchPass puts the load that reads it in
	 * front of the call. That lowering runs late: the guard it builds is a
	 * diamond, which hides the call from a pass that reads the call whole.
	 */
	(*fill)->addFnAttr (rgctx_fetch_attribute);

	std::string spec = fetch_spec (*fill, index, mrgctx);
	llvm::Value *info = emit_protected_call (
		builder, *fill,
		adapt_to_callee (builder, *fill, {rgctx, builder.getInt32 (index)}),
		[&] (llvm::CallBase *site) {
			site->addFnAttr (llvm::Attribute::get (
				context (), rgctx_walk_attribute, spec));
		});

	if (!info->getType ()->isPointerTy ())
		info = builder.CreateIntToPtr (info, llvm::PointerType::get (context (), 0));

	return info;
}

bool
MethodLLVMEmitter::pass_context_to (llvm::Function *callee, std::vector<llvm::Value *> &args)
{
	unsigned count = callee->arg_size ();

	if (count == 0 || !callee->hasParamAttribute (count - 1, llvm::Attribute::Nest))
		return false;

	args.push_back (rgctx);
	return true;
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::code_operand (MonoIrBuilder &builder, MonoMethod *target)
{
	if (!calls_through_context (target))
		return code_address_symbol (target);

	/*
	 * The instantiation's own entry, which is the thunk in front of whatever
	 * tier is running it - and, for a method entered with its context in a
	 * register, the context stub in front of that. So an address taken here
	 * carries the instantiation without anything having to key it, and it stays
	 * right when a later compile replaces the body.
	 */
	return rgctx_fetch (builder, MONO_RGCTX_INFO_GENERIC_METHOD_CODE, target);
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::class_operand (MonoIrBuilder &builder, MonoClass *klass,
                                  const char *prefix)
{
	if (!depends_on_context (klass))
		return class_symbol (klass, prefix);

	llvm::StringRef which (prefix);
	MonoRgctxInfoType info_type = MONO_RGCTX_INFO_KLASS;

	if (which == "mono_vtable_")
		info_type = MONO_RGCTX_INFO_VTABLE;
	else if (which == "mono_statics_")
		info_type = MONO_RGCTX_INFO_STATIC_DATA;

	return rgctx_fetch (builder, info_type, klass);
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::field_operand (MonoIrBuilder &builder, MonoClassField *field)
{
	if (!depends_on_context (field))
		return field_symbol (field);

	return rgctx_fetch (builder, MONO_RGCTX_INFO_CLASS_FIELD, field);
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::method_operand (MonoIrBuilder &builder, MonoMethod *target)
{
	if (!depends_on_context (target))
		return method_symbol (target);

	return rgctx_fetch (builder, MONO_RGCTX_INFO_METHOD, target);
}

} // namespace mono
