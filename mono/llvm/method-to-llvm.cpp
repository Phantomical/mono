#include "method-to-llvm.hpp"
#include "runtime-error.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/metadata.h"
#include "mono/metadata/opcodes.h"
#include "mono/metadata/tokentype.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/ErrorHandling.h>

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
method_to_llvm (llvm::Module *module, MonoCompile *cfg, MonoMethod *method)
{
	auto emitter = MethodLLVMEmitter (module, cfg, method);
	return emitter.emit ();
}

llvm::LLVMContext &
MethodLLVMEmitter::context () const
{
	return module->getContext ();
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

	MonoIrBuilder builder (context ());
	llvm::BasicBlock *body = llvm::BasicBlock::Create (context (), "entry", function);
	builder.SetInsertPoint (body);

	if (auto error = emit_arg_allocas (builder))
		return std::move (error);
	if (auto error = emit_local_allocas (builder))
		return std::move (error);

	while (ip < code_size) {
		offset = ip;

		/*
		 * Everything reachable is still one straight line, so an instruction that
		 * follows a terminator has no block to go in. That is what branch targets
		 * will start their own blocks for.
		 */
		if (builder.GetInsertBlock ()->getTerminator () != nullptr)
			return unsupported_il ("unreachable instruction");

		if (llvm::Error error = emit_instruction (builder))
			return std::move (error);
	}

	if (builder.GetInsertBlock ()->getTerminator () == nullptr)
		return invalid_il ("method body ends without returning");

	return function;
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
	 * A local or argument index is decoded here rather than in the emitters: mono's
	 * opcode table says which of the two widths to read, and an instruction that
	 * takes one takes nothing else. The short forms that carry the index in the
	 * opcode itself have no operand at all and pass their own constant below.
	 */
	uint32_t index = 0;

	switch (mono_opcodes[opcode].argument) {
	case MonoShortInlineVar: {
		llvm::Expected<uint8_t> read = read_u8 ();

		if (!read)
			return read.takeError ();

		index = *read;
		break;
	}
	case MonoInlineVar: {
		llvm::Expected<uint16_t> read = read_u16 ();

		if (!read)
			return read.takeError ();

		index = *read;
		break;
	}
	default:
		break;
	}

	switch (opcode) {
	case MONO_CEE_NOP:
		return llvm::Error::success ();
	case MONO_CEE_RET:
		return emit_ret (builder);

	case MONO_CEE_LDLOC:
	case MONO_CEE_LDLOC_S:
		return emit_ldloc (builder, index);
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
		return emit_ldloca (builder, index);

	case MONO_CEE_STLOC:
	case MONO_CEE_STLOC_S:
		return emit_stloc (builder, index);
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

	default:
		return unsupported_il (llvm::Twine ("no translation for ")
		                       + mono_opcode_name (opcode));
	}
}

/// Throw the corlib exception NAME - "DivideByZeroException" and friends, from
/// System - and end the block. Nothing after the call is reachable.
void
MethodLLVMEmitter::emit_throw_corlib_exception (MonoIrBuilder &builder, const char *name)
{
	MonoClass *klass = mono_class_load_from_name (mono_get_corlib (), "System", name);
	uint32_t token = m_class_get_type_token (klass) - MONO_TOKEN_TYPE_DEF;

	builder.CreateCall (throw_corlib_exception_decl (module), builder.getInt32 (token));
	builder.CreateUnreachable ();
}

/// Throw the corlib exception NAME when CONDITION holds, and go on emitting into the
/// block where it did not.
void
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

	for (int i = 0; i < nargs; ++i) {
		auto mtype = mono_arg_type (method, i);
		auto ltyper = convert_type (mtype);
		if (!ltyper)
			return ltyper.takeError ();
		auto ltype = ltyper.get ();

		auto alloca = builder.CreateAlloca (ltype, nullptr, names[i]);
		alloca->setAlignment (type_alignment (mtype));
		builder.CreateAlignedStore (function->getArg (i), alloca, alloca->getAlign ());

		args.push_back ({
			.alloca = alloca,
			.type = mtype,
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
