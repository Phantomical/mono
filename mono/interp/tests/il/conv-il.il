// The conv.* forms that a C# compiler does not emit.
//
// generate_code () selects the MINT opcode from the stack type of the source.
// Three source types do not occur in code from csc: a managed pointer (ldloca,
// ldarga, ldflda), an object reference, and a float under an opcode that names
// a native int. Each test here puts one of those on the stack.
//
// A method named test_<n>_<what> is a test. It passes when it returns <n>.
//
// An address is not a constant. A test on a managed pointer therefore states a
// relation: the address goes through the conversion under test and through a
// path that other tests already cover, and the two answers must agree.
//
// Three of these tests are not correct CIL. In Table III.8 the int32 row has an
// empty cell in the & column, so conv.i4, conv.u4 and conv.ovf.u4 on a managed
// pointer are invalid. The interpreter takes them, and the three generate_code ()
// arms for them have no other input. The LLVM backend refuses the method with
// InvalidProgramException, so these three fail on the compiled arm:
//
//	test_1_conv_u4_from_mp_truncates
//	test_1_conv_i4_from_mp_truncates
//	test_1_conv_ovf_u4_from_mp

.assembly extern mscorlib { }
.module extern 'interp-tests-cs.netmodule'

.class public auto ansi ConvIL extends [mscorlib]System.Object
{
	.custom instance void [.module 'interp-tests-cs.netmodule']NoOptAttribute::.ctor() = ( 01 00 00 00 )
	// The transform folds an operation on a constant. An operand that must
	// reach the opcode at run time comes through one of these first.
	.method public static int32 Id4 (int32 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	.method public static int64 Id8 (int64 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	.method public static float32 IdR4 (float32 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	.method public static float64 IdR8 (float64 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	// -------------------------------------------------------------------
	// conv.i8 / conv.u8 / conv.i4 / conv.u4 from a managed pointer
	// -------------------------------------------------------------------

	// A pointer widened to eight bytes and turned back into a pointer must
	// still address the local it came from.
	.method public static int32 test_42_conv_i8_from_mp () cil managed
	{
		.maxstack 2
		.locals init (int32 held, int64 addr)
		ldc.i4 42
		stloc.0
		ldloca.s 0
		conv.i8
		stloc.1
		ldloc.1
		conv.i
		ldind.i4
		ret
	}

	.method public static int32 test_42_conv_u8_from_mp () cil managed
	{
		.maxstack 2
		.locals init (int32 held, int64 addr)
		ldc.i4 42
		stloc.0
		ldloca.s 0
		conv.u8
		stloc.1
		ldloc.1
		conv.i
		ldind.i4
		ret
	}

	// conv.u4 keeps the low four bytes of the address and nothing else.
	.method public static int32 test_1_conv_u4_from_mp_truncates () cil managed
	{
		.maxstack 4
		.locals init (int32 held, int64 addr)
		ldloca.s 0
		conv.i8
		stloc.1
		ldloca.s 0
		conv.u4
		conv.u8
		ldloc.1
		ldc.i8 0xFFFFFFFF
		and
		ceq
		ret
	}

	// conv.i4 keeps the same four bytes, and widening the result again sign
	// extends them.
	.method public static int32 test_1_conv_i4_from_mp_truncates () cil managed
	{
		.maxstack 4
		.locals init (int32 held, int64 addr)
		ldloca.s 0
		conv.i8
		stloc.1
		ldloca.s 0
		conv.i4
		conv.i8
		ldloc.1
		ldc.i4.s 32
		shl
		ldc.i4.s 32
		shr
		ceq
		ret
	}

	// The checked form must trap on an address above uint32 max and must give
	// the address back below it. Nothing here fixes where the local lands, so
	// each arm asserts against the address rather than against a constant.
	.method public static int32 test_1_conv_ovf_u4_from_mp () cil managed
	{
		.maxstack 4
		.locals init (int32 held, int32 result, int64 addr)
		ldloca.s 0
		conv.i8
		stloc.2
		.try {
			ldloca.s 0
			conv.ovf.u4
			conv.u8
			ldloc.2
			ceq
			ldloc.2
			ldc.i8 0xFFFFFFFF
			cgt.un
			ldc.i4.0
			ceq
			and
			stloc.1
			leave IL_done
		} catch [mscorlib]System.OverflowException {
			pop
			ldloc.2
			ldc.i8 0xFFFFFFFF
			cgt.un
			stloc.1
			leave IL_done
		}
	IL_done:
		ldloc.1
		ret
	}

	// -------------------------------------------------------------------
	// conv.i and conv.u from a float64
	// -------------------------------------------------------------------

	// The fraction goes, the sign stays.
	.method public static int32 test_7_conv_i_from_r8 () cil managed
	{
		.maxstack 2
		ldc.r8 7.9
		call float64 ConvIL::IdR8 (float64)
		conv.i
		conv.i4
		ret
	}

	.method public static int32 test_1_conv_i_from_r8_negative () cil managed
	{
		.maxstack 3
		ldc.r8 -7.9
		call float64 ConvIL::IdR8 (float64)
		conv.i
		conv.i8
		ldc.i8 -7
		ceq
		ret
	}

	// A value too wide for four bytes, so a 32 bit conversion would lose it.
	.method public static int32 test_1_conv_i_from_r8_is_64_bit () cil managed
	{
		.maxstack 3
		ldc.r8 1e15
		call float64 ConvIL::IdR8 (float64)
		conv.i
		conv.i8
		ldc.i8 1000000000000000
		ceq
		ret
	}

	.method public static int32 test_1_conv_u_from_r8_is_64_bit () cil managed
	{
		.maxstack 3
		ldc.r8 1e15
		call float64 ConvIL::IdR8 (float64)
		conv.u
		conv.i8
		ldc.i8 1000000000000000
		ceq
		ret
	}

	// Above int64 max the result wraps into the sign bit, which is what
	// makes this the unsigned conversion.
	.method public static int32 test_1_conv_u_from_r8_above_int64 () cil managed
	{
		.maxstack 3
		ldc.r8 1e19
		call float64 ConvIL::IdR8 (float64)
		conv.u
		conv.i8
		ldc.i8 -8446744073709551616
		ceq
		ret
	}

	// -------------------------------------------------------------------
	// conv.i from an object and from a managed pointer
	// -------------------------------------------------------------------

	.method public static int32 test_1_conv_i_from_object () cil managed
	{
		.maxstack 3
		.locals init (object held)
		newobj instance void [mscorlib]System.Object::.ctor ()
		stloc.0
		ldloc.0
		conv.i
		conv.i8
		ldc.i8 0
		cgt.un
		ret
	}

	.method public static int32 test_42_conv_i_from_mp () cil managed
	{
		.maxstack 2
		.locals init (int32 held)
		ldc.i4 42
		stloc.0
		ldloca.s 0
		conv.i
		ldind.i4
		ret
	}

	// -------------------------------------------------------------------
	// conv.r.un on a value that is already a float
	// -------------------------------------------------------------------

	// The source is a float, so "unsigned" has nothing to act on and the
	// negative value stays negative.
	.method public static int32 test_1_conv_r_un_from_r8 () cil managed
	{
		.maxstack 3
		ldc.r8 -3.5
		call float64 ConvIL::IdR8 (float64)
		conv.r.un
		ldc.r8 -3.5
		ceq
		ret
	}

	// -------------------------------------------------------------------
	// conv.ovf.i.un and conv.ovf.u.un
	// -------------------------------------------------------------------

	.method public static int32 test_1_conv_ovf_i_un_from_r8 () cil managed
	{
		.maxstack 3
		ldc.r8 1e15
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.i.un
		conv.i8
		ldc.i8 1000000000000000
		ceq
		ret
	}

	// A float has no unsigned reading, so .un changes nothing here: -1.0
	// truncates to -1, which a native int holds.
	.method public static int32 test_1_conv_ovf_i_un_from_r8_negative () cil managed
	{
		.maxstack 3
		ldc.r8 -1.0
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.i.un
		conv.i8
		ldc.i8 -1
		ceq
		ret
	}

	// The float32 source of the same conversion, which had no arm at all.
	.method public static int32 test_1_conv_ovf_i_un_from_r4_negative () cil managed
	{
		.maxstack 3
		ldc.r4 -1.0
		call float32 ConvIL::IdR4 (float32)
		conv.ovf.i.un
		conv.i8
		ldc.i8 -1
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_u_un_from_r8 () cil managed
	{
		.maxstack 3
		ldc.r8 1e15
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.u.un
		conv.i8
		ldc.i8 1000000000000000
		ceq
		ret
	}

	// The source is read as unsigned, so a negative int32 widens to a value
	// with a clear sign bit rather than trapping.
	.method public static int32 test_1_conv_ovf_i_un_from_i4 () cil managed
	{
		.maxstack 3
		ldc.i4.m1
		call int32 ConvIL::Id4 (int32)
		conv.ovf.i.un
		conv.i8
		ldc.i8 0xFFFFFFFF
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_u_un_from_i4 () cil managed
	{
		.maxstack 3
		ldc.i4.m1
		call int32 ConvIL::Id4 (int32)
		conv.ovf.u.un
		conv.i8
		ldc.i8 0xFFFFFFFF
		ceq
		ret
	}

	// Read as unsigned the source is 2^64-1, which a signed native int cannot
	// hold.
	.method public static int32 test_1_conv_ovf_i_un_from_i8_traps () cil managed
	{
		.maxstack 3
		.locals init (int32 result)
		.try {
			ldc.i8 -1
			call int64 ConvIL::Id8 (int64)
			conv.ovf.i.un
			pop
			leave IL_done
		} catch [mscorlib]System.OverflowException {
			pop
			ldc.i4.1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	// -------------------------------------------------------------------
	// conv.ovf.i8.un and conv.ovf.u8.un
	// -------------------------------------------------------------------

	.method public static int32 test_1_conv_ovf_i8_un_from_r8 () cil managed
	{
		.maxstack 3
		ldc.r8 1e15
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.i8.un
		ldc.i8 1000000000000000
		ceq
		ret
	}

	// The float reading of .un again: -1.0 truncates to -1, which an int64 holds.
	.method public static int32 test_1_conv_ovf_i8_un_from_r8_negative () cil managed
	{
		.maxstack 3
		ldc.r8 -1.0
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.i8.un
		ldc.i8 -1
		ceq
		ret
	}

	// The same source under the unsigned destination, which -1 does not fit.
	.method public static int32 test_1_conv_ovf_u8_un_from_r8_negative_throws () cil managed
	{
		.maxstack 3
		.locals init (int32 result)
		.try {
			ldc.r8 -1.0
			call float64 ConvIL::IdR8 (float64)
			conv.ovf.u8.un
			pop
			leave IL_done
		} catch [mscorlib]System.OverflowException {
			pop
			ldc.i4.1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	// A value past int64 max that an unsigned destination still holds.
	.method public static int32 test_1_conv_ovf_u8_un_from_large_r8 () cil managed
	{
		.maxstack 3
		ldc.r8 1e19
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.u8.un
		ldc.i8 0x8AC7230489E80000
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_i8_un_from_r4 () cil managed
	{
		.maxstack 3
		ldc.r4 1e10
		call float32 ConvIL::IdR4 (float32)
		conv.ovf.i8.un
		ldc.i8 10000000000
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_u8_un_from_r4 () cil managed
	{
		.maxstack 3
		ldc.r4 1e10
		call float32 ConvIL::IdR4 (float32)
		conv.ovf.u8.un
		ldc.i8 10000000000
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_i8_un_from_i4 () cil managed
	{
		.maxstack 3
		ldc.i4.m1
		call int32 ConvIL::Id4 (int32)
		conv.ovf.i8.un
		ldc.i8 0xFFFFFFFF
		ceq
		ret
	}

	// The unsigned destination takes the same value unchanged.
	.method public static int32 test_1_conv_ovf_u8_un_from_i8 () cil managed
	{
		.maxstack 3
		ldc.i8 -1
		call int64 ConvIL::Id8 (int64)
		conv.ovf.u8.un
		ldc.i8 -1
		ceq
		ret
	}

	// -------------------------------------------------------------------
	// conv.ovf.i8 and conv.ovf.i from an integer
	//
	// csc emits neither of these over an integer source, because no such
	// conversion can overflow on a 64 bit target.
	// -------------------------------------------------------------------

	.method public static int32 test_1_conv_ovf_i8_from_i4 () cil managed
	{
		.maxstack 3
		ldc.i4.s -5
		call int32 ConvIL::Id4 (int32)
		conv.ovf.i8
		ldc.i8 -5
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_i8_from_i8 () cil managed
	{
		.maxstack 3
		ldc.i8 -5
		call int64 ConvIL::Id8 (int64)
		conv.ovf.i8
		ldc.i8 -5
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_i_from_i4 () cil managed
	{
		.maxstack 3
		ldc.i4.s -5
		call int32 ConvIL::Id4 (int32)
		conv.ovf.i
		conv.i8
		ldc.i8 -5
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_i_from_i8 () cil managed
	{
		.maxstack 3
		ldc.i8 -5
		call int64 ConvIL::Id8 (int64)
		conv.ovf.i
		conv.i8
		ldc.i8 -5
		ceq
		ret
	}

	// conv.ovf.i over a float, which csc does not emit either. The trapping
	// case says that the destination is checked and not only retyped.
	.method public static int32 test_1_conv_ovf_i_from_r8 () cil managed
	{
		.maxstack 3
		ldc.r8 1e15
		call float64 ConvIL::IdR8 (float64)
		conv.ovf.i
		conv.i8
		ldc.i8 1000000000000000
		ceq
		ret
	}

	.method public static int32 test_1_conv_ovf_i_from_r8_traps () cil managed
	{
		.maxstack 3
		.locals init (int32 result)
		.try {
			ldc.r8 1e30
			call float64 ConvIL::IdR8 (float64)
			conv.ovf.i
			pop
			leave IL_done
		} catch [mscorlib]System.OverflowException {
			pop
			ldc.i4.1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}
}
