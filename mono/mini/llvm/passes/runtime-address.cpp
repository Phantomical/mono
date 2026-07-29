/**
 * \file
 * runtime-address.cpp: recovering the runtime address behind a constant operand.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "runtime-address.hpp"

#include "../engine.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Value.h>

using namespace llvm;

namespace mono {

bool
runtime_address (const Value *v, const DataLayout &dl, uint64_t *addr)
{
	int64_t offset = 0;

	/*
	 * Peel until we reach something that names an address. Casts between
	 * pointer and integer keep the value; a GEP or a constant-offset expression
	 * shifts it, so the shift has to be carried along.
	 */
	for (;;) {
		if (auto *literal = dyn_cast<ConstantInt> (v)) {
			*addr = literal->getZExtValue () + offset;
			return true;
		}

		if (auto *gv = dyn_cast<GlobalVariable> (v)) {
			void *base = MonoLLVMJIT::get_singleton ()->symbol_address (gv->getName ());
			if (!base)
				return false;
			*addr = (uint64_t) (uintptr_t) base + offset;
			return true;
		}

		if (auto *expr = dyn_cast<ConstantExpr> (v)) {
			if (expr->getOpcode () == Instruction::IntToPtr ||
			    expr->getOpcode () == Instruction::PtrToInt) {
				v = expr->getOperand (0);
				continue;
			}
		}

		if (!v->getType ()->isPointerTy ())
			return false;

		APInt step (dl.getIndexTypeSizeInBits (v->getType ()), 0);
		const Value *base = v->stripAndAccumulateConstantOffsets (dl, step, true);
		if (base == v)
			return false;

		offset += step.getSExtValue ();
		v = base;
	}
}

} // namespace mono
