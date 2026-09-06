/**
 * \file
 * \brief The counter that asks tier0-classic for tier 1.
 */

#include "compile.h"
#include "ir-emit.h"

#include <mono/mini/domain-method.h>

/// Emits, out of line: "if (*counter > 0) mono_tier0_count (method, domain)".
///
/// The plain load keeps the icall's own atomic decrement off the path a spent
/// body takes. That path is every entry and every loop turn for the rest of the
/// method's run. A spent body calls nothing of its own there. A stack
/// overflow then faults in managed code, where mono_handle_soft_stack_ovf ()
/// raises a catchable StackOverflowException instead of aborting.
static void
emit_guarded_count (MonoCompile *cfg, int32_t *counter)
{
	MonoInst *addr, *iargs [2];
	MonoBasicBlock *done_bb;
	int val_reg;

	EMIT_NEW_PCONST (cfg, addr, counter);

	val_reg = alloc_ireg (cfg);
	MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, val_reg, addr->dreg, 0);

	NEW_BBLOCK (cfg, done_bb);

	MONO_EMIT_NEW_BIALU_IMM (cfg, OP_COMPARE_IMM, -1, val_reg, 0);
	MONO_EMIT_NEW_BRANCH_BLOCK (cfg, OP_IBLE, done_bb);

	cfg->cbb->out_of_line = TRUE;
	EMIT_NEW_METHODCONST (cfg, iargs [0], cfg->method);
	EMIT_NEW_PCONST (cfg, iargs [1], cfg->domain);
	mono_emit_jit_icall (cfg, mono_tier0_count, iargs);

	MONO_START_BB (cfg, done_bb);
}

/// Emits one count against the method's way out of tier 0, into the block being
/// built. Does nothing while cfg is translating an inlined callee's own body.
///
/// The count sits behind a branch, so that block has to be one a branch can end.
/// The initlocals block is not. The caller decides what one count is: the first
/// code block charges one call, and a backward branch charges one turn.
void
mini_tier0_emit_counter (MonoCompile *cfg)
{
	if (cfg->current_method != cfg->method)
		return;

	int32_t *counter = mono_tier0_counter_address (cfg->method, cfg->domain);

	if (counter != NULL)
		emit_guarded_count (cfg, counter);
}
