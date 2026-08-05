#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/debug-helpers.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/tokentype.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/ADT/ScopeExit.h>
#include <llvm/Support/ErrorHandling.h>

#include <algorithm>
#include <optional>
#include <string_view>

namespace mono {

namespace {

MonoType *
mono_arg_type (MonoMethod *method, size_t i)
{
	auto sig = method->signature;
	size_t count = sig->param_count + sig->hasthis;

	if (i >= count)
		llvm::report_fatal_error ("mono_arg_type: index was out of bounds ("
		                          + llvm::Twine (i) + " >= " + llvm::Twine (count) + ")");

	if (sig->hasthis && i == 0) {
		return m_class_is_valuetype (method->klass) ? m_class_get_this_arg (method->klass)
		                                            : m_class_get_byval_arg (method->klass);
	}

	return sig->params[i - sig->hasthis];
}

/// The runtime call that builds a corlib exception from its type token and unwinds
/// out of the frame that called it.
llvm::FunctionCallee
throw_corlib_exception_decl (llvm::Module *module)
{
	llvm::LLVMContext &ctx = module->getContext ();
	llvm::FunctionCallee callee =
		module->getOrInsertFunction ("mono_llvm_throw_corlib_exception",
		                             llvm::Type::getVoidTy (ctx),
		                             llvm::Type::getInt32Ty (ctx));

	if (auto *function = llvm::dyn_cast<llvm::Function> (callee.getCallee ())) {
		/* Unwinding is not returning, so this stays free to throw. */
		function->setDoesNotReturn ();
		function->addFnAttr (llvm::Attribute::Cold);
	}

	return callee;
}

} // namespace

llvm::Expected<llvm::Function *>
method_to_llvm (llvm::Module *module, MonoCompile *cfg, MonoMethod *method,
                std::vector<ExternalSymbol> *externals)
{
	auto emitter = MethodLLVMEmitter (module, cfg, method, externals);
	llvm::Expected<llvm::Function *> function = emitter.emit ();

	if (!function)
		return function;

	/* Each filter body rides along as a function of its own. */
	for (uint32_t i = 0; i < cfg->header->num_clauses; ++i) {
		if (cfg->header->clauses[i].flags != MONO_EXCEPTION_CLAUSE_FILTER)
			continue;

		MethodLLVMEmitter filter (module, cfg, method, externals);
		llvm::Expected<llvm::Function *> body = filter.emit_filter (*function, i);

		if (!body)
			return body.takeError ();
	}

	return function;
}

/// How the CLI categorizes T on the evaluation stack.
StackType
MethodLLVMEmitter::stack_type (MonoType *t)
{
	if (t->byref)
		return ManagedPtr;

	t = mini_get_underlying_type (t);

	switch (t->type) {
	/* Anything narrower than four bytes is tracked as int32 once it is pushed. */
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return Int32;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return Int64;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return NativeInt;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		return Float;
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	/* Generic sharing hands us these as references. */
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		return ObjectRef;
	case MONO_TYPE_GENERICINST:
		return mono_type_generic_inst_is_valuetype (t) ? Invalid : ObjectRef;
	default:
		return Invalid;
	}
}

/// VALUE as an operand of TYPE, widening it if the two operands of a binary operation
/// did not arrive as the same thing.
///
/// Only ever a widening: no operand table in III.1.5 pairs an operand with a result
/// narrower than itself.
llvm::Value *
MethodLLVMEmitter::coerce (MonoIrBuilder &builder, llvm::Value *value, llvm::Type *type)
{
	llvm::Type *from = value->getType ();

	if (from == type)
		return value;
	if (type->isFloatingPointTy ())
		return builder.CreateFPExt (value, type);
	/* An unmanaged pointer is tracked as native int but travels as a pointer. */
	if (from->isPointerTy ())
		return builder.CreatePtrToInt (value, type);
	/* int32 paired with native int is sign-extended, never zero-extended. */
	return builder.CreateSExt (value, type);
}

/// VALUE, freshly loaded out of a location of type T, as the CLI tracks it on the stack.
///
/// I.8.7: a location narrower than four bytes reaches the stack as int32, and it is the
/// location's own signedness - not the value's - that decides how the bits it gains get
/// filled.
llvm::Value *
MethodLLVMEmitter::widen_to_stack (MonoIrBuilder &builder, llvm::Value *value, MonoType *t)
{
	if (t->byref)
		return value;

	switch (mini_get_underlying_type (t)->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_I2:
		return builder.CreateSExt (value, builder.getInt32Ty ());
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
		return builder.CreateZExt (value, builder.getInt32Ty ());
	default:
		return value;
	}
}

/// The type a value loaded out of a location of type T is tracked as once it is pushed.
MonoType *
MethodLLVMEmitter::stack_slot_type (MonoType *t)
{
	if (t->byref)
		return t;

	switch (mini_get_underlying_type (t)->type) {
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return mono_get_int32_type ();
	default:
		return t;
	}
}

/// The CLI's name for T's category, or T's own name when it has none.
std::string
MethodLLVMEmitter::describe (MonoType *t, StackType type)
{
	switch (type) {
	case Int32:
		return "int32";
	case Int64:
		return "int64";
	case NativeInt:
		return "native int";
	case Float:
		return "float";
	case ManagedPtr:
		return "managed pointer";
	case ObjectRef:
		return "object reference";
	case Invalid:
		break;
	}

	char *name = mono_type_full_name (t);
	std::string text = name;

	g_free (name);
	return text;
}

namespace {

/// How many bytes of operand OPCODE carries, or none for a switch, whose length is in
/// its own operand.
std::optional<size_t>
operand_size (MonoOpcodeEnum opcode)
{
	switch (mono_opcodes[opcode].argument) {
	case MonoInlineNone:
		return 0;
	case MonoShortInlineVar:
	case MonoShortInlineI:
	case MonoShortInlineBrTarget:
		return 1;
	case MonoInlineVar:
		return 2;
	case MonoInlineI:
	case MonoShortInlineR:
	case MonoInlineBrTarget:
	case MonoInlineType:
	case MonoInlineField:
	case MonoInlineMethod:
	case MonoInlineTok:
	case MonoInlineString:
	case MonoInlineSig:
		return 4;
	case MonoInlineI8:
	case MonoInlineR:
		return 8;
	default:
		return std::nullopt;
	}
}

} // namespace

/// Decode the instruction at AT far enough to say where control goes from it.
llvm::Expected<MethodLLVMEmitter::Flow>
MethodLLVMEmitter::decode_flow (size_t at)
{
	const unsigned char *cursor = code + at;
	Flow flow;

	flow.opcode = mono_opcode_value (&cursor, code + code_size);

	if (flow.opcode == MonoOpcodeEnum_Invalid) {
		offset = at;
		return invalid_il ("unrecognized opcode");
	}

	size_t operand = static_cast<size_t> (cursor - code) + 1;
	std::optional<size_t> size = operand_size (flow.opcode);

	if (size) {
		flow.next = operand + *size;
	} else {
		/* switch: a count, then that many four-byte displacements. */
		if (code_size - operand < 4) {
			offset = at;
			return truncated_il (4);
		}

		uint32_t count = code[operand] | (code[operand + 1] << 8)
		                 | (code[operand + 2] << 16) | (code[operand + 3] << 24);

		flow.next = operand + 4 + static_cast<size_t> (count) * 4;
	}

	if (flow.next > code_size) {
		offset = at;
		return truncated_il (flow.next - code_size);
	}

	/*
	 * Displacements are relative to the instruction after the branch, which is why
	 * the targets are worked out from `next` rather than from `at`.
	 */
	if (mono_opcodes[flow.opcode].argument == MonoShortInlineBrTarget)
		flow.targets.push_back (flow.next + static_cast<int8_t> (code[operand]));
	else if (mono_opcodes[flow.opcode].argument == MonoInlineBrTarget)
		flow.targets.push_back (flow.next
		                        + static_cast<int32_t> (code[operand]
		                                                | (code[operand + 1] << 8)
		                                                | (code[operand + 2] << 16)
		                                                | (code[operand + 3] << 24)));
	else if (!size)
		for (size_t i = operand + 4; i < flow.next; i += 4)
			flow.targets.push_back (
				flow.next
				+ static_cast<int32_t> (code[i] | (code[i + 1] << 8)
				                        | (code[i + 2] << 16)
				                        | (code[i + 3] << 24)));

	return flow;
}

bool
MethodLLVMEmitter::Flow::falls_through () const
{
	/*
	 * mono's flow types are not quite an oracle for this: `break` is filed under
	 * ERROR though a debugger breakpoint comes back, and mono_ldnativeobj under
	 * RETURN though all it does is push a value.
	 */
	if (opcode == MONO_CEE_BREAK || opcode == MONO_CEE_MONO_LDNATIVEOBJ)
		return true;

	switch (mono_opcodes[opcode].flow_type) {
	case MONO_FLOW_BRANCH:
	case MONO_FLOW_RETURN:
	case MONO_FLOW_ERROR:
		return false;
	case MONO_FLOW_CALL:
		/* jmp is the one call control never comes back from. */
		return opcode != MONO_CEE_JMP;
	default:
		return true;
	}
}

/// Find every offset a block starts at and give each one an empty LLVM block.
///
/// A block starts where something branches to it and after anything that does not fall
/// through, which is all this needs to know: what is on the evaluation stack when a
/// block is entered is settled while emitting, by whichever predecessor gets there
/// first.
llvm::Error
MethodLLVMEmitter::find_block_leaders ()
{
	std::vector<size_t> leaders = { 0 };
	size_t at = 0;

	while (at < code_size) {
		llvm::Expected<Flow> flow = decode_flow (at);

		if (!flow)
			return flow.takeError ();

		for (size_t target : flow->targets)
			leaders.push_back (target);

		if (!flow->falls_through ()
		    || mono_opcodes[flow->opcode].flow_type == MONO_FLOW_COND_BRANCH)
			leaders.push_back (flow->next);

		at = flow->next;
	}

	/*
	 * A protected region and each of its handlers are entered from outside the IL's
	 * own control flow, so their boundaries start blocks whether or not anything
	 * branches to them.
	 */
	for (uint32_t i = 0; i < num_clauses; ++i) {
		MonoExceptionClause *clause = &clauses[i];

		leaders.push_back (clause->try_offset);
		leaders.push_back (clause->try_offset + clause->try_len);
		leaders.push_back (clause->handler_offset);
		leaders.push_back (clause->handler_offset + clause->handler_len);

		if (clause->flags == MONO_EXCEPTION_CLAUSE_FILTER)
			leaders.push_back (clause->data.filter_offset);
	}

	for (size_t leader : leaders) {
		if (leader > code_size) {
			offset = 0;
			return invalid_il (llvm::Twine ("branch target IL_")
			                   + llvm::Twine::utohexstr (leader)
			                   + " is outside the method body");
		}

		/* The leader one past the end is the fallthrough of a trailing ret. */
		if (leader == code_size)
			continue;

		Block &block = blocks[leader];
		char name[16];

		if (block.block != nullptr)
			continue;

		g_snprintf (name, sizeof (name), "IL_%04x", static_cast<unsigned> (leader));
		block.block = llvm::BasicBlock::Create (context (), name, function);
	}

	mark_reachable_blocks ();
	return llvm::Error::success ();
}

/// Flag the blocks control can actually get to.
///
/// Unreachable IL is legal and compilers emit it - a `br` after a `leave`, the tail of a
/// protected region after every path out of it has branched. It has no entry stack for
/// the same reason nothing reaches it, so translating it means inventing one, which both
/// mistypes its own body and, through its fallthrough edge, can settle the entry stack of
/// the live block it runs into.
void
MethodLLVMEmitter::mark_reachable_blocks ()
{
	std::vector<size_t> worklist;

	auto reach = [&] (size_t at) {
		auto found = blocks.find (at);

		if (found == blocks.end () || found->second.reachable)
			return;

		found->second.reachable = true;
		worklist.push_back (at);
	};

	reach (0);

	/*
	 * A protected region and its handlers are entered by the runtime rather than by
	 * anything in the IL. The `+ len` boundaries are not roots: they are block starts
	 * only because a region has to end somewhere, and nobody enters them.
	 */
	for (uint32_t i = 0; i < num_clauses; ++i) {
		reach (clauses[i].try_offset);
		reach (clauses[i].handler_offset);

		if (clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER)
			reach (clauses[i].data.filter_offset);
	}

	while (!worklist.empty ()) {
		size_t at = worklist.back ();
		worklist.pop_back ();

		/* Walk the block's instructions to find where it can go from here. */
		while (at < code_size) {
			llvm::Expected<Flow> flow = decode_flow (at);

			if (!flow) {
				/* find_block_leaders already reported anything malformed. */
				llvm::consumeError (flow.takeError ());
				break;
			}

			for (size_t target : flow->targets)
				reach (target);

			if (!flow->falls_through ())
				break;

			at = flow->next;

			/* The next block is a successor; the rest of this one is not. */
			if (blocks.count (at) != 0) {
				reach (at);
				break;
			}
		}
	}
}

/// The offset a branch at the current instruction jumps to.
llvm::Expected<size_t>
MethodLLVMEmitter::branch_target (int32_t displacement)
{
	size_t target = static_cast<size_t> (static_cast<ptrdiff_t> (ip) + displacement);

	if (blocks.find (target) == blocks.end ())
		return invalid_il (llvm::Twine ("branch target IL_")
		                   + llvm::Twine::utohexstr (target)
		                   + " is not the start of an instruction");

	return target;
}

/// The memory a value of TYPE at DEPTH lives in while a branch is taken.
///
/// Slots are shared by everything that spills the same type at the same depth, so a
/// conditional branch writes once for both of its edges. Sharing is safe because every
/// edge into a block stores before it jumps, so a reload always sees the store from the
/// predecessor it actually came from.
llvm::AllocaInst *
MethodLLVMEmitter::spill_slot (size_t depth, llvm::Type *type)
{
	llvm::AllocaInst *&slot = spills[{ depth, type }];

	if (slot == nullptr) {
		MonoIrBuilder entry (entry_block, entry_block->begin ());

		slot = entry.CreateAlloca (type, nullptr,
		                           llvm::Twine ("stack") + llvm::Twine (depth));
	}

	return slot;
}

/// Put the evaluation stack in memory, and say where a block entered from here will
/// find it.
std::vector<MethodLLVMEmitter::Slot>
MethodLLVMEmitter::spill_stack (MonoIrBuilder &builder)
{
	std::vector<Slot> slots;

	for (size_t depth = 0; depth < stack.size (); ++depth) {
		llvm::Value *value = stack[depth].value;

		slots.push_back ({ spill_slot (depth, value->getType ()), stack[depth].type });
		builder.CreateStore (value, slots.back ().alloca);
	}

	return slots;
}

/// Record that TARGET is entered holding SLOTS, which is also where the two edges of a
/// conditional branch are checked against each other: they spill once and enter twice.
llvm::Error
MethodLLVMEmitter::enter_block (MonoIrBuilder &builder, size_t target,
                                const std::vector<Slot> &slots)
{
	Block &block = blocks[target];

	/*
	 * The fallthrough edge of a conditional branch is not checked by branch_target,
	 * so this is where a branch as the last instruction of the method is caught.
	 */
	if (block.block == nullptr)
		return invalid_il ("control falls off the end of the method body");

	if (!block.entry_known) {
		block.entry = slots;
		block.entry_known = true;
		return llvm::Error::success ();
	}

	if (block.entry.size () != slots.size ())
		return invalid_il (llvm::Twine ("the evaluation stack is ")
		                   + llvm::Twine (slots.size ()) + " deep here but IL_"
		                   + llvm::Twine::utohexstr (target) + " is entered with "
		                   + llvm::Twine (block.entry.size ()));

	for (size_t depth = 0; depth < slots.size (); ++depth) {
		if (block.entry[depth].alloca == slots[depth].alloca)
			continue;

		/*
		 * The paths disagree on the representation at this depth. The slot
		 * keeps the type the first path gave it and this edge converts on the
		 * way in - the same direction mini merges, whose interface variables
		 * are typed by the first-reaching path and later edges convert_value
		 * into them. Anything the coercion cannot express - a struct meeting
		 * an integer, say - is a merge the spec does not define either.
		 */
		llvm::Value *current = builder.CreateLoad (
			slots[depth].alloca->getAllocatedType (), slots[depth].alloca);
		llvm::Expected<llvm::Value *> converted = coerce_to_location (
			builder, { current, slots[depth].type }, block.entry[depth].type);

		if (!converted
		    || (*converted)->getType ()
		               != block.entry[depth].alloca->getAllocatedType ()) {
			if (!converted)
				llvm::consumeError (converted.takeError ());
			return invalid_il (llvm::Twine ("the evaluation stack holds a "
			                                "different type at depth ")
			                   + llvm::Twine (depth)
			                   + " than the other paths into IL_"
			                   + llvm::Twine::utohexstr (target) + " leave there");
		}

		builder.CreateStore (*converted, block.entry[depth].alloca);
	}

	return llvm::Error::success ();
}

/// Read back what a predecessor spilled, so the block starts with the stack it expects.
void
MethodLLVMEmitter::reload_stack (MonoIrBuilder &builder, const Block &block)
{
	for (const Slot &slot : block.entry)
		push_stack (builder.CreateLoad (slot.alloca->getAllocatedType (), slot.alloca),
		            slot.type);
}

llvm::Expected<llvm::Function *>
MethodLLVMEmitter::emit ()
{
	auto declr = create_method_decl (method);
	if (!declr)
		return declr.takeError ();

	function = declr.get ();
	code = cfg->header->code;
	code_size = cfg->header->code_size;
	clauses = cfg->header->clauses;
	num_clauses = cfg->header->num_clauses;
	clause_state.resize (num_clauses);

	/*
	 * Every frame needs a description mono's unwinder can walk through,
	 * clauses or not - an exception below it unwinds through it, and so does
	 * a stack scan - so codegen must describe this function even where LLVM
	 * proves it cannot itself unwind.
	 */
	function->setUWTableKind (llvm::UWTableKind::Default);

	/*
	 * An invoke is only well formed on a function with a personality, and mono's is
	 * the hook its own unwinder recognises. Nothing calls it on the managed path -
	 * mono_handle_exception does the search - but LLVM will not emit the LSDA the
	 * unwinder reads without one named here.
	 */
	if (num_clauses > 0) {
		function->setPersonalityFn (llvm::cast<llvm::Constant> (
			module->getOrInsertFunction (
				       "mono_personality",
				       llvm::FunctionType::get (
					       llvm::Type::getInt32Ty (context ()), true))
				.getCallee ()));
		/*
		 * What lets the clause gather tell "every protected call optimized
		 * away" - an empty record, published - from "never had clauses".
		 */
		function->addFnAttr ("mono-has-eh-clauses");
	}

	MonoIrBuilder builder (context ());

	entry_block = llvm::BasicBlock::Create (context (), "entry", function);
	builder.SetInsertPoint (entry_block);

	/*
	 * ByReference<T> is a contract with the JIT rather than code: its IL
	 * bodies just throw, and every JIT is required to substitute the real
	 * semantics (mini's required intrinsics). The struct is one interior
	 * pointer; the ctor stores it, the getter loads it.
	 */
	if (m_class_get_image (method->klass) == mono_get_corlib ()
	    && std::string_view (m_class_get_name_space (method->klass)) == "System"
	    && std::string_view (m_class_get_name (method->klass)) == "ByReference`1") {
		llvm::Align align (TARGET_SIZEOF_VOID_P);
		std::string_view name = method->name;

		if (name == ".ctor") {
			builder.CreateAlignedStore (function->getArg (1),
			                            function->getArg (0), align);
			builder.CreateRetVoid ();
			return function;
		}
		if (name == "get_Value") {
			builder.CreateRet (builder.CreateAlignedLoad (
				llvm::PointerType::get (context (), 0),
				function->getArg (0), align));
			return function;
		}
		return unsupported_il ("an unrecognized ByReference member");
	}

	/*
	 * Before anything that can call out: a stack walk entered below this
	 * frame has to find the chain already linked.
	 */
	if (method->save_lmf)
		if (auto error = emit_push_lmf (builder))
			return std::move (error);

	if (auto error = emit_arg_allocas (builder))
		return std::move (error);
	if (auto error = emit_local_allocas (builder))
		return std::move (error);

	/*
	 * A filter body runs as a function of its own against this frame, and
	 * llvm.localrecover is how it reaches the arguments and locals: escaping
	 * them here is what pins each to a frame offset the filter can recompute
	 * from the frame pointer. Order is the recovery index: arguments first,
	 * then locals.
	 */
	bool has_filters = false;

	for (uint32_t i = 0; i < num_clauses; ++i)
		has_filters |= clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER;
	if (has_filters) {
		std::vector<llvm::Value *> escaped;

		for (const Entry &arg : args)
			escaped.push_back (arg.alloca);
		for (const Entry &local : locals)
			escaped.push_back (local.alloca);
		builder.CreateIntrinsic (llvm::Intrinsic::localescape, {}, escaped);
		function->addFnAttr ("frame-pointer", "all");
	}

	if (auto error = find_block_leaders ())
		return std::move (error);

	/*
	 * A finally is entered from its own leaves as well as by unwinding, so it needs
	 * somewhere to record which is in progress before it is jumped to, and a byte for
	 * a thread abort that arrives while it is running to be held in until it is done.
	 */
	for (uint32_t i = 0; i < num_clauses; ++i) {
		if (clauses[i].flags != MONO_EXCEPTION_CLAUSE_FINALLY)
			continue;

		clause_state[i].resume_at =
			builder.CreateAlloca (builder.getInt32Ty (), nullptr,
			                      llvm::Twine ("resume_at") + llvm::Twine (i));
		clause_state[i].abort_guard =
			builder.CreateAlloca (builder.getInt8Ty (), nullptr,
			                      llvm::Twine ("abort_guard") + llvm::Twine (i));
	}

	/*
	 * A handler is entered by the runtime rather than by anything in the IL, so what
	 * it starts holding is settled here rather than by a predecessor: a catch or a
	 * filter is handed the exception, a finally or a fault nothing at all.
	 */
	for (uint32_t i = 0; i < num_clauses; ++i) {
		MonoExceptionClause *clause = &clauses[i];
		std::vector<Slot> entry;

		if (clause->flags == MONO_EXCEPTION_CLAUSE_NONE
		    || clause->flags == MONO_EXCEPTION_CLAUSE_FILTER)
			entry.push_back ({ spill_slot (0, llvm::PointerType::get (context (), 0)),
			                   mono_get_object_type () });

		if (auto error = enter_block (builder, clause->handler_offset, entry))
			return std::move (error);
	}

	/*
	 * The type initializer runs before the first entry into any of the class's
	 * methods (ECMA-335 II.10.5.3), and the method's own entry is the one
	 * point every way of reaching it funnels through - stubs, vtable slots,
	 * delegates, calli - so the check lives here rather than at call sites.
	 *
	 * A native-to-managed wrapper is the exception. It is entered from C on a
	 * thread that may not be attached yet, and attaching it is the first thing
	 * its body does; anything running before that - the class initializer's own
	 * icall included - reads a null LMF address off a thread that has none. The
	 * wrapper is not one of the class's methods anyway, only glue in front of
	 * one, and the method it goes on to call runs the initializer at its own
	 * entry, by which point the thread is attached.
	 */
	if (method->wrapper_type != MONO_WRAPPER_NATIVE_TO_MANAGED
	    && mono_class_needs_cctor_run (method->klass, method))
		if (auto error = emit_class_init (builder, method->klass))
			return std::move (error);

	if (blocks[0].block == nullptr)
		return invalid_il ("the method has no body");

	builder.CreateBr (blocks[0].block);
	blocks[0].entry_known = true;
	builder.SetInsertPoint (blocks[0].block);

	if (auto error = translate_range (builder, 0, code_size))
		return std::move (error);

	resolve_finally_switches ();
	finish_function ();
	return function;
}

/// Translate the IL in [BEGIN, END), leaving the builder wherever the last
/// instruction did.
llvm::Error
MethodLLVMEmitter::translate_range (MonoIrBuilder &builder, size_t begin, size_t end)
{
	ip = begin;

	while (ip < end) {
		/*
		 * A filter body belongs to a function of its own; nothing in this
		 * one reaches it, so its range is not translated here.
		 */
		if (!filter_mode) {
			bool skipped = false;

			for (uint32_t i = 0; i < num_clauses; ++i)
				if (clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER
				    && ip >= clauses[i].data.filter_offset
				    && ip < clauses[i].handler_offset) {
					ip = clauses[i].handler_offset;
					skipped = true;
				}
			if (skipped)
				continue;
		}

		offset = ip;

		if (auto found = blocks.find (ip); found != blocks.end () && ip != begin) {
			Block &next = found->second;

			/*
			 * Nothing gets here, so there is no entry stack to translate the
			 * body against - skip to the next block something does reach and
			 * let finish_function leave this one as `unreachable`. Whatever
			 * came before cannot have fallen in, or this block would be
			 * reachable, so no edge is being dropped.
			 */
			if (!next.reachable) {
				while (ip < end) {
					llvm::Expected<Flow> flow = decode_flow (ip);

					if (!flow)
						return flow.takeError ();

					ip = flow->next;

					if (auto live = blocks.find (ip);
					    live != blocks.end () && live->second.reachable)
						break;
				}

				continue;
			}

			/*
			 * Falling into a block is an edge like any other, so the stack goes
			 * through memory here too rather than staying in the values the
			 * previous block happened to leave behind.
			 */
			if (builder.GetInsertBlock ()->getTerminator () == nullptr) {
				if (auto error = enter_block (builder, ip, spill_stack (builder)))
					return std::move (error);

				builder.CreateBr (next.block);
			}

			pop_stack (stack.size ());
			builder.SetInsertPoint (next.block);
			reload_stack (builder, next);

			/*
			 * Entering a catch or filter handler is the one moment the caught
			 * exception is reliably in hand, so it is remembered here for the
			 * rethrow that may want it after the body has emptied the stack.
			 */
			for (uint32_t i = 0; i < num_clauses; ++i) {
				if (clauses[i].handler_offset != ip)
					continue;

				if (!stack.empty ()
				    && (clauses[i].flags == MONO_EXCEPTION_CLAUSE_NONE
				        || clauses[i].flags == MONO_EXCEPTION_CLAUSE_FILTER))
					clause_state[i].caught = stack.front ().value;

				/*
				 * Every way into a finally lands here, so this is where its
				 * body starts as far as a thread abort is concerned.
				 */
				if (clauses[i].flags == MONO_EXCEPTION_CLAUSE_FINALLY)
					emit_finally_body_marker (builder, i, /* opening */ true);
			}
		} else if (builder.GetInsertBlock ()->getTerminator () != nullptr) {
			return invalid_il ("unreachable instruction is not the start of a block");
		}

		if (llvm::Error error = emit_instruction (builder))
			return std::move (error);
	}

	if (builder.GetInsertBlock ()->getTerminator () == nullptr)
		return invalid_il ("method body ends without returning");

	return llvm::Error::success ();
}

/// The filter body of PARENT's clause CLAUSE_INDEX, as a function of its own.
///
/// The runtime's search pass calls it through call_filter with the parent
/// frame's registers restored: the exception arrives in RAX, the chained frame
/// pointer is the parent's frame, and the answer - match or keep searching -
/// is returned like any int. The parent escaped its arguments and locals
/// (llvm.localescape, same order as here), so llvm.localrecover turns the
/// parent frame pointer back into their addresses.
llvm::Expected<llvm::Function *>
MethodLLVMEmitter::emit_filter (llvm::Function *parent, uint32_t clause_index)
{
	code = cfg->header->code;
	code_size = cfg->header->code_size;
	clauses = cfg->header->clauses;
	num_clauses = cfg->header->num_clauses;
	clause_state.resize (num_clauses);
	filter_mode = true;

	size_t begin = clauses[clause_index].data.filter_offset;
	size_t end = clauses[clause_index].handler_offset;

	function = llvm::Function::Create (
		llvm::FunctionType::get (llvm::Type::getInt32Ty (context ()), false),
		llvm::GlobalValue::ExternalLinkage,
		parent->getName () + "$filter" + llvm::Twine (clause_index), module);
	/* The chained frame pointer is how the parent frame is found. */
	function->addFnAttr ("frame-pointer", "all");
	/*
	 * call_filter enters with the stack 16-aligned - the opposite parity from
	 * a SysV call - so the frame realigns itself or every callee inherits the
	 * wrong parity.
	 */
	function->addFnAttr ("stackrealign");

	MonoIrBuilder builder (context ());

	entry_block = llvm::BasicBlock::Create (context (), "entry", function);
	builder.SetInsertPoint (entry_block);

	llvm::Type *ptr = llvm::PointerType::get (context (), 0);
	llvm::Value *exc = arch::emit_entered_exception (builder);
	llvm::Value *frame = builder.CreateIntrinsic (
		ptr, llvm::Intrinsic::frameaddress, { builder.getInt32 (1) });

	auto recover = [&] (unsigned index) {
		return builder.CreateIntrinsic (
			llvm::Intrinsic::localrecover, {},
			{ parent, frame, builder.getInt32 (static_cast<int32_t> (index)) });
	};
	auto sig = method->signature;
	unsigned nargs = sig->param_count + sig->hasthis;
	unsigned index = 0;

	for (unsigned i = 0; i < nargs; ++i)
		args.push_back ({ recover (index++), mono_arg_type (method, i), sig->pinvoke != 0 });
	for (size_t i = 0; i < cfg->header->num_locals; ++i)
		locals.push_back ({ recover (index++), cfg->header->locals[i] });

	if (auto error = find_block_leaders ())
		return std::move (error);

	/* Entered like a handler: the exception is the whole evaluation stack. */
	llvm::AllocaInst *exc_slot = spill_slot (0, ptr);

	builder.CreateAlignedStore (exc, exc_slot, exc_slot->getAlign ());
	if (auto error = enter_block (builder, begin,
	                              { { exc_slot, mono_get_object_type () } }))
		return std::move (error);

	Block &first = blocks[begin];

	builder.CreateBr (first.block);
	builder.SetInsertPoint (first.block);
	reload_stack (builder, first);

	if (auto error = translate_range (builder, begin, end))
		return std::move (error);

	finish_function ();
	return function;
}

/// The function-wide sweeps every translation ends with.
void
MethodLLVMEmitter::finish_function ()
{
	/*
	 * A block nothing reached still has to be well formed for the verifier, and the
	 * only honest thing to put in one is that control never gets here.
	 */
	for (auto &entry : blocks)
		if (entry.second.block->empty ())
			MonoIrBuilder (entry.second.block).CreateUnreachable ();

	/*
	 * A call site must repeat its callee's calling convention or the pair is
	 * undefined; settling it once here covers every emitter without each
	 * having to remember. Only the direct fastcc calls need it - everything
	 * else (helpers, intrinsics, legacy boundaries) stays at the C default.
	 */
	for (llvm::BasicBlock &bb : *function)
		for (llvm::Instruction &instruction : bb)
			if (auto *call = llvm::dyn_cast<llvm::CallBase> (&instruction))
				if (auto *callee = call->getCalledFunction ();
				    callee != nullptr
				    && callee->getCallingConv () == llvm::CallingConv::Fast)
					call->setCallingConv (llvm::CallingConv::Fast);
}

/// Translate the instruction at OFFSET, leaving IP on the one after it.
llvm::Error
MethodLLVMEmitter::emit_instruction (MonoIrBuilder &builder)
{
	const unsigned char *cursor = code + ip;
	MonoOpcodeEnum opcode = mono_opcode_value (&cursor, code + code_size);

	if (opcode == MonoOpcodeEnum_Invalid)
		return invalid_il ("unrecognized opcode");

	/* mono_opcode_value leaves the cursor on the opcode's last byte, not past it. */
	ip = static_cast<size_t> (cursor - code) + 1;

	/*
	 * The operand is decoded here rather than in the emitters: mono's opcode table
	 * says how wide it is, and an instruction that takes one takes nothing else. It
	 * stays raw little-endian bits until a case below says what they mean - an index,
	 * a signed constant, an IEC 60559 float. Instructions that carry their operand in
	 * the opcode itself read nothing and pass their own constant.
	 */
	uint64_t operand = 0;

	switch (mono_opcodes[opcode].argument) {
	case MonoShortInlineVar:
	case MonoShortInlineI:
	case MonoShortInlineBrTarget: {
		llvm::Expected<uint8_t> read = read_u8 ();

		if (!read)
			return read.takeError ();

		operand = *read;
		break;
	}
	case MonoInlineVar: {
		llvm::Expected<uint16_t> read = read_u16 ();

		if (!read)
			return read.takeError ();

		operand = *read;
		break;
	}
	case MonoInlineI:
	case MonoShortInlineR:
	case MonoInlineBrTarget:
	case MonoInlineField:
	case MonoInlineType:
	case MonoInlineMethod:
	case MonoInlineString:
	case MonoInlineTok:
	case MonoInlineSig: {
		llvm::Expected<uint32_t> read = read_u32 ();

		if (!read)
			return read.takeError ();

		operand = *read;
		break;
	}
	case MonoInlineI8:
	case MonoInlineR: {
		llvm::Expected<uint64_t> read = read_u64 ();

		if (!read)
			return read.takeError ();

		operand = *read;
		break;
	}
	default:
		break;
	}

	/*
	 * A branch displacement is signed and is counted from the instruction after the
	 * branch, which `ip` now points at. Only the branch cases below read this.
	 */
	int32_t displacement = mono_opcodes[opcode].argument == MonoShortInlineBrTarget
	                               ? static_cast<int8_t> (operand)
	                               : static_cast<int32_t> (operand);

	/* Prefixes accumulate; anything else consumes whatever is pending and clears it. */
	switch (opcode) {
	case MONO_CEE_VOLATILE_:
	case MONO_CEE_UNALIGNED_:
	case MONO_CEE_CONSTRAINED_:
	case MONO_CEE_TAIL_:
	case MONO_CEE_READONLY_:
	case MONO_CEE_NO_:
		return emit_prefix (opcode, operand);
	default:
		break;
	}

	llvm::scope_exit consumed ([this] { prefixes = Prefixes {}; });

	switch (opcode) {
	case MONO_CEE_NOP:
		return llvm::Error::success ();
	case MONO_CEE_BREAK:
		return emit_break (builder);
	case MONO_CEE_RET:
		return emit_ret (builder);
	case MONO_CEE_LOCALLOC:
		return emit_localloc (builder);
	case MONO_CEE_SIZEOF:
		return emit_sizeof (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_CKFINITE:
		return emit_ckfinite (builder);

	case MONO_CEE_LDC_I4:
		return emit_ldc_i4 (builder, static_cast<int32_t> (operand));
	/* The short form is signed, so -128..-1 arrive as 0x80..0xFF. */
	case MONO_CEE_LDC_I4_S:
		return emit_ldc_i4 (builder, static_cast<int8_t> (operand));
	case MONO_CEE_LDC_I4_M1:
		return emit_ldc_i4 (builder, -1);
	case MONO_CEE_LDC_I4_0:
		return emit_ldc_i4 (builder, 0);
	case MONO_CEE_LDC_I4_1:
		return emit_ldc_i4 (builder, 1);
	case MONO_CEE_LDC_I4_2:
		return emit_ldc_i4 (builder, 2);
	case MONO_CEE_LDC_I4_3:
		return emit_ldc_i4 (builder, 3);
	case MONO_CEE_LDC_I4_4:
		return emit_ldc_i4 (builder, 4);
	case MONO_CEE_LDC_I4_5:
		return emit_ldc_i4 (builder, 5);
	case MONO_CEE_LDC_I4_6:
		return emit_ldc_i4 (builder, 6);
	case MONO_CEE_LDC_I4_7:
		return emit_ldc_i4 (builder, 7);
	case MONO_CEE_LDC_I4_8:
		return emit_ldc_i4 (builder, 8);
	case MONO_CEE_LDC_I8:
		return emit_ldc_i8 (builder, static_cast<int64_t> (operand));
	case MONO_CEE_LDC_R4:
		return emit_ldc_r4 (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDC_R8:
		return emit_ldc_r8 (builder, operand);
	case MONO_CEE_LDNULL:
		return emit_ldnull (builder);
	case MONO_CEE_LDSTR:
		return emit_ldstr (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDTOKEN:
		return emit_ldtoken (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_DUP:
		return emit_dup ();
	case MONO_CEE_POP:
		return emit_pop ();

	case MONO_CEE_LDFLD:
		return emit_ldfld (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_CALL:
		return emit_call (builder, static_cast<uint32_t> (operand), false);
	case MONO_CEE_CALLVIRT:
		return emit_call (builder, static_cast<uint32_t> (operand), true);
	case MONO_CEE_JMP:
		return emit_jmp (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_NEWARR:
		return emit_newarr (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDLEN:
		return emit_ldlen (builder);
	case MONO_CEE_LDELEMA:
		return emit_ldelema (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_LDELEM_I1:
	case MONO_CEE_LDELEM_U1:
	case MONO_CEE_LDELEM_I2:
	case MONO_CEE_LDELEM_U2:
	case MONO_CEE_LDELEM_I4:
	case MONO_CEE_LDELEM_U4:
	case MONO_CEE_LDELEM_I8:
	case MONO_CEE_LDELEM_I:
	case MONO_CEE_LDELEM_R4:
	case MONO_CEE_LDELEM_R8:
	case MONO_CEE_LDELEM_REF:
		return emit_ldelem (builder, builtin_element_type (opcode));
	case MONO_CEE_LDELEM: {
		llvm::Expected<MonoType *> element =
			element_type_from_token (static_cast<uint32_t> (operand));

		if (!element)
			return element.takeError ();
		return emit_ldelem (builder, *element);
	}

	case MONO_CEE_STELEM_I1:
	case MONO_CEE_STELEM_I2:
	case MONO_CEE_STELEM_I4:
	case MONO_CEE_STELEM_I8:
	case MONO_CEE_STELEM_I:
	case MONO_CEE_STELEM_R4:
	case MONO_CEE_STELEM_R8:
	case MONO_CEE_STELEM_REF:
		return emit_stelem (builder, builtin_element_type (opcode));
	case MONO_CEE_STELEM: {
		llvm::Expected<MonoType *> element =
			element_type_from_token (static_cast<uint32_t> (operand));

		if (!element)
			return element.takeError ();
		return emit_stelem (builder, *element);
	}

	case MONO_CEE_LDIND_I1:
	case MONO_CEE_LDIND_U1:
	case MONO_CEE_LDIND_I2:
	case MONO_CEE_LDIND_U2:
	case MONO_CEE_LDIND_I4:
	case MONO_CEE_LDIND_U4:
	case MONO_CEE_LDIND_I8:
	case MONO_CEE_LDIND_I:
	case MONO_CEE_LDIND_R4:
	case MONO_CEE_LDIND_R8:
	case MONO_CEE_LDIND_REF:
		return emit_ldind (builder, builtin_element_type (opcode));
	case MONO_CEE_STIND_REF:
	case MONO_CEE_STIND_I1:
	case MONO_CEE_STIND_I2:
	case MONO_CEE_STIND_I4:
	case MONO_CEE_STIND_I8:
	case MONO_CEE_STIND_R4:
	case MONO_CEE_STIND_R8:
	case MONO_CEE_STIND_I:
		return emit_stind (builder, builtin_element_type (opcode));

	case MONO_CEE_LDOBJ:
		return emit_ldobj (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_STOBJ:
		return emit_stobj (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_CPOBJ:
		return emit_cpobj (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_INITOBJ:
		return emit_initobj (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_CPBLK:
		return emit_cpblk (builder);
	case MONO_CEE_INITBLK:
		return emit_initblk (builder);

	case MONO_CEE_BOX:
		return emit_box (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_UNBOX:
		return emit_unbox (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_UNBOX_ANY:
		return emit_unbox_any (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_NEWOBJ:
		return emit_newobj (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDFTN:
		return emit_ldftn (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDVIRTFTN:
		return emit_ldvirtftn (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_CALLI:
		return emit_calli (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_CASTCLASS:
		return emit_castclass (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_ISINST:
		return emit_isinst (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_MKREFANY:
		return emit_mkrefany (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_REFANYVAL:
		return emit_refanyval (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_REFANYTYPE:
		return emit_refanytype (builder);

	case MONO_CEE_LDSFLD:
		return emit_ldsfld (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDSFLDA:
		return emit_ldsflda (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_STSFLD:
		return emit_stsfld (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_LDFLDA:
		return emit_ldflda (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_STFLD:
		return emit_stfld (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_LDARG:
	case MONO_CEE_LDARG_S:
		return emit_ldarg (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDARG_0:
		return emit_ldarg (builder, 0);
	case MONO_CEE_LDARG_1:
		return emit_ldarg (builder, 1);
	case MONO_CEE_LDARG_2:
		return emit_ldarg (builder, 2);
	case MONO_CEE_LDARG_3:
		return emit_ldarg (builder, 3);

	case MONO_CEE_LDARGA:
	case MONO_CEE_LDARGA_S:
		return emit_ldarga (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_STARG:
	case MONO_CEE_STARG_S:
		return emit_starg (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_LDLOC:
	case MONO_CEE_LDLOC_S:
		return emit_ldloc (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_LDLOC_0:
		return emit_ldloc (builder, 0);
	case MONO_CEE_LDLOC_1:
		return emit_ldloc (builder, 1);
	case MONO_CEE_LDLOC_2:
		return emit_ldloc (builder, 2);
	case MONO_CEE_LDLOC_3:
		return emit_ldloc (builder, 3);

	case MONO_CEE_LDLOCA:
	case MONO_CEE_LDLOCA_S:
		return emit_ldloca (builder, static_cast<uint32_t> (operand));

	case MONO_CEE_STLOC:
	case MONO_CEE_STLOC_S:
		return emit_stloc (builder, static_cast<uint32_t> (operand));
	case MONO_CEE_STLOC_0:
		return emit_stloc (builder, 0);
	case MONO_CEE_STLOC_1:
		return emit_stloc (builder, 1);
	case MONO_CEE_STLOC_2:
		return emit_stloc (builder, 2);
	case MONO_CEE_STLOC_3:
		return emit_stloc (builder, 3);

	case MONO_CEE_ADD:
		return emit_add (builder);
	case MONO_CEE_SUB:
		return emit_sub (builder);
	case MONO_CEE_MUL:
		return emit_mul (builder);
	case MONO_CEE_DIV:
		return emit_div (builder);
	case MONO_CEE_REM:
		return emit_rem (builder);
	case MONO_CEE_DIV_UN:
		return emit_div_un (builder);
	case MONO_CEE_REM_UN:
		return emit_rem_un (builder);
	case MONO_CEE_NEG:
		return emit_neg (builder);

	case MONO_CEE_ADD_OVF:
		return emit_add_ovf (builder, false);
	case MONO_CEE_ADD_OVF_UN:
		return emit_add_ovf (builder, true);
	case MONO_CEE_MUL_OVF:
		return emit_mul_ovf (builder, false);
	case MONO_CEE_MUL_OVF_UN:
		return emit_mul_ovf (builder, true);
	case MONO_CEE_SUB_OVF:
		return emit_sub_ovf (builder, false);
	case MONO_CEE_SUB_OVF_UN:
		return emit_sub_ovf (builder, true);

	case MONO_CEE_BR:
	case MONO_CEE_BR_S:
		return emit_br (builder, displacement);
	case MONO_CEE_LEAVE:
	case MONO_CEE_LEAVE_S:
		return emit_leave (builder, displacement);
	case MONO_CEE_ENDFINALLY:
		return emit_endfinally (builder);
	case MONO_CEE_ENDFILTER:
		return emit_endfilter (builder);
	case MONO_CEE_THROW:
		return emit_throw (builder);
	case MONO_CEE_RETHROW:
		return emit_rethrow (builder);
	case MONO_CEE_BRTRUE:
	case MONO_CEE_BRTRUE_S:
		return emit_brcond (builder, displacement, true);
	case MONO_CEE_BRFALSE:
	case MONO_CEE_BRFALSE_S:
		return emit_brcond (builder, displacement, false);
	case MONO_CEE_SWITCH:
		return emit_switch (builder);

	case MONO_CEE_BEQ:
	case MONO_CEE_BEQ_S:
		return emit_branch_compare (builder, BinaryOp::Beq, displacement);
	case MONO_CEE_BGE:
	case MONO_CEE_BGE_S:
		return emit_branch_compare (builder, BinaryOp::Bge, displacement);
	case MONO_CEE_BGT:
	case MONO_CEE_BGT_S:
		return emit_branch_compare (builder, BinaryOp::Bgt, displacement);
	case MONO_CEE_BLE:
	case MONO_CEE_BLE_S:
		return emit_branch_compare (builder, BinaryOp::Ble, displacement);
	case MONO_CEE_BLT:
	case MONO_CEE_BLT_S:
		return emit_branch_compare (builder, BinaryOp::Blt, displacement);
	case MONO_CEE_BNE_UN:
	case MONO_CEE_BNE_UN_S:
		return emit_branch_compare (builder, BinaryOp::BneUn, displacement);
	case MONO_CEE_BGE_UN:
	case MONO_CEE_BGE_UN_S:
		return emit_branch_compare (builder, BinaryOp::BgeUn, displacement);
	case MONO_CEE_BGT_UN:
	case MONO_CEE_BGT_UN_S:
		return emit_branch_compare (builder, BinaryOp::BgtUn, displacement);
	case MONO_CEE_BLE_UN:
	case MONO_CEE_BLE_UN_S:
		return emit_branch_compare (builder, BinaryOp::BleUn, displacement);
	case MONO_CEE_BLT_UN:
	case MONO_CEE_BLT_UN_S:
		return emit_branch_compare (builder, BinaryOp::BltUn, displacement);

	case MONO_CEE_CEQ:
		return emit_compare (builder, BinaryOp::Beq);
	case MONO_CEE_CGT:
		return emit_compare (builder, BinaryOp::Bgt);
	case MONO_CEE_CGT_UN:
		return emit_compare (builder, BinaryOp::BgtUn);
	case MONO_CEE_CLT:
		return emit_compare (builder, BinaryOp::Blt);
	case MONO_CEE_CLT_UN:
		return emit_compare (builder, BinaryOp::BltUn);

	case MONO_CEE_AND:
		return emit_and (builder);
	case MONO_CEE_OR:
		return emit_or (builder);
	case MONO_CEE_XOR:
		return emit_xor (builder);
	case MONO_CEE_NOT:
		return emit_not (builder);
	case MONO_CEE_SHL:
		return emit_shift (builder, BinaryOp::Shl);
	case MONO_CEE_SHR:
		return emit_shift (builder, BinaryOp::Shr);
	case MONO_CEE_SHR_UN:
		return emit_shift (builder, BinaryOp::ShrUn);

	case MONO_CEE_CONV_I1:
		return emit_conv (builder, ConvType::I1);
	case MONO_CEE_CONV_U1:
		return emit_conv (builder, ConvType::U1);
	case MONO_CEE_CONV_I2:
		return emit_conv (builder, ConvType::I2);
	case MONO_CEE_CONV_U2:
		return emit_conv (builder, ConvType::U2);
	case MONO_CEE_CONV_I4:
		return emit_conv (builder, ConvType::I4);
	case MONO_CEE_CONV_U4:
		return emit_conv (builder, ConvType::U4);
	case MONO_CEE_CONV_I8:
		return emit_conv (builder, ConvType::I8);
	case MONO_CEE_CONV_U8:
		return emit_conv (builder, ConvType::U8);
	case MONO_CEE_CONV_I:
		return emit_conv (builder, ConvType::I);
	case MONO_CEE_CONV_U:
		return emit_conv (builder, ConvType::U);
	case MONO_CEE_CONV_R4:
		return emit_conv (builder, ConvType::R4);
	case MONO_CEE_CONV_R8:
		return emit_conv (builder, ConvType::R8);
	case MONO_CEE_CONV_R_UN:
		return emit_conv_r_un (builder);

	case MONO_CEE_CONV_OVF_I1:
		return emit_conv_ovf (builder, ConvType::I1, false);
	case MONO_CEE_CONV_OVF_U1:
		return emit_conv_ovf (builder, ConvType::U1, false);
	case MONO_CEE_CONV_OVF_I2:
		return emit_conv_ovf (builder, ConvType::I2, false);
	case MONO_CEE_CONV_OVF_U2:
		return emit_conv_ovf (builder, ConvType::U2, false);
	case MONO_CEE_CONV_OVF_I4:
		return emit_conv_ovf (builder, ConvType::I4, false);
	case MONO_CEE_CONV_OVF_U4:
		return emit_conv_ovf (builder, ConvType::U4, false);
	case MONO_CEE_CONV_OVF_I8:
		return emit_conv_ovf (builder, ConvType::I8, false);
	case MONO_CEE_CONV_OVF_U8:
		return emit_conv_ovf (builder, ConvType::U8, false);
	case MONO_CEE_CONV_OVF_I:
		return emit_conv_ovf (builder, ConvType::I, false);
	case MONO_CEE_CONV_OVF_U:
		return emit_conv_ovf (builder, ConvType::U, false);

	case MONO_CEE_CONV_OVF_I1_UN:
		return emit_conv_ovf (builder, ConvType::I1, true);
	case MONO_CEE_CONV_OVF_U1_UN:
		return emit_conv_ovf (builder, ConvType::U1, true);
	case MONO_CEE_CONV_OVF_I2_UN:
		return emit_conv_ovf (builder, ConvType::I2, true);
	case MONO_CEE_CONV_OVF_U2_UN:
		return emit_conv_ovf (builder, ConvType::U2, true);
	case MONO_CEE_CONV_OVF_I4_UN:
		return emit_conv_ovf (builder, ConvType::I4, true);
	case MONO_CEE_CONV_OVF_U4_UN:
		return emit_conv_ovf (builder, ConvType::U4, true);
	case MONO_CEE_CONV_OVF_I8_UN:
		return emit_conv_ovf (builder, ConvType::I8, true);
	case MONO_CEE_CONV_OVF_U8_UN:
		return emit_conv_ovf (builder, ConvType::U8, true);
	case MONO_CEE_CONV_OVF_I_UN:
		return emit_conv_ovf (builder, ConvType::I, true);
	case MONO_CEE_CONV_OVF_U_UN:
		return emit_conv_ovf (builder, ConvType::U, true);

	/*
	 * The runtime's own opcodes, which only ever appear in a body it generated.
	 * A method loaded from metadata carrying one is not IL at all.
	 */
	case MONO_CEE_MONO_ICALL:
	case MONO_CEE_MONO_LDPTR:
	case MONO_CEE_MONO_LDDOMAIN:
	case MONO_CEE_MONO_CLASSCONST:
	case MONO_CEE_MONO_METHODCONST:
	case MONO_CEE_MONO_NOT_TAKEN:
	case MONO_CEE_MONO_OBJADDR:
	case MONO_CEE_MONO_VTADDR:
	case MONO_CEE_MONO_RETHROW:
	case MONO_CEE_MONO_NEWOBJ:
	case MONO_CEE_MONO_LDNATIVEOBJ:
	case MONO_CEE_MONO_RETOBJ:
	case MONO_CEE_MONO_LDPTR_INT_REQ_FLAG:
	case MONO_CEE_MONO_JIT_ICALL_ADDR:
	case MONO_CEE_MONO_ICALL_ADDR:
	case MONO_CEE_MONO_TLS:
	case MONO_CEE_MONO_ATOMIC_STORE_I4:
	case MONO_CEE_MONO_LD_DELEGATE_METHOD_PTR:
	case MONO_CEE_MONO_CALLI_EXTRA_ARG:
	case MONO_CEE_MONO_SAVE_LAST_ERROR:
	case MONO_CEE_MONO_SAVE_LMF:
	case MONO_CEE_MONO_RESTORE_LMF:
		if (!in_wrapper ())
			return invalid_il (llvm::Twine (mono_opcode_name (opcode))
			                   + " outside a wrapper");

		switch (opcode) {
		case MONO_CEE_MONO_ICALL:
			return emit_mono_icall (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_LDPTR:
			return emit_mono_ldptr (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_LDDOMAIN:
			return emit_mono_lddomain (builder);
		case MONO_CEE_MONO_CLASSCONST:
			return emit_mono_classconst (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_METHODCONST:
			return emit_mono_methodconst (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_OBJADDR:
			return emit_mono_objaddr (builder);
		case MONO_CEE_MONO_VTADDR:
			return emit_mono_vtaddr (builder);
		case MONO_CEE_MONO_RETHROW:
			return emit_mono_rethrow (builder);
		case MONO_CEE_MONO_NEWOBJ:
			return emit_mono_newobj (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_LDNATIVEOBJ:
			return emit_mono_ldnativeobj (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_RETOBJ:
			return emit_mono_retobj (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_JIT_ICALL_ADDR:
			return emit_mono_jit_icall_addr (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_ICALL_ADDR:
			return emit_mono_icall_addr (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_TLS:
			return emit_mono_tls (builder, static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_ATOMIC_STORE_I4:
			return emit_mono_atomic_store_i4 (builder,
			                                  static_cast<uint32_t> (operand));
		case MONO_CEE_MONO_LD_DELEGATE_METHOD_PTR:
			return emit_mono_ld_delegate_method_ptr (builder);
		case MONO_CEE_MONO_CALLI_EXTRA_ARG:
			return emit_mono_calli_extra_arg (builder,
			                                  static_cast<uint32_t> (operand));

		/*
		 * A sticky flag rather than a prefix: an address push may sit
		 * between it and the call whose errno it asks for, so it rides
		 * until the next call consumes it.
		 */
		case MONO_CEE_MONO_SAVE_LAST_ERROR:
			pending_save_last_error = true;
			return llvm::Error::success ();

		/* A hint that the branch it precedes is the unlikely one. */
		case MONO_CEE_MONO_NOT_TAKEN:
			return llvm::Error::success ();

		/*
		 * Bracketing marks around a call out to native code. Every wrapper that
		 * emits them is a save_lmf wrapper, so the chain is already linked for
		 * the whole body and unlinked on every exit; there is nothing narrower
		 * to do here.
		 */
		case MONO_CEE_MONO_SAVE_LMF:
		case MONO_CEE_MONO_RESTORE_LMF:
			return llvm::Error::success ();

		/*
		 * The flag a thread polls to notice it has been asked to stop. Its
		 * address, not its value - an ldind follows.
		 */
		case MONO_CEE_MONO_LDPTR_INT_REQ_FLAG:
			push_stack (address_symbol ("mono_thread_interruption_request_flag",
			                            &mono_thread_interruption_request_flag),
			            m_class_get_byval_arg (mono_defaults.int_class));
			return llvm::Error::success ();

		default:
			g_assert_not_reached ();
		}

	default:
		return unsupported_il (llvm::Twine ("no translation for ")
		                       + mono_opcode_name (opcode));
	}
}

/// The errno capture a pending mono_save_last_error asked for, right after the
/// call it decorated. Nothing may come between: any other runtime call can
/// clobber the value.
void
MethodLLVMEmitter::consume_save_last_error (MonoIrBuilder &builder)
{
	if (!pending_save_last_error)
		return;
	pending_save_last_error = false;

	llvm::FunctionCallee decl = module->getOrInsertFunction (
		"mono_marshal_set_last_error", llvm::Type::getVoidTy (context ()));

	if (auto *function = llvm::dyn_cast<llvm::Function> (decl.getCallee ()))
		function->setDoesNotThrow ();
	builder.CreateCall (decl);
}

/// Throw the corlib exception NAME - "DivideByZeroException" and friends, from
/// System - and end the block. Nothing after the call is reachable.
void
MethodLLVMEmitter::emit_throw_corlib_exception (MonoIrBuilder &builder, const char *name)
{
	MonoClass *klass = mono_class_load_from_name (mono_get_corlib (), "System", name);
	uint32_t token = m_class_get_type_token (klass) - MONO_TOKEN_TYPE_DEF;

	emit_unwinding_call (builder, throw_corlib_exception_decl (module),
	                     { builder.getInt32 (token) });
}

/// Throw the corlib exception NAME when CONDITION holds, and go on emitting into the
/// block where it did not.
llvm::BranchInst *
MethodLLVMEmitter::emit_cond_exception (MonoIrBuilder &builder, llvm::Value *condition,
                                        const char *name)
{
	llvm::BasicBlock *throw_bb =
		llvm::BasicBlock::Create (context (), llvm::Twine ("throw_") + name, function);
	llvm::BasicBlock *next_bb = llvm::BasicBlock::Create (context (), "no_throw", function);
	llvm::BranchInst *branch = builder.CreateCondBr (condition, throw_bb, next_bb);

	/*
	 * These guards sit in the fallthrough path of ordinary arithmetic, so say which
	 * way they go: otherwise the throw is as likely as the work it protects, and the
	 * block layout interleaves the two.
	 */
	llvm::MDBuilder md (context ());
	branch->setMetadata (llvm::LLVMContext::MD_prof, md.createBranchWeights (1, 1000));

	builder.SetInsertPoint (throw_bb);
	emit_throw_corlib_exception (builder, name);

	builder.SetInsertPoint (next_bb);
	return branch;
}

/// Throw NullReferenceException if POINTER is null, and go on emitting where it was not.
///
/// The branch is tagged !make.implicit, which is what lets LLVM's ImplicitNullChecks
/// pass delete it again: it folds the test into whichever faulting memory operation
/// follows in the not-taken block and records the fault address in a fault map, so the
/// check costs nothing until it fires. That only works in the shape emitted here - the
/// dereference in the not-taken arm, on the pointer that was tested - and the pass
/// declines and leaves the branch alone when the field offset is too far into the page
/// for the hardware to trap on it.
void
MethodLLVMEmitter::emit_null_check (MonoIrBuilder &builder, llvm::Value *pointer)
{
	llvm::BranchInst *branch = emit_cond_exception (builder, builder.CreateIsNull (pointer),
	                                                "NullReferenceException");

	branch->setMetadata ("make.implicit", llvm::MDNode::get (context (), {}));
}

llvm::Error
MethodLLVMEmitter::emit_arg_allocas (MonoIrBuilder &builder)
{
	auto sig = method->signature;
	unsigned nargs = sig->param_count + sig->hasthis;

	std::vector<const char *> names;
	names.resize (nargs);
	mono_method_get_param_names (method, names.data () + sig->hasthis);
	if (sig->hasthis)
		names[0] = "this";

	/*
	 * A wrapper that is itself a native entry is called with its value types
	 * already marshalled, so the slot an argument is spilled to has to be that
	 * shape and that size - the body reads the native fields straight out of
	 * it, past where the managed layout would have ended.
	 */
	bool native = native_signature ();

	for (unsigned i = 0; i < nargs; ++i) {
		auto mtype = mono_arg_type (method, i);
		auto ltyper = convert_type (mtype, native);
		if (!ltyper)
			return ltyper.takeError ();
		auto ltype = ltyper.get ();

		auto alloca = builder.CreateAlloca (ltype, nullptr, names[i]);

		alloca->setAlignment (type_alignment (mtype, native));
		builder.CreateAlignedStore (function->getArg (i), alloca,
		                            alloca->getAlign ());

		args.push_back ({
			.alloca = alloca,
			.type = mtype,
			.native = native,
		});
	}

	return llvm::Error::success ();
}

llvm::Error
MethodLLVMEmitter::emit_local_allocas (MonoIrBuilder &builder)
{
	const auto &DL = module->getDataLayout ();
	auto header = cfg->header;

	for (size_t i = 0; i < header->num_locals; ++i) {
		auto local = header->locals[i];
		auto ltyper = convert_type (local);
		if (!ltyper)
			return ltyper.takeError ();

		auto ltype = ltyper.get ();
		auto alloca = builder.CreateAlloca (ltype, nullptr,
		                                    llvm::Twine ("local") + llvm::Twine (i));
		alloca->setAlignment (type_alignment (local));
		builder.CreateMemSet (alloca, builder.getInt8 (0),
		                      builder.getInt64 (DL.getTypeAllocSize (ltype)),
		                      alloca->getAlign ());

		locals.push_back ({
			.alloca = alloca,
			.type = local,
		});
	}

	return llvm::Error::success ();
}

} // namespace mono
