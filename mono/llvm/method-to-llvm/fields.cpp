#include "method-to-llvm.hpp"
#include "method-symbols.hpp"
#include "operand-class.hpp"
#include "runtime-error.hpp"
#include "../passes/class-init.hpp"
#include "../vtable-facts.hpp"
#include "../runtime/naming.hpp"
#include "../runtime/options.hpp"
#include "mini-runtime.h"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/class.h"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/gc-internals.h"
#include "mono/metadata/loader.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/remoting.h"
#include "mono/metadata/threads-types.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Type.h>
#include <cstdint>
#include <cstdio>

namespace mono {

namespace {

/// The addresses and shifts a reference store needs to mark a card itself.
///
/// card_table is null when the collector marks no cards, and the other fields
/// then say nothing. That is the one field to test before reading the rest.
struct WriteBarrierLayout {
	uint8_t *card_table = nullptr;
	uintptr_t card_mask = 0;
	int card_bits = 0;
	char *nursery_start = nullptr;
	int nursery_bits = 0;
	bool value_decides = false;
	volatile gboolean *concurrent_flag = nullptr;
};

/// Reads the collector's write-barrier layout, once for the process.
///
/// The collector fixes each address and shift while it starts, before any method
/// compiles, so a compile can bake them in.
const WriteBarrierLayout &
write_barrier_layout ()
{
	static const WriteBarrierLayout layout = [] {
		WriteBarrierLayout read;
		gpointer mask = nullptr;
		size_t nursery_size = 0;

		read.card_table = mono_gc_get_card_table (&read.card_bits, &mask);

		/*
		 * mono_gc_card_table_nursery_check () aborts under Boehm, so a null
		 * table has to send us back before that call runs. Sgen sets the nursery
		 * bounds earlier in sgen_gc_init () than the card table, so a table this
		 * call reports means the bounds are already set.
		 */
		if (read.card_table == nullptr)
			return read;

		read.card_mask = reinterpret_cast<uintptr_t> (mask);
		read.nursery_start = static_cast<char *> (
			mono_gc_get_nursery (&read.nursery_bits, &nursery_size));

		// A concurrent major collector marks the card whatever the value is.
		// This is the split mono_gc_get_specific_write_barrier () keeps two
		// wrappers for.
		read.value_decides = mono_gc_card_table_nursery_check ();

		// Such a collector marks those cards only while a collection runs, and
		// this flag is what says so.
		if (!read.value_decides)
			read.concurrent_flag = mono_gc_get_concurrent_collection_flag ();

		return read;
	}();

	return layout;
}

} // namespace

llvm::FunctionCallee
MethodLLVMEmitter::wbarrier_decl ()
{
	llvm::LLVMContext &ctx = context ();
	llvm::Type *ptr = llvm::PointerType::get (ctx, 0);

	return module->getOrInsertFunction ("mono_gc_wbarrier_generic_store_internal",
	                                    llvm::Type::getVoidTy (ctx), ptr, ptr);
}

/// Stores a reference and marks the card the collector reads for it.
///
/// address can name any location a reference lives in: an object field, a static, an
/// array element, or a local the method took the address of. value must be a pointer.
///
/// The caller owns the volatile. prefix. The store this writes carries no ordering.
///
/// The builder can end on a different block than it started on, so a caller holding a
/// block pointer has to take it again.
void
MethodLLVMEmitter::emit_reference_store (MonoIrBuilder &builder, llvm::Value *address,
                                         llvm::Value *value, llvm::Align align,
                                         ManagedAccess access)
{
	const WriteBarrierLayout &gc = write_barrier_layout ();

	if (gc.card_table == nullptr) {
		builder.CreateCall (wbarrier_decl (), {address, value});
		return;
	}

	llvm::Type *word = builder.getIntNTy (TARGET_SIZEOF_VOID_P * 8);
	llvm::Value *nursery = llvm::ConstantInt::get (
		word, reinterpret_cast<uintptr_t> (gc.nursery_start) >> gc.nursery_bits);

	llvm::StoreInst *store = builder.CreateAlignedStore (value, address, align);

	if (llvm::MDNode *tag = tbaa_tag (access, /*is_reference=*/true))
		store->setMetadata (llvm::LLVMContext::MD_tbaa, tag);

	llvm::Value *target = builder.CreatePtrToInt (address, word);
	llvm::Value *target_is_old = builder.CreateICmpNE (
		builder.CreateLShr (target, gc.nursery_bits), nursery, "wb_target_is_old");

	/*
	 * The collector reads the card under an old destination that names a young
	 * object. While a concurrent collection runs, it reads the card under every
	 * old destination:
	 *
	 *     mark = target_is_old && (value_is_young || concurrent_collection)
	 *
	 * The wrapper reads the destination back for the value test, because its
	 * caller made the store. We test the value we stored instead. A thread that
	 * puts a different reference there marks the card with its own barrier, so the
	 * card is marked either way.
	 */
	auto value_is_young = [&] {
		llvm::Value *stored =
			builder.CreateLShr (builder.CreatePtrToInt (value, word), gc.nursery_bits);

		return builder.CreateICmpEQ (stored, nursery, "wb_value_is_young");
	};

	llvm::BasicBlock *card = llvm::BasicBlock::Create (context (), "wb_mark", function);
	llvm::BasicBlock *done = llvm::BasicBlock::Create (context (), "wb_done", function);

	if (gc.value_decides) {
		// A collector that collects nothing concurrently drops the last term. The
		// two tests that are left are shifts and compares on values already in
		// registers, so one branch carries them both.
		builder.CreateCondBr (builder.CreateAnd (target_is_old, value_is_young ()),
		                      card, done);
	} else if (gc.concurrent_flag != nullptr) {
		llvm::Type *flag_type = builder.getIntNTy (sizeof (gboolean) * 8);
		llvm::BasicBlock *old_target =
			llvm::BasicBlock::Create (context (), "wb_target_old", function);

		builder.CreateCondBr (target_is_old, old_target, done);
		builder.SetInsertPoint (old_target);

		/*
		 * The load is volatile, which keeps it inside its loop and behind the
		 * store. The collector sets the flag with the world stopped, so a load in
		 * front of the store can read false while the collection starts. The store
		 * then lands with no card in an object the marker already read.
		 */
		llvm::Constant *flag =
			address_symbol ("mono_gc_concurrent_collection_flag",
		                        const_cast<gboolean *> (gc.concurrent_flag));
		llvm::Value *concurrent = builder.CreateICmpNE (
			builder.CreateAlignedLoad (flag_type, flag, llvm::Align (sizeof (gboolean)),
			                           true, "wb_concurrent"),
			llvm::ConstantInt::get (flag_type, 0));

		builder.CreateCondBr (builder.CreateOr (value_is_young (), concurrent), card,
		                      done);
	} else {
		// A collector that keeps no flag can run a concurrent collection at any
		// moment, so every old destination gets a card.
		builder.CreateCondBr (target_is_old, card, done);
	}

	builder.SetInsertPoint (card);

	llvm::Value *index = builder.CreateLShr (target, gc.card_bits);

	// A zero mask is a table that covers the address space, so the index needs none.
	if (gc.card_mask != 0)
		index = builder.CreateAnd (index, llvm::ConstantInt::get (word, gc.card_mask));

	llvm::Constant *table = address_symbol ("mono_gc_card_table", gc.card_table);

	builder.CreateAlignedStore (builder.getInt8 (1),
	                            builder.CreateGEP (builder.getInt8Ty (), table, index),
	                            llvm::Align (1));
	builder.CreateBr (done);
	builder.SetInsertPoint (done);
}

/// Resolves the field that token names and lays out its declaring class, so callers can
/// read the field's offset.
///
/// An instance opcode passes out_is_static. A field that turns out static then succeeds
/// instead of raising a mismatch, and out_is_static is set to true, leaving the reroute
/// to the caller. A static opcode always refuses an instance field.
llvm::Expected<MonoClassField *>
MethodLLVMEmitter::resolve_field (uint32_t token, bool want_static, bool *out_is_static)
{
	ERROR_DECL (metadata_error);
	MonoClass *klass = nullptr;
	MonoClassField *field;

	if (in_wrapper ()) {
		field = static_cast<MonoClassField *> (wrapper_data (token));

		if (field == nullptr)
			return invalid_il (llvm::Twine ("wrapper data slot ") + llvm::Twine (token)
			                   + " does not name a field");
	} else {
		field = mono_field_from_token_checked (
			m_class_get_image (method->klass), token, &klass,
			mono_method_get_context (method), metadata_error);

		if (field == nullptr)
			return runtime_error (metadata_error);
	}

	if (checks_accessibility () && !mono_method_can_access_field (method, field))
		return field_access_failure (field);

	/*
	 * Laying the class out is what makes the offset readable. It settles metadata
	 * only, and it is not the class initializer, which must never run here.
	 */
	mono_class_init_checked (field->parent, metadata_error);
	if (!is_ok (metadata_error))
		return runtime_error (metadata_error);

	bool is_static = (mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) != 0;

	if (out_is_static != nullptr)
		*out_is_static = is_static;
	if (is_static && !want_static && out_is_static != nullptr)
		return field;
	if (is_static != want_static) {
		char *name = mono_field_full_name (field);
		llvm::Error error =
			invalid_il (llvm::Twine ("this instruction needs ")
		                    + (want_static ? "a static" : "an instance") + " field, but "
		                    + name + " is " + (is_static ? "static" : "an instance field"));

		g_free (name);
		return std::move (error);
	}

	return field;
}

llvm::Constant *
MethodLLVMEmitter::extern_symbol (const std::string &name)
{
	if (llvm::GlobalValue *existing = module->getNamedValue (name))
		return existing;

	return new llvm::GlobalVariable (*module, llvm::Type::getInt8Ty (context ()), false,
	                                 llvm::GlobalValue::ExternalLinkage, nullptr, name);
}

llvm::Constant *
MethodLLVMEmitter::vtable_for (MonoClass *klass)
{
	// An open class comes from the context rather than from a symbol, and a
	// shared body has no one vtable to name. A generic type definition and an
	// open constructed type both carry type parameters, which the runtime lays
	// out no vtable over.
	if (depends_on_context (klass) || mono_class_is_gtd (klass)
	    || mono_class_is_open_constructed_type (m_class_get_byval_arg (klass)))
		return nullptr;

	return class_symbol (klass, "mono_vtable_");
}

/// The global a class's vtable is named by, carrying what the class alone
/// settles about it.
llvm::Constant *
MethodLLVMEmitter::vtable_symbol (MonoClass *klass, const std::string &symbol)
{
	if (llvm::GlobalValue *existing = module->getNamedValue (symbol))
		return existing;

	auto *global = new llvm::GlobalVariable (*module, llvm::Type::getInt8Ty (context ()),
	                                         false, llvm::GlobalValue::ExternalLinkage,
	                                         nullptr, symbol);

	if (std::optional<VTableFacts> facts = vtable_facts_for (klass))
		mark_vtable_facts (*global, *facts);

	return global;
}

/// What a fold can read off klass's vtable symbol, or nothing where this
/// compile cannot state every field.
///
/// Nothing leaves the symbol unmarked rather than marked with a hole in it.
std::optional<VTableFacts>
MethodLLVMEmitter::vtable_facts_for (MonoClass *klass)
{
	llvm::Expected<llvm::Constant *> named =
		typeof_symbol (m_class_get_byval_arg (klass));

	if (!named) {
		llvm::consumeError (named.takeError ());
		return std::nullopt;
	}

	if (*named == nullptr)
		return std::nullopt;

	VTableFacts facts;

	// Each pointer is the symbol something else compares against: a type test
	// reads the class word, and typeof names the System.Type object. The two
	// sides have to be one value for the comparison to fold.
	facts.klass = class_symbol (klass, "mono_class_");
	facts.type = *named;
	facts.rank = uint8_t (m_class_get_rank (klass));
	return facts;
}

void
MethodLLVMEmitter::record_external (const std::string &name, ExternalSymbol::Kind kind,
                                    void *object)
{
	if (externals != nullptr)
		externals->push_back ({ name, kind, object });
}

/// Builds a symbol name for object: name makes it readable, the pointer's own address
/// makes it unique.
///
/// A printed name alone is not unique. Two assemblies loaded from different bytes can
/// share an assembly name and define classes, methods and fields that print identically.
/// Assembly.Load (byte[]) on two builds of one source is the ordinary way this happens.
/// MonoJit::register_symbol () refuses a name it has already bound to a different
/// address, so without the pointer the second assembly's first compile fails.
std::string
MethodLLVMEmitter::identity_symbol (const std::string &name, const void *object)
{
	char suffix[32];

	snprintf (suffix, sizeof (suffix), "@%p", object);
	return name + suffix;
}

/// The address the engine must resolve for a per-class run-time structure.
///
/// A class contributes three symbols: mono_statics_<class> for the block that holds its
/// static fields, mono_vtable_<class> for its MonoVTable, and mono_class_<class> for the
/// MonoClass itself. Each class gets one relocation instead of one per field. Every
/// static of a class shares a symbol and differs only in the GEP offset.
llvm::Constant *
MethodLLVMEmitter::class_symbol (MonoClass *klass, const char *prefix)
{
	// A shared body serves every reference instantiation, and each of them has
	// its own class, vtable and statics. class_operand () is the form that
	// fetches one from the context. A site that still burns the symbol in
	// refuses here, and the method is compiled against the instantiation the
	// caller asked for instead.
	if (depends_on_context (klass))
		cannot_share (llvm::Twine (prefix) + "an open class");

	char *name = mono_type_full_name (m_class_get_byval_arg (klass));
	std::string symbol = identity_symbol (std::string (prefix) + name, klass);
	ExternalSymbol::Kind kind = ExternalSymbol::Kind::Class;

	if (llvm::StringRef (prefix) == "mono_vtable_")
		kind = ExternalSymbol::Kind::VTable;
	else if (llvm::StringRef (prefix) == "mono_statics_")
		kind = ExternalSymbol::Kind::Statics;

	g_free (name);
	record_external (symbol, kind, klass);

	llvm::Constant *symbolic = kind == ExternalSymbol::Kind::VTable
	                                   ? vtable_symbol (klass, symbol)
	                                   : extern_symbol (symbol);

	// A pass that answers a dispatch site has the vtable and needs the class. A
	// pass that answers a type test has the class the test names.
	if (kind == ExternalSymbol::Kind::VTable || kind == ExternalSymbol::Kind::Class)
		mark_class_reference (*llvm::cast<llvm::GlobalValue> (symbolic), klass);

	return symbolic;
}

llvm::Constant *
MethodLLVMEmitter::field_symbol (MonoClassField *field)
{
	if (depends_on_context (field))
		cannot_share ("a field of an open class");

	char *name = mono_field_full_name (field);
	std::string symbol = identity_symbol (std::string ("mono_field_") + name, field);

	g_free (name);
	record_external (symbol, ExternalSymbol::Kind::Field, field);
	return extern_symbol (symbol);
}

/// Whether klass's type initializer has already run in the domain this method
/// compiles for.
///
/// A true answer is permanent: mono_runtime_class_init_full () raises the flag only
/// after the initializer returns, and no path lowers it again. A false answer means
/// "not known", not "not initialized" - the flag is still 0 on the thread that is
/// inside the initializer right now. So a caller can drop a check on true, and must
/// keep one on false.
bool
MethodLLVMEmitter::cctor_already_ran (MonoClass *klass)
{
	// An open class has no vtable of its own. Which vtable the code gets is settled
	// by the context a shared body is entered with, so the check has to stand.
	if (depends_on_context (klass))
		return false;

	MonoVTable *vtable = mono_class_try_get_vtable (cfg->domain, klass);

	return vtable != nullptr && vtable->initialized != 0;
}

/// Whether a read of field answers the same for the rest of the program.
///
/// An ordinary program writes such a field only from the type initializer. ECMA-335
/// III.4.15 says a store through the address has unpredictable behavior, and
/// RuntimeFieldInfo.CheckStaticReadonly () refuses a write through FieldInfo. A
/// debugger client goes under that check, so the answer is false while
/// gen_sdb_seq_points is on.
bool
MethodLLVMEmitter::invariant_static_read (MonoClassField *field)
{
	MonoType *ftype = mono_field_get_type_internal (field);

	if ((ftype->attrs & FIELD_ATTRIBUTE_INIT_ONLY) == 0)
		return false;

	if (mini_get_debug_options ()->gen_sdb_seq_points)
		return false;

	// I.12.6.7 forbids coalescing a volatile operation, which is the transform the
	// mark exists to license.
	if (prefixes.volatile_)
		return false;

	// Each thread or context gets storage of its own, and the type initializer
	// writes only the copy belonging to whichever one ran it. So the value one
	// reads says nothing about what the next reads, whatever initonly claims.
	if (mono_class_field_is_special_static (field))
		return false;

	/*
	 * !invariant.load claims the value at every point the address is
	 * dereferenceable. LLVM then moves a marked load above a class-init guard that
	 * still stands, and reads the zeroed statics block. Dominance by the guard is
	 * not enough. The initializer must be complete at translate time, which also
	 * keeps the mark away from a method the initializer itself calls:
	 * mono_runtime_class_init_full () lets that thread back past the guard while
	 * the fields are still zero.
	 */
	return cctor_already_ran (field->parent);
}

/// Emits the check that runs klass's static constructor, unless it has already run.
///
/// The constructor itself never runs here. It is arbitrary managed code, and a
/// compilation thread must not execute it. A site that still needs the check gets a
/// call, and ClassInitPass deletes the ones a dominating check already covers.
llvm::Error
MethodLLVMEmitter::emit_class_init (MonoIrBuilder &builder, MonoClass *klass)
{
	// The call would find the work done and return at once.
	if (cctor_already_ran (klass))
		return llvm::Error::success ();

	// Goes through the wrapper because a cctor can throw. The failure stays pending
	// until the wrapper's check turns it into a real exception.
	llvm::Expected<llvm::Function *> init =
		icall_wrapper_decl (MONO_JIT_ICALL_mono_generic_class_init);

	if (!init)
		return init.takeError ();

	llvm::Expected<llvm::Value *> vtable = class_operand (builder, klass, "mono_vtable_");

	if (!vtable)
		return vtable.takeError ();

	(*init)->addFnAttr (class_init_attribute);
	emit_protected_call (builder, *init,
	                     adapt_to_callee (builder, *init, {*vtable}));
	return llvm::Error::success ();
}

/// Whether an instance access to field through receiver can land on a transparent proxy.
///
/// If it can, the field's bytes are not at their offset. They live in the real object
/// the proxy stands for, possibly in another context or domain. The access must go
/// through the runtime's remoting field wrappers instead.
///
/// A receiver that is this method's own `this` usually proves a real object, but not on
/// ContextBoundObject and not on MarshalByRefObject itself. The same-context shortcut in
/// mono_remoting_wrapper runs those bodies with `this` still the proxy, so their fields
/// check on every access.
bool
MethodLLVMEmitter::remote_field_access (StackValue receiver, MonoClassField *field)
{
#ifdef DISABLE_REMOTING
	return false;
#else
	MonoClass *klass = field->parent;

	if (m_class_is_valuetype (klass) || stack_type (receiver.type) != ObjectRef)
		return false;
	if (mono_class_is_contextbound (klass)
	    || klass == mono_defaults.marshalbyrefobject_class)
		return true;
	return mono_class_is_marshalbyref (klass) && !is_own_this (receiver.value);
#endif
}

/// Pushes the operands every remoting field wrapper needs after the object: the
/// declaring MonoClass, the MonoClassField, and the field's offset, in that order. A
/// wrapper call then collects its arguments off the stack like any other call.
void
MethodLLVMEmitter::push_field_wrapper_operands (MonoIrBuilder &builder,
                                                MonoClassField *field)
{
	MonoType *nint = mono_get_int_type ();

	push_stack (class_symbol (field->parent, "mono_class_"), nint);
	push_stack (field_symbol (field), nint);
	push_stack (builder.getInt64 (m_field_get_offset (field)), nint);
}

/// The tag a field access through this receiver can carry.
///
/// An object receiver gets the field tag, and so does a managed pointer this
/// body gave its type (trusted_byrefs). Anything else gets the coarse leaf.
/// ManagedAccess says what each of those rests on.
ManagedAccess
MethodLLVMEmitter::field_access (const StackValue &object, MonoClassField *field)
{
	// field_address () takes the offset from the token alone and never asks
	// whether the receiver agrees. That is right for addressing, where a
	// receiver reaching the wrong storage is the caller's error. A tag claims
	// some other access cannot reach that storage, so it needs the receiver.
	if (stack_type (object.type) == ObjectRef || trusted_byrefs.count (object.value) != 0)
		return ManagedAccess::of_field (field);

	return ManagedAccess::typed ();
}

llvm::Expected<llvm::Value *>
MethodLLVMEmitter::field_address (MonoIrBuilder &builder, StackValue object,
                                  MonoClassField *field, bool null_check)
{
	StackType type = stack_type (object.type);

	if (type != ObjectRef && type != ManagedPtr && type != NativeInt)
		return invalid_il (llvm::Twine ("a field cannot be reached through operand type ")
		                   + describe (object.type, type));

	llvm::Value *base = object.value;

	// A native int receiver is only a number until something dereferences it.
	if (!base->getType ()->isPointerTy ())
		base = builder.CreateIntToPtr (base, llvm::PointerType::get (context (), 0));

	if (null_check)
		emit_null_check (builder, base);

	/*
	 * A field's recorded offset counts from the start of the MonoObject. A value type
	 * is reached through its own address and carries no header, so this offset must
	 * have that header removed.
	 */
	int32_t offset = static_cast<int32_t> (m_field_get_offset (field));

	if (m_class_is_valuetype (field->parent))
		offset -= MONO_ABI_SIZEOF (MonoObject);

	return builder.CreateGEP (builder.getInt8Ty (), base, builder.getInt32 (offset));
}

/// The block index and offset a thread static has in the domain this method
/// compiles for, or nothing where this compile cannot read one.
///
/// A shared body serves several instantiations and reaches its fields through the
/// run-time generic context, so there is no one offset to burn in. A context static
/// lives on the MonoAppContext instead of the thread, so its offset addresses the
/// wrong object. Both keep the icall.
std::optional<std::pair<uint32_t, uint32_t> >
MethodLLVMEmitter::thread_static_slot (MonoClassField *field)
{
	guint32 packed;

	if (!thread_static_fast_path () || depends_on_context (field))
		return std::nullopt;

	/*
	 * Creating the declaring class's vtable is what assigns the offset, and this
	 * asks without creating one. A method reaches tier 1 by being interpreted
	 * first, and the interpreter's own transform creates that vtable at the same
	 * site, so what this misses is a site tier 0 never ran - a cold arm, where
	 * the icall costs nothing.
	 */
	if (!mono_special_static_field_offset (cfg->domain, field, &packed))
		return std::nullopt;

	if (ACCESS_SPECIAL_STATIC_OFFSET (packed, type) != SPECIAL_STATIC_OFFSET_TYPE_THREAD)
		return std::nullopt;

	return std::make_pair (
		static_cast<uint32_t> (ACCESS_SPECIAL_STATIC_OFFSET (packed, index)),
		static_cast<uint32_t> (ACCESS_SPECIAL_STATIC_OFFSET (packed, offset)));
}

/// Where field lives in its class's statics block.
///
/// An RVA field lives there too: creating the vtable copies the image data into the
/// block at the field's offset. A thread- or context-local static does not get a fixed
/// offset into that block. thread_static_slot () answers for a thread static this
/// compile can reach off the thread; every special static it refuses takes its address
/// from the runtime on each access.
llvm::Expected<llvm::Value *>
MethodLLVMEmitter::static_field_address (MonoIrBuilder &builder, MonoClassField *field)
{
	if (mono_class_field_is_special_static (field)) {
		if (std::optional<std::pair<uint32_t, uint32_t> > slot = thread_static_slot (field))
			return thread_static_address (builder, slot->first, slot->second);

		llvm::Type *ptr = llvm::PointerType::get (context (), 0);
		llvm::Value *domain = builder.CreateCall (
			module->getOrInsertFunction ("mono_domain_get", ptr));
		llvm::Expected<llvm::Value *> symbol = field_operand (builder, field);

		if (!symbol)
			return symbol.takeError ();

		llvm::Expected<llvm::Function *> lookup =
			icall_wrapper_decl (MONO_JIT_ICALL_mono_class_static_field_address);

		if (!lookup)
			return lookup.takeError ();

		llvm::Value *address = emit_protected_call (
			builder, *lookup,
			adapt_to_callee (builder, *lookup, {domain, *symbol}));

		// The signature says native int. Every caller here wants a pointer.
		if (!address->getType ()->isPointerTy ())
			address = builder.CreateIntToPtr (address, ptr);
		return address;
	}

	llvm::Expected<llvm::Value *> block =
		class_operand (builder, field->parent, "mono_statics_");

	if (!block)
		return block.takeError ();

	// A static's offset is into the block itself, so there is no header to discount.
	return builder.CreateGEP (builder.getInt8Ty (), *block,
	                          builder.getInt32 (m_field_get_offset (field)));
}

/*
 * III.4.10  ldfld - load field of an object
 *
 *   Format     Assembly Format   Description
 *   7B <T>     ldfld field       Push the value of field of object (or value type) obj,
 *                                onto the stack.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., value
 *
 * Description:
 *
 *   The ldfld instruction pushes onto the stack the value of a field of obj. obj shall
 *   be an object (type O), a managed pointer (type &), an unmanaged pointer (type
 *   native int), or an instance of a value type. The use of an unmanaged pointer is not
 *   permitted in verifiable code. field is a metadata token (a fieldref or fielddef see
 *   Partition II) that shall refer to a field member. The return type is that
 *   associated with field. ldfld pops the object reference off the stack and pushes the
 *   value for the field in its place. The field can be either an instance field (in
 *   which case obj shall not be null) or a static field.
 *
 *   The ldfld instruction can be preceded by either or both of the unaligned. and
 *   volatile. prefixes.
 *
 *   If required field values are converted to the representation of their intermediate
 *   type (§I.8.7) when loaded onto the stack (§III.1.1.1).
 *
 *   [Note: That is field values that are smaller than 4 bytes, a boolean or a character
 *   are converted to 4 bytes by sign or zero-extension as appropriate. Floating-point
 *   values are converted to their native size (type F). end note]
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 *   System.NullReferenceException is thrown if obj is null and the field is not static.
 */
llvm::Error
MethodLLVMEmitter::emit_ldfld (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	bool is_static = false;
	llvm::Expected<MonoClassField *> field = resolve_field (token, false, &is_static);
	if (!field)
		return field.takeError ();

	if (is_static) {
		pop_stack (1);
		return emit_ldsfld (builder, token);
	}

	MonoType *ftype = mono_field_get_type_internal (*field);
	StackValue object = get_stack (0);

#ifndef DISABLE_REMOTING
	if (remote_field_access (object, *field)) {
		MonoMethod *wrapper = mono_marshal_get_ldfld_wrapper (ftype);
		llvm::Expected<llvm::Function *> decl = create_method_decl (wrapper);

		if (!decl)
			return decl.takeError ();

		MonoMethodSignature *wsig = mono_method_signature_internal (wrapper);

		push_field_wrapper_operands (builder, *field);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, wsig);

		if (!args)
			return args.takeError ();

		llvm::Value *value = emit_protected_call (builder, *decl, *args);

		pop_stack (4);
		return push_produced (builder, value, wsig->ret);
	}
#endif

