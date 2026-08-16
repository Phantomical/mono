// Basic-block shapes the transform has code for and a C# compiler never makes:
// a switch whose only edge is the fall-through, a backward switch arm, and a
// branch reached from an empty block.

.assembly extern mscorlib { }

.class public auto ansi BBlocks extends [mscorlib]System.Object
{
	// Every arm targets the instruction after the switch, so the block has one
	// successor and merges with what follows it.
	.method public static int32 test_6_switch_arms_all_fall_through () cil managed
	{
		.maxstack 2
		ldc.i4.1
		switch (IL_next, IL_next)
	IL_next:
		ldc.i4.6
		ret
	}

	// A switch arm that goes backwards.  Case bodies in C# always sit after the
	// switch, so this edge shape comes from IL only.
	.method public static int32 test_8_backward_switch_arm () cil managed
	{
		.maxstack 2
		.locals init (int32 count)
		ldc.i4.0
		stloc.0
		br IL_dispatch
	IL_back:
		ldloc.0
		ldc.i4.8
		add
		stloc.0
		br IL_done
	IL_dispatch:
		ldc.i4.0
		switch (IL_back)
		ldc.i4.m1
		stloc.0
	IL_done:
		ldloc.0
		ret
	}

	// A chain of blocks holding nothing but a branch.
	.method public static int32 test_4_empty_block_chain () cil managed
	{
		.maxstack 2
		br IL_a
	IL_a:
		br IL_b
	IL_b:
		br IL_c
	IL_c:
		ldc.i4.4
		ret
	}

	// Code after an unconditional branch that nothing reaches.  The transform
	// has to drop it rather than fall into it.
	.method public static int32 test_2_unreachable_tail () cil managed
	{
		.maxstack 2
		ldc.i4.2
		br IL_end
		ldc.i4.s 9
		ret
	IL_end:
		ret
	}

	// A conditional branch whose two arms leave the same local, reached from
	// several predecessors.
	.method public static int32 test_12_diamond_merge () cil managed
	{
		.maxstack 3
		.locals init (int32 acc, int32 i)
		ldc.i4.0
		stloc.0
		ldc.i4.0
		stloc.1
	IL_loop:
		ldloc.1
		ldc.i4.4
		bge IL_done
		ldloc.1
		ldc.i4.2
		rem
		brtrue IL_odd
		ldloc.0
		ldc.i4.1
		add
		stloc.0
		br IL_step
	IL_odd:
		ldloc.0
		ldc.i4.5
		add
		stloc.0
	IL_step:
		ldloc.1
		ldc.i4.1
		add
		stloc.1
		br IL_loop
	IL_done:
		ldloc.0
		ret
	}
}
