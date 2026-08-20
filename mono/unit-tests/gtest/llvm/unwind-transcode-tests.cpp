/*
 * Tests for transcode_unwind () in mono/llvm/jinfo.cpp, which turns the
 * `.mono_unwind` records describing one function into the unwind ops mono's
 * unwinder executes.
 *
 * Three records do not survive unchanged. Two of them state a rule mono has no
 * opcode for and are replayed as one it does, and the answer they are replayed
 * as is what these cases pin: a wrong one does not stop a stack walk, it
 * continues it to a wrong frame. The third states no rule and is dropped.
 *
 * An op the transcoder does not name has to come back as an error rather than
 * as a partial description, so the refusals are cases here as well.
 */

#include "jinfo.hpp"

#include "mini-unwind.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace mono {
namespace test {
namespace {

/* The DWARF numbers of the two registers the cases below name. */
constexpr int32_t dwarf_rsp = 7;
constexpr int32_t dwarf_rbp = 6;

UnwindRecord
record (uint32_t offset, uint8_t op, int32_t reg, int64_t value)
{
	UnwindRecord r;

	r.offset = offset;
	r.op = op;
	r.reg = reg;
	r.value = value;
	return r;
}

/// The entry records every frame opens with: the CFA is 8 above rsp.
UnwindRecord
entry_def_cfa ()
{
	return record (0, MONO_UNWIND_OP_DEF_CFA, dwarf_rsp, 8);
}

struct Op {
	uint8_t op;
	uint16_t reg;
	int32_t val;
	uint32_t when;

	bool operator== (const Op &other) const
	{
		return op == other.op && reg == other.reg && val == other.val
		       && when == other.when;
	}
};

/// The ops transcode_unwind () answers, or an empty answer when it refuses.
/// The list it builds is freed here, so a case only ever sees the values.
std::vector<Op>
transcode (const std::vector<UnwindRecord> &records, bool *refused = nullptr)
{
	llvm::Expected<GSList *> answer = transcode_unwind (records);
	std::vector<Op> ops;

	if (!answer) {
		llvm::consumeError (answer.takeError ());
		if (refused != nullptr)
			*refused = true;
		return ops;
	}

	if (refused != nullptr)
		*refused = false;

	for (GSList *l = *answer; l != nullptr; l = l->next) {
		const MonoUnwindOp *op = (const MonoUnwindOp *) l->data;

		ops.push_back (Op { op->op, op->reg, op->val, op->when });
	}

	mono_free_unwind_info (*answer);
	return ops;
}

/// Where the last def_cfa_offset in \p ops put the CFA.
int32_t
final_cfa_offset (const std::vector<Op> &ops)
{
	int32_t offset = -1;

	for (const Op &op : ops)
		if (op.op == DW_CFA_def_cfa_offset || op.op == DW_CFA_def_cfa)
			offset = op.val;

	return offset;
}

TEST (UnwindTranscode, AnAdjustmentBecomesTheOffsetItArrivesAt)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x10, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 16) });

	ASSERT_EQ (ops.size (), 2u);
	EXPECT_EQ (ops[1], (Op { DW_CFA_def_cfa_offset, 0, 24, 0x10 }));
}

TEST (UnwindTranscode, AdjustmentsAccumulate)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x10, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 16),
		  record (0x20, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 32),
		  record (0x30, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, -48) });

	ASSERT_EQ (ops.size (), 4u);
	EXPECT_EQ (ops[1].val, 24);
	EXPECT_EQ (ops[2].val, 56);
	EXPECT_EQ (ops[3].val, 8);
}

/// An absolute record has to reset the running offset, or every adjustment
/// behind it answers for a CFA the frame left long ago.
TEST (UnwindTranscode, AnAbsoluteOffsetRestartsTheCount)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x10, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 16),
		  record (0x20, MONO_UNWIND_OP_DEF_CFA_OFFSET, 0, 64),
		  record (0x30, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 8) });

	EXPECT_EQ (final_cfa_offset (ops), 72);
}

/// A def_cfa names a register and an offset at once, so it moves the running
/// offset as much as a def_cfa_offset does.
TEST (UnwindTranscode, ADefCfaRestartsTheCount)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x10, MONO_UNWIND_OP_DEF_CFA, dwarf_rbp, 32),
		  record (0x20, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 8) });

	EXPECT_EQ (final_cfa_offset (ops), 40);
}

/// mono executes remember and restore itself, so the running offset has to
/// follow them or an adjustment after a restore answers from the wrong state.
TEST (UnwindTranscode, RestoreStateTakesTheOffsetBackWithIt)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x10, MONO_UNWIND_OP_REMEMBER_STATE, 0, 0),
		  record (0x20, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 64),
		  record (0x30, MONO_UNWIND_OP_RESTORE_STATE, 0, 0),
		  record (0x40, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 16) });

	EXPECT_EQ (final_cfa_offset (ops), 24);
}

/// The argument size states no rule, so it leaves nothing behind and does not
/// disturb the offset the records around it are counting.
TEST (UnwindTranscode, TheArgumentSizeIsDropped)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x10, MONO_UNWIND_OP_ARGS_SIZE, 0, 32),
		  record (0x20, MONO_UNWIND_OP_ADJUST_CFA_OFFSET, 0, 16) });

	ASSERT_EQ (ops.size (), 2u);
	EXPECT_EQ (ops[1], (Op { DW_CFA_def_cfa_offset, 0, 24, 0x20 }));
}

/// An op this transcoder does not name is a description it cannot complete, so
/// it refuses the whole frame rather than answering with the rest of it.
TEST (UnwindTranscode, AnUnknownOpIsRefused)
{
	bool refused = false;

	transcode ({ entry_def_cfa (), record (0x10, 200, 0, 0) }, &refused);
	EXPECT_TRUE (refused);
}

TEST (UnwindTranscode, RestoreStateWithoutRememberIsRefused)
{
	bool refused = false;

	transcode ({ entry_def_cfa (),
	             record (0x10, MONO_UNWIND_OP_RESTORE_STATE, 0, 0) },
	           &refused);
	EXPECT_TRUE (refused);
}

/// mono holds one remembered state, so a second remember would mis-unwind.
TEST (UnwindTranscode, NestedRememberIsRefused)
{
	bool refused = false;

	transcode ({ entry_def_cfa (),
	             record (0x10, MONO_UNWIND_OP_REMEMBER_STATE, 0, 0),
	             record (0x20, MONO_UNWIND_OP_REMEMBER_STATE, 0, 0) },
	           &refused);
	EXPECT_TRUE (refused);
}

/// RESTORE reverts a register to its rule at entry. A register the entry saved
/// goes back to that offset.
TEST (UnwindTranscode, RestoreReplaysTheEntryOffset)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0, MONO_UNWIND_OP_OFFSET, dwarf_rbp, -16),
		  record (0x30, MONO_UNWIND_OP_RESTORE, dwarf_rbp, 0) });

	ASSERT_EQ (ops.size (), 3u);
	EXPECT_EQ (ops[2].op, DW_CFA_offset);
	EXPECT_EQ (ops[2].val, -16);
}

/// A register the entry did not save has no offset to go back to, so it
/// reverts to being unsaved.
TEST (UnwindTranscode, RestoreOfAnUnsavedRegisterIsSameValue)
{
	std::vector<Op> ops = transcode (
		{ entry_def_cfa (),
		  record (0x30, MONO_UNWIND_OP_RESTORE, dwarf_rbp, 0) });

	ASSERT_EQ (ops.size (), 2u);
	EXPECT_EQ (ops[1].op, DW_CFA_same_value);
}

} // namespace
} // namespace test
} // namespace mono