	/*
	 * object can also be an instance of a value type (III.4.10). A value type is
	 * not something to dereference: a value class is already the slot that holds
	 * it, and a SIMD value must get a slot before its field even has an address.
	 * Neither case needs a null check, because a value is never null.
	 *
	 * This branch decides on the raw type, not the underlying one. A magic nint
	 * rides the stack as its scalar underlying type. Reading through that
	 * underlying type would treat the struct's own bits as a pointer instead of
	 * a value to address.
	 */
	if (!object.type->byref && MONO_TYPE_ISSTRUCT (object.type)
	    && (held_in_memory (object.type)
	        || !object.value->getType ()->isPointerTy ())) {
		llvm::Value *home = held_in_memory (object.type)
		                            ? object.value
		                            : spill_to_temporary (builder, object.type);
		int32_t offset = static_cast<int32_t> (m_field_get_offset (*field))
		                 - MONO_ABI_SIZEOF (MonoObject);
		llvm::Value *address = builder.CreateGEP (builder.getInt8Ty (), home,
		                                          builder.getInt32 (offset));

		pop_stack (1);
		return push_from_location (builder, address, ftype, /*native=*/false,
		                           ManagedAccess::of_field (*field));
	}

	llvm::Expected<llvm::Value *> address = field_address (builder, object, *field);
	if (!address)
		return address.takeError ();

