/**
 * \file
 * \brief What exception dispatch and stack walking assume about the machine.
 */

#include "arch/arch.hpp"

#include "mini.h"

// This breaks some LLVM headers
#undef PIC

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>

namespace mono::arch {

bool
reg_is_recoverable (int hw_reg)
{
	switch (hw_reg) {
	case AMD64_RSP:
	case AMD64_RBP:
	case AMD64_RBX:
	case AMD64_R12:
	case AMD64_R13:
	case AMD64_R14:
	case AMD64_R15:
		return true;
	default:
		return false;
	}
}

llvm::Value *
emit_entered_exception (llvm::IRBuilderBase &b)
{
	llvm::Type *ptr = llvm::PointerType::get (b.getContext (), 0);

	return b.CreateCall (
		llvm::InlineAsm::get (llvm::FunctionType::get (ptr, false), "", "={rax}",
	                              /*hasSideEffects=*/true));
}

} // namespace mono::arch
