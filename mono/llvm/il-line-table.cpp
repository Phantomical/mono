/**
 * \file
 * \brief IL offsets as LLVM debug info.
 */

#include "il-line-table.hpp"

#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

namespace mono {

namespace {

/*
 * The subprogram ids ride in module metadata, as one `!{DISubprogram, i64 id}`
 * tuple each under the name below.
 *
 * The compiler reads them at the machine layer, and by then
 * StripInlineCopiesPass has taken every inlined copy's body back off.
 * Function::deleteBody () clears the function's metadata, the subprogram
 * attachment with it, so an id kept on the function is gone exactly where it is
 * wanted. The module owns this metadata instead, and the inlined locations keep
 * the subprogram itself alive.
 */
constexpr llvm::StringRef subprogram_ids_name = "mono.il.subprogram.ids";

void
note_subprogram_id (llvm::Module &m, llvm::DISubprogram *sp, uint64_t id)
{
	llvm::LLVMContext &ctx = m.getContext ();
	llvm::Metadata *entry[] = {
		sp,
		llvm::ConstantAsMetadata::get (llvm::ConstantInt::get (
			llvm::Type::getInt64Ty (ctx), id)),
	};

	m.getOrInsertNamedMetadata (subprogram_ids_name)
		->addOperand (llvm::MDNode::get (ctx, entry));
}

} // namespace

struct IlDebugScope {
	llvm::DISubprogram *subprogram = nullptr;
	llvm::DILocation *cur = nullptr;
};

struct IlDebugModule::Impl {
	llvm::DIBuilder di;
	llvm::DICompileUnit *cu = nullptr;
	llvm::DIFile *file = nullptr;
	/* Stable addresses: the translator holds IlDebugScope* for the whole compile. */
	std::vector<std::unique_ptr<IlDebugScope>> scopes;

	explicit Impl (llvm::Module &m) : di (m) {}
};

IlDebugModule::IlDebugModule (llvm::Module *module)
	: impl_ (std::make_unique<Impl> (*module))
{
	if (!module->getModuleFlag ("Debug Info Version"))
		module->addModuleFlag (llvm::Module::Warning, "Debug Info Version",
		                       llvm::DEBUG_METADATA_VERSION);

	/*
	 * One file for the whole module, named after nothing in particular: what
	 * matters is the per-function names, and no tool opens the file this names.
	 */
	impl_->file = impl_->di.createFile ("mono-tier1", ".");

	/*
	 * NoDebug: this metadata is here to carry IL offsets down to the machine
	 * layer, where the compiler reads them off the instructions and writes
	 * `.mono_lines`. We never read DWARF back, so asking for none of it to be
	 * emitted is a straight saving.
	 */
	impl_->cu = impl_->di.createCompileUnit (
		llvm::dwarf::DW_LANG_C99, impl_->file, "mono tier-1", /*isOptimized=*/ true, "", 0,
		llvm::StringRef (), llvm::DICompileUnit::NoDebug);
}

IlDebugModule::~IlDebugModule () = default;

IlDebugScope *
IlDebugModule::add_function (llvm::Function *fn, const char *name, uint64_t id)
{
	llvm::DISubroutineType *type =
		impl_->di.createSubroutineType (impl_->di.getOrCreateTypeArray ({}));

	llvm::DISubprogram *sp = impl_->di.createFunction (
		impl_->cu, name, name, impl_->file, /*LineNo=*/ 1, type, /*ScopeLine=*/ 1,
		llvm::DINode::FlagZero,
		llvm::DISubprogram::SPFlagDefinition | llvm::DISubprogram::SPFlagOptimized);

	fn->setSubprogram (sp);
	note_subprogram_id (*fn->getParent (), sp, id);

	impl_->scopes.push_back (std::make_unique<IlDebugScope> ());
	IlDebugScope *scope = impl_->scopes.back ().get ();
	scope->subprogram = sp;
	/*
	 * Everything emitted before the first call to il_debug_set_location () -
	 * the prologue, the entry block's allocas - belongs to the method's first
	 * IL byte.
	 */
	scope->cur = llvm::DILocation::get (fn->getContext (), IL_OFFSET_LINE_BIAS,
	                                    /*Column=*/ 1, sp);
	return scope;
}

void
IlDebugModule::finish ()
{
	impl_->di.finalize ();
}

llvm::DenseMap<const llvm::DISubprogram *, uint64_t>
il_debug_subprogram_ids (const llvm::Module &m)
{
	llvm::DenseMap<const llvm::DISubprogram *, uint64_t> ids;
	const llvm::NamedMDNode *named = m.getNamedMetadata (subprogram_ids_name);

	if (named == nullptr)
		return ids;

	for (const llvm::MDNode *entry : named->operands ()) {
		if (entry->getNumOperands () != 2)
			continue;

		auto *sp = llvm::dyn_cast_or_null<llvm::DISubprogram> (entry->getOperand (0));
		auto *id = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata> (
			entry->getOperand (1));

		if (sp != nullptr && id != nullptr)
			ids[sp] = llvm::cast<llvm::ConstantInt> (id->getValue ())
					  ->getZExtValue ();
	}

	return ids;
}

void
il_debug_set_location (IlDebugScope *scope, llvm::IRBuilder<> *builder, uint32_t il_offset)
{
	if (!scope)
		return;

	scope->cur = llvm::DILocation::get (
		scope->subprogram->getContext (), il_offset + IL_OFFSET_LINE_BIAS,
		/*Column=*/ 1, scope->subprogram);

	if (builder)
		builder->SetCurrentDebugLocation (llvm::DebugLoc (scope->cur));
}

void
il_debug_set_instruction_location (IlDebugScope *scope, llvm::Instruction *inst,
                                   uint32_t il_offset)
{
	if (!scope || !inst)
		return;

	inst->setDebugLoc (llvm::DebugLoc (llvm::DILocation::get (
		scope->subprogram->getContext (), il_offset + IL_OFFSET_LINE_BIAS,
		/*Column=*/ 1, scope->subprogram)));
}

void
il_debug_reapply (IlDebugScope *scope, llvm::IRBuilder<> *builder)
{
	if (scope && builder && scope->cur)
		builder->SetCurrentDebugLocation (llvm::DebugLoc (scope->cur));
}

} // namespace mono