	ManagedAccess access = field_access (object, *field);

	pop_stack (1);
	return push_from_location (builder, *address, ftype, /*native=*/false, access);
}

/*
 * III.4.11  ldflda - load field address
 *
 *   Format     Assembly Format   Description
 *   7C <T>     ldflda field      Push the address of field of object obj on the stack.
 *
 * Stack Transition:
 *
 *   ..., obj -> ..., address
 *
 * Description:
 *
 *   The ldflda instruction pushes the address of a field of obj. obj is either an
 *   object, type O, a managed pointer, type &, or an unmanaged pointer, type native
 *   int. The use of an unmanaged pointer is not allowed in verifiable code. The value
 *   returned by ldflda is a managed pointer (type &) unless obj is an unmanaged
 *   pointer, in which case it is an unmanaged pointer (type native int).
 *
 *   field is a metadata token (a fieldref or fielddef; see Partition II) that shall
 *   refer to a field member. The field can be either an instance field (in which case
 *   obj shall not be null) or a static field.
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.InvalidOperationException is thrown if the obj is not within the application
 *   domain from which it is being accessed. The address of a field that is not inside
 *   the accessing application domain cannot be loaded.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 *   System.NullReferenceException is thrown if obj is null and the field isn't static.
 *
 * Correctness:
 *
 *   Correct CIL ensures that field is a valid fieldref token and that the type of obj
 *   is compatible-with the Class of field.
 *
 * Verifiability:
 *
 *   For verifiable code, obj shall not be an unmanaged pointer.
 */
