// The math opcodes that arithmetic.cs, math-rest.cs and mathintrins.cs leave
// alone.  There are two groups: div.i8 against a -1 divisor, and the operands
// that no ordinary value stands in for.
//
// The -1 divisor picks a guard arm in MINT_DIV_I8.  A signed zero, an infinity
// and a subnormal pick no arm, because each libm handler is one expression.
// They check the answer the handler gives rather than the path it takes.
//
// A test that makes more than one check returns the number of checks that hold.
//
// Operands come through NoInlining identity helpers.  A literal operand lets the
// transform fold the operation away, and then the opcode never runs.

using System;
using System.Runtime.CompilerServices;

[Instrumented]
public class MathTail {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long MtL (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double MtD (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static float MtF (float x) { return x; }

	static int Ok (bool held) { return held ? 1 : 0; }

	// MathF.Abs is one of the opcodes under test, so the tolerance takes a
	// magnitude with a sign test instead.
	static double MagD (double x) { return x < 0.0 ? -x : x; }

	static float MagF (float x) { return x < 0.0f ? -x : x; }

	static int NearD (double got, double want)
	{
		return MagD (got - want) <= 1e-12 * (MagD (want) + 1.0) ? 1 : 0;
	}

	static int NearF (float got, float want)
	{
		return MagF (got - want) <= 1e-6f * (MagF (want) + 1.0f) ? 1 : 0;
	}

	// A zero keeps its sign in IEEE, and == does not see that sign.
	static int PosZeroD (double x) { return Ok (x == 0.0 && 1.0 / x > 0.0); }

	static int NegZeroD (double x) { return Ok (x == 0.0 && 1.0 / x < 0.0); }

	static int NegZeroF (float x) { return Ok (x == 0.0f && 1.0f / x < 0.0f); }

	//
	// div on 64-bit operands with a -1 divisor.  MINT_DIV_I8 reads the dividend
	// only after it sees that divisor, so no other divisor reaches the overflow
	// arm.  math-rest.cs takes a 64-bit remainder by -1, and arithmetic.cs
	// divides at 64 bits, but no test divides by -1 at this width.
	//

	// ECMA-335 III.3.31: div overflows when the dividend is the most negative
	// number and the divisor is -1, because the quotient has no representation.
	public static int test_1_div_i8_min_value_by_minus_one_throws ()
	{
		long a = MtL (Int64.MinValue), b = MtL (-1);
		try {
			return (int) (a / b);
		} catch (OverflowException) {
			return 1;
		}
	}

	public static int test_3_div_i8_by_minus_one_negates ()
	{
		return Ok (MtL (7) / MtL (-1) == -7)
		     + Ok (MtL (-7) / MtL (-1) == 7)
		     + Ok (MtL (0) / MtL (-1) == 0);
	}

	// Both dividends meet the -1 divisor and neither one overflows.  The 32-bit
	// minimum has room at this width, and the 64-bit minimum plus one is the
	// nearest miss.
	public static int test_2_div_i8_near_misses_of_the_overflow ()
	{
		return Ok (MtL (Int32.MinValue) / MtL (-1) == 2147483648L)
		     + Ok (MtL (Int64.MinValue + 1) / MtL (-1) == Int64.MaxValue);
	}

	public static int test_2_div_i8_min_value_by_other_divisors ()
	{
		return Ok (MtL (Int64.MinValue) / MtL (1) == Int64.MinValue)
		     + Ok (MtL (Int64.MinValue) / MtL (-2) == 4611686018427387904L);
	}

	// The zero divisor comes first, so this pair never reaches the overflow arm.
	public static int test_1_div_i8_min_value_by_zero_throws ()
	{
		long a = MtL (Int64.MinValue), b = MtL (0);
		try {
			return (int) (a / b);
		} catch (DivideByZeroException) {
			return 1;
		}
	}

	//
	// A negative zero through the handlers that must keep its sign.  Ceiling
	// keeps it at the double width in mathintrins.cs, and these are the rest.
	//

	public static int test_4_sqrt_keeps_a_negative_zero ()
	{
		// A negative zero is the one negative operand that sqrt does not answer
		// with NaN.
		return NegZeroD (Math.Sqrt (MtD (-0.0)))
		     + NegZeroF (MathF.Sqrt (MtF (-0.0f)))
		     + PosZeroD (Math.Sqrt (MtD (0.0)))
		     + Ok (double.IsNaN (Math.Sqrt (MtD (-1e-320))));
	}

	public static int test_4_cbrt_keeps_a_negative_zero ()
	{
		return NegZeroD (Math.Cbrt (MtD (-0.0)))
		     + NegZeroF (MathF.Cbrt (MtF (-0.0f)))
		     + PosZeroD (Math.Cbrt (MtD (0.0)))
		     + Ok (float.IsPositiveInfinity (MathF.Cbrt (MtF (float.PositiveInfinity))));
	}

	public static int test_4_rounding_keeps_a_negative_zero ()
	{
		return NegZeroD (Math.Floor (MtD (-0.0)))
		     + NegZeroF (MathF.Floor (MtF (-0.0f)))
		     + NegZeroF (MathF.Ceiling (MtF (-0.5f)))
		     + PosZeroD (Math.Ceiling (MtD (0.0)));
	}

	public static int test_4_odd_functions_keep_a_negative_zero ()
	{
		// These four are odd functions, so a negative zero comes back negative.
		return NegZeroD (Math.Sin (MtD (-0.0)))
		     + NegZeroD (Math.Tan (MtD (-0.0)))
		     + NegZeroD (Math.Asin (MtD (-0.0)))
		     + NegZeroD (Math.Sinh (MtD (-0.0)));
	}

	public static int test_4_odd_functions_keep_a_negative_zero_r4 ()
	{
		return NegZeroF (MathF.Atan (MtF (-0.0f)))
		     + NegZeroF (MathF.Tanh (MtF (-0.0f)))
		     + NegZeroF (MathF.Asinh (MtF (-0.0f)))
		     + NegZeroF (MathF.Atanh (MtF (-0.0f)));
	}

	//
	// pow with an operand that no ordinary base and exponent reach.
	//

	public static int test_4_pow_identities_beat_a_nan ()
	{
		// IEEE 754 pow: a base of 1 gives 1 for every exponent, and an exponent
		// of 0 gives 1 for every base.  Both rules hold over NaN.
		return Ok (Math.Pow (MtD (1.0), MtD (double.NaN)) == 1.0)
		     + Ok (Math.Pow (MtD (double.NaN), MtD (0.0)) == 1.0)
		     + Ok (MathF.Pow (MtF (1.0f), MtF (float.NaN)) == 1.0f)
		     + Ok (MathF.Pow (MtF (float.NaN), MtF (0.0f)) == 1.0f);
	}

	public static int test_4_pow_with_an_infinite_exponent ()
	{
		// The magnitude of the base decides the answer, and a magnitude of 1
		// sits on the boundary and gives 1.
		return Ok (Math.Pow (MtD (-1.0), MtD (double.PositiveInfinity)) == 1.0)
		     + Ok (double.IsPositiveInfinity (Math.Pow (MtD (0.5), MtD (double.NegativeInfinity))))
		     + PosZeroD (Math.Pow (MtD (2.0), MtD (double.NegativeInfinity)))
		     + Ok (MathF.Pow (MtF (-1.0f), MtF (float.NegativeInfinity)) == 1.0f);
	}

	public static int test_4_pow_of_a_zero_base ()
	{
		// A negative exponent divides by the zero.  An odd integer exponent
		// carries the sign of the base into the answer.
		return Ok (double.IsPositiveInfinity (Math.Pow (MtD (0.0), MtD (-1.0))))
		     + Ok (double.IsNegativeInfinity (Math.Pow (MtD (-0.0), MtD (-3.0))))
		     + Ok (double.IsPositiveInfinity (Math.Pow (MtD (-0.0), MtD (-2.0))))
		     + NegZeroD (Math.Pow (MtD (-0.0), MtD (3.0)));
	}

	//
	// atan2 reads the sign of both operands, so a zero and an infinity pick
	// arms that mathintrins.cs does not reach.
	//

	public static int test_5_atan2_of_two_zeros ()
	{
		double nz = MtD (-0.0);
		return NearD (Math.Atan2 (MtD (0.0), nz), Math.PI)
		     + NearD (Math.Atan2 (nz, nz), -Math.PI)
		     + NegZeroD (Math.Atan2 (nz, MtD (1.0)))
		     + PosZeroD (Math.Atan2 (MtD (0.0), MtD (1.0)))
		     + NearD (Math.Atan2 (nz, MtD (-1.0)), -Math.PI);
	}

	public static int test_4_atan2_of_two_infinities ()
	{
		double inf = MtD (double.PositiveInfinity), ninf = MtD (double.NegativeInfinity);
		return NearD (Math.Atan2 (inf, inf), Math.PI / 4)
		     + NearD (Math.Atan2 (inf, ninf), 3 * Math.PI / 4)
		     + NearD (Math.Atan2 (ninf, inf), -Math.PI / 4)
		     + NearF (MathF.Atan2 (MtF (float.NegativeInfinity), MtF (float.NegativeInfinity)),
		              -3 * MathF.PI / 4);
	}

	//
	// Answers that mathintrins.cs asks of the double form only.  The float
	// handlers are separate opcodes and separate libm entry points.
	//

	public static int test_6_float_domain_edges ()
	{
		return Ok (float.IsPositiveInfinity (MathF.Atanh (MtF (1.0f))))
		     + Ok (float.IsNegativeInfinity (MathF.Atanh (MtF (-1.0f))))
		     + Ok (float.IsNaN (MathF.Atanh (MtF (2.0f))))
		     + Ok (float.IsNaN (MathF.Acosh (MtF (0.5f))))
		     + Ok (float.IsNaN (MathF.Acos (MtF (2.0f))))
		     + Ok (float.IsNegativeInfinity (MathF.Log10 (MtF (0.0f))));
	}

	public static int test_6_float_infinities ()
	{
		float inf = MtF (float.PositiveInfinity), ninf = MtF (float.NegativeInfinity);
		return Ok (MathF.Tanh (inf) == 1.0f)
		     + Ok (MathF.Tanh (ninf) == -1.0f)
		     + Ok (float.IsPositiveInfinity (MathF.Cosh (ninf)))
		     + Ok (float.IsNegativeInfinity (MathF.Sinh (ninf)))
		     + Ok (float.IsPositiveInfinity (MathF.Log (inf)))
		     + Ok (float.IsPositiveInfinity (MathF.Floor (inf)));
	}

	//
	// Subnormal operands.  The smallest positive double is 2 raised to -1074,
	// and the roots below divide that exponent exactly, so == is safe.
	//

	public static int test_4_subnormal_roots_are_exact ()
	{
		double d = MtD (double.Epsilon);
		double r = Math.Sqrt (d);
		double c = Math.Cbrt (d);

		return Ok (r * r == double.Epsilon)
		     + Ok (c * c * c == double.Epsilon)
		     + Ok (r > 0.0)
		     + Ok (Math.Sqrt (MtD (2.0 * double.Epsilon)) > r);
	}

	public static int test_4_subnormal_float_roots_are_exact ()
	{
		float s = MtF (2.0f * float.Epsilon);
		float r = MathF.Sqrt (s);

		return Ok (r * r == s)
		     + Ok (MathF.Floor (MtF (float.Epsilon)) == 0.0f)
		     + Ok (MathF.Ceiling (MtF (float.Epsilon)) == 1.0f)
		     + Ok (MathF.Abs (MtF (-float.Epsilon)) == float.Epsilon);
	}

	public static int test_4_subnormal_logs_and_exponents ()
	{
		// The log of the smallest double is about -744.44, and it is finite.
		double l = Math.Log (MtD (double.Epsilon));

		return Ok (l < -744.0 && l > -745.0)
		     + PosZeroD (Math.Exp (MtD (-800.0)))
		     + Ok (Math.Exp (MtD (-744.0)) > 0.0)
		     + Ok (MathF.Log (MtF (float.Epsilon)) < -103.0f);
	}
}
