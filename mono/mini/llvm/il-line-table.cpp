/**
 * \file
 * il-line-table.cpp - IL offsets as LLVM line-table debug info.
 */

#include "il-line-table.hpp"

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace mono {

struct IlLineTable::Impl {
	llvm::DIBuilder di;
	llvm::DISubprogram *subprogram = nullptr;
	llvm::DILocation *cur = nullptr;

	explicit Impl (llvm::Module &m) : di (m) {}
};

IlLineTable::IlLineTable (llvm::Module *module, llvm::Function *fn, const char *name)
	: impl_ (std::make_unique<Impl> (*module))
{
	/*
	 * DWARF 4 keeps the line-table header self-contained. DWARF 5 moves file and
	 * directory names out into `.debug_line_str`, which would make reading the
	 * table back a two-section job for names nothing here ever looks at.
	 */
	if (!module->getModuleFlag ("Dwarf Version"))
		module->addModuleFlag (llvm::Module::Warning, "Dwarf Version", 4);
	if (!module->getModuleFlag ("Debug Info Version"))
		module->addModuleFlag (llvm::Module::Warning, "Debug Info Version",
		                       llvm::DEBUG_METADATA_VERSION);

	/*
	 * The "file" is the method: nothing here is meant to be opened, and naming it
	 * after the method is what makes a dumped line table readable.
	 */
	llvm::DIFile *file = impl_->di.createFile (name, ".");

	/*
	 * LineTablesOnly - `.debug_line` and nothing else. Types and variables are
	 * the parts of debug info mono has nothing to say about, and emitting them
	 * would cost object size for no reader.
	 */
	llvm::DICompileUnit *cu = impl_->di.createCompileUnit (
		llvm::dwarf::DW_LANG_C99, file, "mono tier-1", /*isOptimized=*/ true, "", 0,
		llvm::StringRef (), llvm::DICompileUnit::LineTablesOnly);

	llvm::DISubroutineType *type =
		impl_->di.createSubroutineType (impl_->di.getOrCreateTypeArray ({}));

	impl_->subprogram = impl_->di.createFunction (
		cu, name, name, file, /*LineNo=*/ 1, type, /*ScopeLine=*/ 1,
		llvm::DINode::FlagZero,
		llvm::DISubprogram::SPFlagDefinition | llvm::DISubprogram::SPFlagOptimized);

	fn->setSubprogram (impl_->subprogram);

	/*
	 * Everything emitted before the first OP_IL_SEQ_POINT - the prologue, the
	 * entry block's allocas - belongs to the method's first IL byte.
	 */
	impl_->cur = llvm::DILocation::get (module->getContext (), IL_OFFSET_LINE_BIAS,
	                                    /*Column=*/ 1, impl_->subprogram);
}

IlLineTable::~IlLineTable () = default;

void
IlLineTable::set_location (llvm::IRBuilder<> *builder, uint32_t il_offset)
{
	impl_->cur = llvm::DILocation::get (
		impl_->subprogram->getContext (), il_offset + IL_OFFSET_LINE_BIAS,
		/*Column=*/ 1, impl_->subprogram);

	if (builder)
		builder->SetCurrentDebugLocation (llvm::DebugLoc (impl_->cur));
}

void
IlLineTable::reapply (llvm::IRBuilder<> *builder) const
{
	if (builder && impl_->cur)
		builder->SetCurrentDebugLocation (llvm::DebugLoc (impl_->cur));
}

void
IlLineTable::finish ()
{
	impl_->di.finalize ();
}

} // namespace mono