llvm::Error
MethodLLVMEmitter::emit_ldflda (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	bool is_static = false;
	llvm::Expected<MonoClassField *> field = resolve_field (token, false, &is_static);
	if (!field)
		return field.takeError ();

	if (is_static) {
		pop_stack (1);
		return emit_ldsflda (builder, token);
	}

	StackValue object = get_stack (0);

#ifndef DISABLE_REMOTING
	if (remote_field_access (object, *field)) {
		MonoMethod *wrapper =
			mono_marshal_get_ldflda_wrapper (mono_field_get_type_internal (*field));
		llvm::Expected<llvm::Function *> decl = create_method_decl (wrapper);

		if (!decl)
			return decl.takeError ();

		push_field_wrapper_operands (builder, *field);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, mono_method_signature_internal (wrapper));

		if (!args)
			return args.takeError ();

		llvm::Value *value = emit_protected_call (builder, *decl, *args);

		if (!value->getType ()->isPointerTy ())
			value = builder.CreateIntToPtr (
				value, llvm::PointerType::get (context (), 0));

		pop_stack (4);
		push_stack (value,
		            m_class_get_this_arg (mono_class_from_mono_type_internal (
				    mono_field_get_type_internal (*field))));
		return llvm::Error::success ();
	}
#endif

	/*
	 * Taking an address dereferences nothing, so this checks only an
	 * object-reference receiver. A native pointer can be null and still compute
	 * an address. The fault waits for whatever dereferences the result later.
	 */
	llvm::Expected<llvm::Value *> address =
		field_address (builder, object, *field,
		               stack_type (object.type) == ObjectRef);
	if (!address)
		return address.takeError ();

	/*
	 * The address of a field inside something the collector does not track must not
	 * start being tracked either, so an unmanaged receiver keeps its own kind.
	 */
	MonoType *ftype = mono_field_get_type_internal (*field);
	MonoType *pushed =
		stack_type (object.type) == NativeInt
			? mono_get_int_type ()
			: m_class_get_this_arg (mono_class_from_mono_type_internal (ftype));

	pop_stack (1);
	if (m_class_is_valuetype (mono_class_from_mono_type_internal (ftype)))
		trusted_byrefs.insert (*address);
	push_stack (*address, pushed);
	return llvm::Error::success ();
}

