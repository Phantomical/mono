/**
 * \file
 * \brief The counter that asks tier0-classic for tier 1.
 */

#include "compile.h"
#include "ir-emit.h"

#include <mono/llvm/runtime.h>
#include <mono/mini/domain-method.h>

/// Emits "if (*counter > 0 && (*counter -= cost) <= 0) mono_tier0_spent
/// (method, domain)", with the call out of line.
///
/// The load, the subtract and the store are plain, so a charge costs no locked
/// instruction and two threads charging at once lose a count between them. The
/// leading test is what keeps a spent body off the rest: it calls nothing of
/// its own and does not write, at every entry and every loop turn for the rest
/// of the method's run. A stack overflow then faults in managed code, where
/// mono_handle_soft_stack_ovf () raises a catchable StackOverflowException
/// instead of aborting.
static void
emit_guarded_count (MonoCompile *cfg, int32_t *counter, int32_t cost)
{
	MonoInst *addr, *iargs [2];
	MonoBasicBlock *done_bb;
	int val_reg, left_reg;

	EMIT_NEW_PCONST (cfg, addr, counter);

	val_reg = alloc_ireg (cfg);
	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, val_reg, addr->dreg, 0);

	NEW_BBLOCK (cfg, done_bb);

	/* Both compares are 32-bit, because the count is. A 64-bit one reads the
	 * subtract's answer as a large positive number the moment it goes past
	 * zero, since the subtract leaves the top half clear rather than signed. */
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, val_reg, 0);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBLE, done_bb);

	left_reg = alloc_ireg (cfg);
	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ISUB_IMM, left_reg, val_reg, cost);
	MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, addr->dreg, 0, left_reg);

	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_ICOMPARE_IMM, -1, left_reg, 0);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBGT, done_bb);

	cfg->cbb->out_of_line = TRUE;
	EMIT_NEW_METHODCONST (cfg, iargs [0], cfg->method);
	EMIT_NEW_PCONST (cfg, iargs [1], cfg->domain);
	mono_emit_jit_icall (cfg, mono_tier0_spent, iargs);

	MONO_START_BB (cfg, done_bb);
}

static void
emit_counter (MonoCompile *cfg, int32_t cost)
{
	int32_t *counter;

	if (cfg->current_method != cfg->method)
		return;

	counter = mono_tier0_counter_address (cfg->method, cfg->domain);

	if (counter != NULL)
		emit_guarded_count (cfg, counter, cost);
}

void
mini_tier0_emit_entry_counter (MonoCompile *cfg)
{
	emit_counter (cfg, mono_llvm_jit_tier0_entry_weight ());
}

void
mini_tier0_emit_loop_counter (MonoCompile *cfg, int32_t il_bytes)
{
	/* One turn charges what the turn ran over, which is how a method that
	 * loops rather than being called reaches the threshold at all. A back edge
	 * whose target is the branch itself still ran a turn. */
	emit_counter (cfg, il_bytes > 0 ? il_bytes : 1);
}
