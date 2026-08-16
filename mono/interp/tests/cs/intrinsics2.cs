// The intrinsic sites that mono/interp/tests/intrinsics.cs leaves alone:
// Debugger.Break, the three-argument arm of the Math table, the element store a
// rectangular array's Set turns into, and the two memory barriers.
//
// Operands come through NoInlining helpers.  The transform folds constants and
// inlines small callees, and a folded operand leaves the opcode untested.

using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Threading;

public class Intrinsics2 {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T Keep<T> (T x) { return x; }

	// Debugger.Break.  The transform asks the break policy, which answers yes
	// when no policy was installed, so the call becomes MINT_BREAK.  The opcode
	// calls the debugger agent, and the agent does nothing while it is off.

	public static int test_7_debugger_break_keeps_the_locals ()
	{
		int a = Id (3), b = Id (4);

		Debugger.Break ();
		return a + b;
	}

	public static int test_0_no_debugger_is_attached ()
	{
		// The premise of the test above: with an agent attached, MINT_BREAK
		// stops the thread instead of returning.
		return Debugger.IsAttached ? 1 : 0;
	}

	// Math.Clamp is the only three-double method in Math here, so it is the only
	// call that reaches the arm that looks for FusedMultiplyAdd.  Neither name resolves to
	// an opcode, so both calls stay ordinary calls.

	public static int test_2_math_clamp_double_below_the_minimum ()
	{
		return (int) Math.Clamp (Keep (1.0), Keep (2.0), Keep (8.0));
	}

	public static int test_3_math_clamp_float_stops_before_the_arm ()
	{
		// The arm reads the parameter type as well as the count, and Math asks
		// for doubles, so the float overload never gets that far.
		return (int) Math.Clamp (Keep (3.0f), Keep (1.0f), Keep (9.0f));
	}

	// A rectangular array's Set becomes element address arithmetic and a store,
	// and the store opcode comes from the element type.  One test per width.
	// Each test writes the neighbouring element first and reads it back after,
	// so the width of the store is under test as well as its value.

	public static int test_9_array2d_set_u1 ()
	{
		byte[,] a = new byte[2, 2];

		a[Id (1), Id (1)] = Keep<byte> (5);
		a[Id (1), Id (0)] = Keep<byte> (9);
		return a[1, 1] == 5 ? a[1, 0] : 0;
	}

	public static int test_3_array2d_set_i1 ()
	{
		sbyte[,] a = new sbyte[2, 2];

		a[Id (1), Id (1)] = Keep<sbyte> (5);
		a[Id (1), Id (0)] = Keep<sbyte> (-3);
		// The load sign extends, so an unsigned load reads 253 and negates wrong.
		return a[1, 1] == 5 ? -a[1, 0] : 0;
	}

	public static int test_5_array2d_set_i2 ()
	{
		short[,] a = new short[2, 2];

		a[Id (0), Id (1)] = Keep<short> (7);
		a[Id (0), Id (0)] = Keep<short> (-5);
		return a[0, 1] == 7 ? -a[0, 0] : 0;
	}

	public static int test_65346_array2d_set_u2 ()
	{
		char[,] a = new char[2, 2];

		a[Id (1), Id (1)] = Keep ('z');
		a[Id (1), Id (0)] = Keep ('\uff42');
		// 0xff42 has the top bit set, so a signed load reads -190.
		return a[1, 1] == 'z' ? a[1, 0] : 0;
	}

	public static int test_7_array2d_set_i8 ()
	{
		long[,] a = new long[2, 2];

		// A four byte store keeps the low word only, so the high word says the
		// store was eight bytes wide.
		a[Id (0), Id (1)] = Keep (0x300000007L);
		return a[0, 1] == 0x300000007L ? 7 : 0;
	}

	public static int test_4_array2d_set_r4 ()
	{
		float[,] a = new float[2, 2];

		a[Id (1), Id (1)] = Keep (2.5f);
		a[Id (1), Id (0)] = Keep (4.25f);
		return a[1, 1] == 2.5f ? (int) a[1, 0] : 0;
	}

	public static int test_6_array2d_set_r8 ()
	{
		double[,] a = new double[2, 2];

		// Every set bit of 6.5 is in the high word, so a four byte store leaves
		// the element at zero.
		a[Id (0), Id (0)] = Keep (6.5);
		return a[0, 0] == 6.5 ? 6 : 0;
	}

	// Memory barriers.  Both names carry the same opcode, and the opcode has to
	// leave the locals beside it alone.

	public static int test_3_interlocked_memory_barrier ()
	{
		int a = Id (1), b = Id (2);

		Interlocked.MemoryBarrier ();
		return a + b;
	}

	public static int test_3_thread_memory_barrier ()
	{
		int a = Id (1), b = Id (2);

		Thread.MemoryBarrier ();
		return a + b;
	}
}