/*
 * III.4.28  stfld - store into a field of an object
 *
 *   Format     Assembly Format   Description
 *   7D <T>     stfld field       Replace the value of field of the object obj with
 *                                value.
 *
 * Stack Transition:
 *
 *   ..., obj, value -> ...,
 *
 * Description:
 *
 *   The stfld instruction replaces the value of a field of an obj (an O) or via a
 *   pointer (type native int, or &) with value. field is a metadata token (a fieldref
 *   or fielddef; see Partition II) that refers to a field member reference. stfld pops
 *   the value and the object reference off the stack and updates the object.
 *
 *   Storing into fields that hold a value smaller than 4 bytes truncates the value as
 *   it moves from the stack to the local variable. Floating-point values are rounded
 *   from their native size (type F) to the size associated with the argument. (See
 *   §III.1.1.1, Numeric data types.)
 *
 *   The stfld instruction can have a prefix of either or both of unaligned. and
 *   volatile..
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.NullReferenceException is thrown if obj is null and the field isn't static.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that field is a valid token referring to a field, and that obj
 *   and value will always have types appropriate for the assignment being performed,
 *   subject to implicit conversion as specified in §III.1.6.
 */
llvm::Error
MethodLLVMEmitter::emit_stfld (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.size () < 2)
		return unbalanced_stack (2);

	bool is_static = false;
	llvm::Expected<MonoClassField *> field = resolve_field (token, false, &is_static);
	if (!field)
		return field.takeError ();

	if (is_static) {
		StackValue stored = get_stack (0);

		pop_stack (2);
		push_stack (stored.value, stored.type, stored.native);
		return emit_stsfld (builder, token);
	}

	MonoType *ftype = mono_field_get_type_internal (*field);

