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

int
dwarf_reg_for_win_unwind (unsigned unwind_reg, bool is_vector)
{
	if (is_vector) {
		/*
		 * The amd64 DWARF numbering puts xmm0 at 17. mono_hw_reg_to_dwarf_reg
		 * () cannot answer for these: mono's own map stops at RIP, because
		 * mono_unwind_frame () restores the general registers alone. jinfo.cpp
		 * drops the rule it builds from this, and says there why.
		 */
		return unwind_reg < 16 ? (int) (17 + unwind_reg) : -1;
	}

	/* A Win64 unwind code names a general register by its hardware encoding,
	 * which is what AMD64_Reg_No counts in. */
	if (unwind_reg >= AMD64_NREG)
		return -1;

	return mono_hw_reg_to_dwarf_reg ((int) unwind_reg);
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
