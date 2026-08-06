/**
 * \file
 * il-line-table.cpp - IL offsets as LLVM debug info.
 */

#include "il-line-table.hpp"

#include <vector>

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace mono {

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
	/*
	 * DWARF 4 keeps the line-table header self-contained. DWARF 5 moves file and
	 * directory names out into `.debug_line_str`, which would make reading the
	 * tables back a two-section job for names nothing here ever looks at.
	 */
	if (!module->getModuleFlag ("Dwarf Version"))
		module->addModuleFlag (llvm::Module::Warning, "Dwarf Version", 4);
	if (!module->getModuleFlag ("Debug Info Version"))
		module->addModuleFlag (llvm::Module::Warning, "Debug Info Version",
		                       llvm::DEBUG_METADATA_VERSION);

	/*
	 * One file for the whole module, named after nothing in particular: the names
	 * that matter are the per-function ones, and nothing here is meant to be
	 * opened.
	 */
	impl_->file = impl_->di.createFile ("mono-tier1", ".");

	/*
	 * FullDebug rather than LineTablesOnly. Line tables alone would carry the IL
	 * offsets, but not which method each one belongs to once a body has been
	 * inlined - DW_TAG_inlined_subroutine lives in `.debug_info`, and that is the
	 * whole point of naming the callees.
	 */
	impl_->cu = impl_->di.createCompileUnit (
		llvm::dwarf::DW_LANG_C99, impl_->file, "mono tier-1", /*isOptimized=*/ true, "", 0,
		llvm::StringRef (), llvm::DICompileUnit::FullDebug);
}

IlDebugModule::~IlDebugModule () = default;

IlDebugScope *
IlDebugModule::add_function (llvm::Function *fn, const char *name)
{
	llvm::DISubroutineType *type =
		impl_->di.createSubroutineType (impl_->di.getOrCreateTypeArray ({}));

	llvm::DISubprogram *sp = impl_->di.createFunction (
		impl_->cu, name, name, impl_->file, /*LineNo=*/ 1, type, /*ScopeLine=*/ 1,
		llvm::DINode::FlagZero,
		llvm::DISubprogram::SPFlagDefinition | llvm::DISubprogram::SPFlagOptimized);

	fn->setSubprogram (sp);

	impl_->scopes.push_back (std::make_unique<IlDebugScope> ());
	IlDebugScope *scope = impl_->scopes.back ().get ();
	scope->subprogram = sp;
	/*
	 * Everything emitted before the first OP_IL_SEQ_POINT - the prologue, the
	 * entry block's allocas - belongs to the method's first IL byte.
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