#ifndef DISABLE_REMOTING
	if (remote_field_access (get_stack (1), *field)) {
		MonoMethod *wrapper = mono_marshal_get_stfld_wrapper (ftype);
		llvm::Expected<llvm::Function *> decl = create_method_decl (wrapper);

		if (!decl)
			return decl.takeError ();

		// The wrapper wants the value after the constants, so it comes off the
		// stack and goes back on top of them.
		StackValue stored = get_stack (0);

		pop_stack (1);
		push_field_wrapper_operands (builder, *field);
		push_stack (stored.value, stored.type, stored.native);

		llvm::Expected<std::vector<llvm::Value *>> args =
			pop_call_arguments (builder, mono_method_signature_internal (wrapper));

		if (!args)
			return args.takeError ();

		emit_protected_call (builder, *decl, *args);
		pop_stack (5);
		return llvm::Error::success ();
	}
#endif

	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), ftype);
	if (!value)
		return value.takeError ();

	StackValue object = get_stack (1);
	llvm::Expected<llvm::Value *> address = field_address (builder, object, *field);
	if (!address)
		return address.takeError ();

	ManagedAccess access = field_access (object, *field);

	pop_stack (2);
	if (llvm::Error stored = emit_memory_store (builder, *value, *address, ftype, access))
		return stored;
	return llvm::Error::success ();
}

/*
 * III.4.14  ldsfld - load static field of a class
 *
 *   Format     Assembly Format   Description
 *   7E <T>     ldsfld field      Push the value of field on the stack.
 *
 * Stack Transition:
 *
 *   ..., -> ..., value
 *
 * Description:
 *
 *   The ldsfld instruction pushes the value of a static (shared among all instances of
 *   a class) field on the stack. field is a metadata token (a fieldref or fielddef; see
 *   Partition II) referring to a static field member. The return type is that
 *   associated with field.
 *
 *   The ldsfld instruction can have a volatile. prefix.
 *
 *   If required field values are converted to the representation of their intermediate
 *   type (§I.8.7) when loaded onto the stack (§III.1.1.1).
 *
 *   [Note: That is field values that are smaller than 4 bytes, a boolean or a character
 *   are converted to 4 bytes by sign or zero-extension as appropriate. Floating-point
 *   values are converted to their native size (type F). end note]
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 */
llvm::Error
MethodLLVMEmitter::emit_ldsfld (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoClassField *> field = resolve_field (token, true);
	if (!field)
		return field.takeError ();

	MonoType *ftype = mono_field_get_type_internal (*field);

	if (llvm::Error error = emit_class_init (builder, (*field)->parent))
		return error;

	llvm::Expected<llvm::Value *> address = static_field_address (builder, *field);
	if (!address)
		return address.takeError ();

	ManagedAccess access = ManagedAccess::of_field (*field);

	access.invariant = invariant_static_read (*field);

	if (llvm::Error error = push_from_location (builder, *address, ftype, /*native=*/false,
	                                            access))
		return error;

	if (MonoClass *held = initonly_static_class (*field))
		if (auto *loaded = llvm::dyn_cast<llvm::Instruction> (get_stack (0).value))
			mark_exact_class (*loaded, held);

	return llvm::Error::success ();
}

/// The class the object in an initonly static field has, or null where this
/// compile cannot read one.
///
/// `initonly` makes the class initializer the only writer that IL has, so the
/// value read once that initializer has run is the value the field keeps. What
/// this states is the class, which stays right while the collector moves the
/// object, so no address is written down.
///
/// Reflection is the writer IL does not have.
/// `mono_field_static_set_value_internal ()` refuses a literal and nothing else,
/// and no compiled body is taken back when a field is written that way. A
/// program that does it reads a stale class here.
MonoClass *
MethodLLVMEmitter::initonly_static_class (MonoClassField *field)
{
	MonoType *type = mono_field_get_type_internal (field);

	if ((mono_field_get_flags (field) & FIELD_ATTRIBUTE_INIT_ONLY) == 0
	    || !MONO_TYPE_IS_REFERENCE (type) || depends_on_context (field))
		return nullptr;

	// A special static lives per thread or per context, so what stands there
	// now says nothing about what a compiled body will read.
	if (field->offset < 0)
		return nullptr;

	ERROR_DECL (vtable_error);
	MonoVTable *vtable =
		mono_class_vtable_checked (cfg->domain, field->parent, vtable_error);

	if (vtable == nullptr) {
		mono_error_cleanup (vtable_error);
		return nullptr;
	}

	/*
	 * The flag goes on once the initializer has run. A body compiled before
	 * that reads whatever the field holds part way through, which the rest of
	 * the initializer is free to replace.
	 */
	if (!vtable->initialized)
		return nullptr;

	auto *held = *(MonoObject **) ((char *) mono_vtable_get_static_field_data (vtable)
	                               + field->offset);

	if (held == nullptr)
		return nullptr;

	MonoClass *klass = mono_object_class (held);

	// A transparent proxy stands in for another class and carries a vtable
	// that is not that class's.
	return mono_class_is_marshalbyref (klass) ? nullptr : klass;
}

/*
 * III.4.15  ldsflda - load static field address
 *
 *   Format     Assembly Format   Description
 *   7F <T>     ldsflda field     Push the address of the static field, field, on the
 *                                stack.
 *
 * Stack Transition:
 *
 *   ..., -> ..., address
 *
 * Description:
 *
 *   The ldsflda instruction pushes the address (a managed pointer, type &, if field
 *   refers to a type whose memory is managed; otherwise an unmanaged pointer, type
 *   native int) of a static field on the stack. field is a metadata token (a fieldref
 *   or fielddef; see Partition II) referring to a static field member. (Note that
 *   field can be a static global with assigned RVA, in which case its memory is
 *   unmanaged; where RVA stands for Relative Virtual Address, the offset of the field
 *   from the base address at which its containing PE file is loaded into memory)
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 *
 * Correctness:
 *
 *   Correct CIL ensures that field is a valid metadata token referring to a static
 *   field member if field refers to a type whose memory is managed.
 *
 * Verifiability:
 *
 *   For verifiable code, field cannot be init-only. If field refers to a type whose
 *   memory is managed, verification (§III.1.8) tracks the type of the value loaded
 *   onto the stack as a managed pointer to the verification type (§I.8.7) of field. If
 *   field refers to a type whose memory is unmanaged, verification (§III.1.8) tracks
 *   the type of the value loaded onto the stack as an unmanaged pointer.
 *
 * Remark:
 *
 *   Using ldsflda to compute the address of a static, init-only field and then using
 *   the resulting pointer to modify that value outside the body of the class
 *   initializer can lead to unpredictable behavior.
 */
llvm::Error
MethodLLVMEmitter::emit_ldsflda (MonoIrBuilder &builder, uint32_t token)
{
	llvm::Expected<MonoClassField *> field = resolve_field (token, true);
	if (!field)
		return field.takeError ();

	/*
	 * Taking the address means the field is about to be touched, so the class must
	 * be initialized here exactly as it is for a direct load.
	 */
	if (llvm::Error error = emit_class_init (builder, (*field)->parent))
		return error;

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Value *> address = static_field_address (builder, *field);

	if (!address)
		return address.takeError ();

	push_stack (*address,
	            m_class_get_this_arg (mono_class_from_mono_type_internal (ftype)));
	return llvm::Error::success ();
}

/*
 * III.4.30  stsfld - store a static field of a class
 *
 *   Format     Assembly Format   Description
 *   80 <T>     stsfld field      Replace the value of field with val.
 *
 * Stack Transition:
 *
 *   ..., val -> ...,
 *
 * Description:
 *
 *   The stsfld instruction replaces the value of a static field with a value from the
 *   stack. field is a metadata token (a fieldref or fielddef; see Partition II) that
 *   shall refer to a static field member. stsfld pops the value off the stack and
 *   updates the static field with that value.
 *
 *   Storing into fields that hold a value smaller than 4 bytes truncates the value as
 *   it moves from the stack to the local variable. Floating-point values are rounded
 *   from their native size (type F) to the size associated with the argument. (See
 *   §III.1.1.1, Numeric data types.)
 *
 *   The stsfld instruction can have a volatile. prefix.
 *
 * Exceptions:
 *
 *   System.FieldAccessException is thrown if field is not accessible.
 *
 *   System.MissingFieldException is thrown if field is not found in the metadata. This
 *   is typically checked when CIL is converted to native code, not at runtime.
 */
llvm::Error
MethodLLVMEmitter::emit_stsfld (MonoIrBuilder &builder, uint32_t token)
{
	if (stack.empty ())
		return unbalanced_stack (1);

	llvm::Expected<MonoClassField *> field = resolve_field (token, true);
	if (!field)
		return field.takeError ();

	MonoType *ftype = mono_field_get_type_internal (*field);
	llvm::Expected<llvm::Value *> value = coerce_to_location (builder, get_stack (0), ftype);
	if (!value)
		return value.takeError ();

	if (llvm::Error error = emit_class_init (builder, (*field)->parent))
		return error;

	llvm::Expected<llvm::Value *> address = static_field_address (builder, *field);

	if (!address)
		return address.takeError ();

	pop_stack (1);
	if (llvm::Error stored =
		    emit_memory_store (builder, *value, *address, ftype,
		                       ManagedAccess::of_field (*field)))
		return stored;
	return llvm::Error::success ();
}

} // namespace mono
