// The Math and MathF calls that the transform turns into interpreter opcodes.
// MINT_ABS and its family are the double form, MINT_ABSF and its family the
// float form.  Round, Min and Max stay ordinary calls.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Each test returns the number of checks that hold, so a failure says how many
// of them were good.
//
// Operands come through NoInlining identity helpers.  A literal operand lets
// the transform fold the call away, and then the opcode never runs.

using System;
using System.Runtime.CompilerServices;

public class MathIntrins {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double D (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static float F (float x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int I (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long L (long x) { return x; }

	// Math.Abs is one of the opcodes under test, so the tolerance takes a
	// magnitude with a sign test instead.  A NaN stays NaN through MagD and
	// then fails the tolerance, which is the correct result.
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

	static int Ok (bool held) { return held ? 1 : 0; }

	// A zero keeps its sign in IEEE, and == does not see that sign.
	static int PosZero (double x) { return Ok (x == 0.0 && 1.0 / x > 0.0); }

	static int NegZero (double x) { return Ok (x == 0.0 && 1.0 / x < 0.0); }

	//
	// Double form.
	//

	public static int test_5_abs_r8 ()
	{
		return NearD (Math.Abs (D (-3.5)), 3.5)
		     + NearD (Math.Abs (D (3.5)), 3.5)
		     + Ok (double.IsPositiveInfinity (Math.Abs (D (double.NegativeInfinity))))
		     + Ok (double.IsNaN (Math.Abs (D (double.NaN))))
		     + PosZero (Math.Abs (D (-0.0)));
	}

	public static int test_4_sqrt_r8 ()
	{
		return NearD (Math.Sqrt (D (4.0)), 2.0)
		     + NearD (Math.Sqrt (D (0.0)), 0.0)
		     + Ok (double.IsNaN (Math.Sqrt (D (-1.0))))
		     + Ok (double.IsPositiveInfinity (Math.Sqrt (D (double.PositiveInfinity))));
	}

	public static int test_4_asin_r8 ()
	{
		return NearD (Math.Asin (D (0.0)), 0.0)
		     + NearD (Math.Asin (D (1.0)), Math.PI / 2)
		     + NearD (Math.Asin (D (-1.0)), -Math.PI / 2)
		     + Ok (double.IsNaN (Math.Asin (D (2.0))));
	}

	public static int test_4_acos_r8 ()
	{
		return NearD (Math.Acos (D (1.0)), 0.0)
		     + NearD (Math.Acos (D (0.0)), Math.PI / 2)
		     + NearD (Math.Acos (D (-1.0)), Math.PI)
		     + Ok (double.IsNaN (Math.Acos (D (2.0))));
	}

	public static int test_4_atan_r8 ()
	{
		return NearD (Math.Atan (D (0.0)), 0.0)
		     + NearD (Math.Atan (D (1.0)), Math.PI / 4)
		     + NearD (Math.Atan (D (-1.0)), -Math.PI / 4)
		     + NearD (Math.Atan (D (double.PositiveInfinity)), Math.PI / 2);
	}

	public static int test_4_asinh_r8 ()
	{
		return NearD (Math.Asinh (D (0.0)), 0.0)
		     + NearD (Math.Asinh (D (1.0)), 0.881373587019543)
		     + NearD (Math.Asinh (D (-1.0)), -0.881373587019543)
		     + Ok (double.IsPositiveInfinity (Math.Asinh (D (double.PositiveInfinity))));
	}

	public static int test_4_acosh_r8 ()
	{
		return NearD (Math.Acosh (D (1.0)), 0.0)
		     + NearD (Math.Acosh (D (2.0)), 1.3169578969248166)
		     + Ok (double.IsNaN (Math.Acosh (D (0.5))))
		     + Ok (double.IsPositiveInfinity (Math.Acosh (D (double.PositiveInfinity))));
	}

	public static int test_4_atanh_r8 ()
	{
		return NearD (Math.Atanh (D (0.0)), 0.0)
		     + NearD (Math.Atanh (D (0.5)), 0.5493061443340548)
		     + Ok (double.IsPositiveInfinity (Math.Atanh (D (1.0))))
		     + Ok (double.IsNaN (Math.Atanh (D (2.0))));
	}

	public static int test_5_ceiling_r8 ()
	{
		// ceil (-0.5) is negative zero.
		return NearD (Math.Ceiling (D (1.2)), 2.0)
		     + NearD (Math.Ceiling (D (-1.2)), -1.0)
		     + NearD (Math.Ceiling (D (2.0)), 2.0)
		     + NegZero (Math.Ceiling (D (-0.5)))
		     + Ok (double.IsNaN (Math.Ceiling (D (double.NaN))));
	}

	public static int test_5_floor_r8 ()
	{
		return NearD (Math.Floor (D (1.8)), 1.0)
		     + NearD (Math.Floor (D (-1.2)), -2.0)
		     + NearD (Math.Floor (D (-2.0)), -2.0)
		     + NearD (Math.Floor (D (0.5)), 0.0)
		     + Ok (double.IsPositiveInfinity (Math.Floor (D (double.PositiveInfinity))));
	}

	public static int test_4_cos_r8 ()
	{
		return NearD (Math.Cos (D (0.0)), 1.0)
		     + NearD (Math.Cos (D (Math.PI)), -1.0)
		     + NearD (Math.Cos (D (Math.PI / 3)), 0.5)
		     + Ok (double.IsNaN (Math.Cos (D (double.NaN))));
	}

	public static int test_4_sin_r8 ()
	{
		return NearD (Math.Sin (D (0.0)), 0.0)
		     + NearD (Math.Sin (D (Math.PI / 2)), 1.0)
		     + NearD (Math.Sin (D (Math.PI / 6)), 0.5)
		     + NearD (Math.Sin (D (-Math.PI / 2)), -1.0);
	}

	public static int test_4_tan_r8 ()
	{
		return NearD (Math.Tan (D (0.0)), 0.0)
		     + NearD (Math.Tan (D (Math.PI / 4)), 1.0)
		     + NearD (Math.Tan (D (-Math.PI / 4)), -1.0)
		     + Ok (double.IsNaN (Math.Tan (D (double.NaN))));
	}

	public static int test_4_cosh_r8 ()
	{
		return NearD (Math.Cosh (D (0.0)), 1.0)
		     + NearD (Math.Cosh (D (1.0)), 1.5430806348152437)
		     + NearD (Math.Cosh (D (-1.0)), 1.5430806348152437)
		     + Ok (double.IsPositiveInfinity (Math.Cosh (D (double.NegativeInfinity))));
	}

	public static int test_4_sinh_r8 ()
	{
		return NearD (Math.Sinh (D (0.0)), 0.0)
		     + NearD (Math.Sinh (D (1.0)), 1.1752011936438014)
		     + NearD (Math.Sinh (D (-1.0)), -1.1752011936438014)
		     + Ok (double.IsNegativeInfinity (Math.Sinh (D (double.NegativeInfinity))));
	}

	public static int test_4_tanh_r8 ()
	{
		return NearD (Math.Tanh (D (0.0)), 0.0)
		     + NearD (Math.Tanh (D (1.0)), 0.7615941559557649)
		     + NearD (Math.Tanh (D (double.PositiveInfinity)), 1.0)
		     + NearD (Math.Tanh (D (double.NegativeInfinity)), -1.0);
	}

	public static int test_4_exp_r8 ()
	{
		return NearD (Math.Exp (D (0.0)), 1.0)
		     + NearD (Math.Exp (D (1.0)), Math.E)
		     + NearD (Math.Exp (D (double.NegativeInfinity)), 0.0)
		     + Ok (double.IsPositiveInfinity (Math.Exp (D (double.PositiveInfinity))));
	}

	public static int test_5_log_r8 ()
	{
		return NearD (Math.Log (D (1.0)), 0.0)
		     + NearD (Math.Log (D (Math.E)), 1.0)
		     + Ok (double.IsNegativeInfinity (Math.Log (D (0.0))))
		     + Ok (double.IsNaN (Math.Log (D (-1.0))))
		     + Ok (double.IsPositiveInfinity (Math.Log (D (double.PositiveInfinity))));
	}

	public static int test_5_log10_r8 ()
	{
		return NearD (Math.Log10 (D (1.0)), 0.0)
		     + NearD (Math.Log10 (D (1000.0)), 3.0)
		     + NearD (Math.Log10 (D (0.001)), -3.0)
		     + Ok (double.IsNegativeInfinity (Math.Log10 (D (0.0))))
		     + Ok (double.IsNaN (Math.Log10 (D (-1.0))));
	}

	public static int test_4_cbrt_r8 ()
	{
		return NearD (Math.Cbrt (D (27.0)), 3.0)
		     + NearD (Math.Cbrt (D (-8.0)), -2.0)
		     + NearD (Math.Cbrt (D (0.0)), 0.0)
		     + Ok (double.IsNegativeInfinity (Math.Cbrt (D (double.NegativeInfinity))));
	}

	public static int test_5_atan2_r8 ()
	{
		return NearD (Math.Atan2 (D (0.0), D (1.0)), 0.0)
		     + NearD (Math.Atan2 (D (1.0), D (1.0)), Math.PI / 4)
		     + NearD (Math.Atan2 (D (1.0), D (0.0)), Math.PI / 2)
		     + NearD (Math.Atan2 (D (0.0), D (-1.0)), Math.PI)
		     + NearD (Math.Atan2 (D (-1.0), D (-1.0)), -3 * Math.PI / 4);
	}

	public static int test_6_pow_r8 ()
	{
		// A negative base with a non-integral exponent gives NaN.  1.0 / 3.0 is
		// not the cube root it reads as.
		return NearD (Math.Pow (D (2.0), D (10.0)), 1024.0)
		     + NearD (Math.Pow (D (2.0), D (0.5)), 1.4142135623730951)
		     + NearD (Math.Pow (D (0.0), D (0.0)), 1.0)
		     + NearD (Math.Pow (D (-2.0), D (3.0)), -8.0)
		     + Ok (double.IsNaN (Math.Pow (D (-8.0), D (1.0 / 3.0))))
		     + NearD (Math.Pow (D (double.PositiveInfinity), D (0.0)), 1.0);
	}

	//
	// Float form.  The transform takes the operand type from the class name, so
	// only MathF reaches the MINT_*F opcodes.  The float overloads on Math stay
	// ordinary calls.
	//

	public static int test_5_abs_r4 ()
	{
		return NearF (MathF.Abs (F (-3.5f)), 3.5f)
		     + NearF (MathF.Abs (F (3.5f)), 3.5f)
		     + Ok (float.IsPositiveInfinity (MathF.Abs (F (float.NegativeInfinity))))
		     + Ok (float.IsNaN (MathF.Abs (F (float.NaN))))
		     + PosZero (MathF.Abs (F (-0.0f)));
	}

	public static int test_4_sqrt_r4 ()
	{
		return NearF (MathF.Sqrt (F (4.0f)), 2.0f)
		     + NearF (MathF.Sqrt (F (0.0f)), 0.0f)
		     + Ok (float.IsNaN (MathF.Sqrt (F (-1.0f))))
		     + Ok (float.IsPositiveInfinity (MathF.Sqrt (F (float.PositiveInfinity))));
	}

	public static int test_6_inverse_trig_r4 ()
	{
		return NearF (MathF.Asin (F (0.0f)), 0.0f)
		     + NearF (MathF.Asin (F (1.0f)), MathF.PI / 2)
		     + NearF (MathF.Acos (F (1.0f)), 0.0f)
		     + NearF (MathF.Acos (F (-1.0f)), MathF.PI)
		     + NearF (MathF.Atan (F (1.0f)), MathF.PI / 4)
		     + Ok (float.IsNaN (MathF.Asin (F (2.0f))));
	}

	public static int test_6_inverse_hyperbolic_r4 ()
	{
		return NearF (MathF.Asinh (F (0.0f)), 0.0f)
		     + NearF (MathF.Asinh (F (1.0f)), 0.8813736f)
		     + NearF (MathF.Acosh (F (1.0f)), 0.0f)
		     + NearF (MathF.Acosh (F (2.0f)), 1.3169579f)
		     + NearF (MathF.Atanh (F (0.0f)), 0.0f)
		     + NearF (MathF.Atanh (F (0.5f)), 0.54930615f);
	}

	public static int test_6_trig_r4 ()
	{
		return NearF (MathF.Sin (F (0.0f)), 0.0f)
		     + NearF (MathF.Sin (F (MathF.PI / 2)), 1.0f)
		     + NearF (MathF.Cos (F (0.0f)), 1.0f)
		     + NearF (MathF.Cos (F (MathF.PI)), -1.0f)
		     + NearF (MathF.Tan (F (0.0f)), 0.0f)
		     + NearF (MathF.Tan (F (MathF.PI / 4)), 1.0f);
	}

	public static int test_6_hyperbolic_r4 ()
	{
		return NearF (MathF.Sinh (F (0.0f)), 0.0f)
		     + NearF (MathF.Sinh (F (1.0f)), 1.1752012f)
		     + NearF (MathF.Cosh (F (0.0f)), 1.0f)
		     + NearF (MathF.Cosh (F (-1.0f)), 1.5430806f)
		     + NearF (MathF.Tanh (F (0.0f)), 0.0f)
		     + NearF (MathF.Tanh (F (1.0f)), 0.7615942f);
	}

	public static int test_6_ceiling_floor_r4 ()
	{
		return NearF (MathF.Ceiling (F (1.2f)), 2.0f)
		     + NearF (MathF.Ceiling (F (-1.2f)), -1.0f)
		     + NearF (MathF.Ceiling (F (2.0f)), 2.0f)
		     + NearF (MathF.Floor (F (1.8f)), 1.0f)
		     + NearF (MathF.Floor (F (-1.2f)), -2.0f)
		     + NearF (MathF.Floor (F (-2.0f)), -2.0f);
	}

	public static int test_4_exp_r4 ()
	{
		return NearF (MathF.Exp (F (0.0f)), 1.0f)
		     + NearF (MathF.Exp (F (1.0f)), MathF.E)
		     + NearF (MathF.Exp (F (float.NegativeInfinity)), 0.0f)
		     + Ok (float.IsPositiveInfinity (MathF.Exp (F (float.PositiveInfinity))));
	}

	public static int test_6_log_r4 ()
	{
		return NearF (MathF.Log (F (1.0f)), 0.0f)
		     + NearF (MathF.Log (F (MathF.E)), 1.0f)
		     + Ok (float.IsNegativeInfinity (MathF.Log (F (0.0f))))
		     + NearF (MathF.Log10 (F (1.0f)), 0.0f)
		     + NearF (MathF.Log10 (F (1000.0f)), 3.0f)
		     + Ok (float.IsNaN (MathF.Log10 (F (-1.0f))));
	}

	public static int test_4_cbrt_r4 ()
	{
		return NearF (MathF.Cbrt (F (27.0f)), 3.0f)
		     + NearF (MathF.Cbrt (F (-8.0f)), -2.0f)
		     + NearF (MathF.Cbrt (F (0.0f)), 0.0f)
		     + Ok (float.IsNegativeInfinity (MathF.Cbrt (F (float.NegativeInfinity))));
	}

	public static int test_5_atan2_r4 ()
	{
		return NearF (MathF.Atan2 (F (0.0f), F (1.0f)), 0.0f)
		     + NearF (MathF.Atan2 (F (1.0f), F (1.0f)), MathF.PI / 4)
		     + NearF (MathF.Atan2 (F (1.0f), F (0.0f)), MathF.PI / 2)
		     + NearF (MathF.Atan2 (F (0.0f), F (-1.0f)), MathF.PI)
		     + NearF (MathF.Atan2 (F (-1.0f), F (-1.0f)), -3 * MathF.PI / 4);
	}

	public static int test_5_pow_r4 ()
	{
		return NearF (MathF.Pow (F (2.0f), F (10.0f)), 1024.0f)
		     + NearF (MathF.Pow (F (2.0f), F (0.5f)), 1.4142135f)
		     + NearF (MathF.Pow (F (0.0f), F (0.0f)), 1.0f)
		     + NearF (MathF.Pow (F (-2.0f), F (3.0f)), -8.0f)
		     + Ok (float.IsNaN (MathF.Pow (F (-8.0f), F (1.0f / 3.0f))));
	}

	public static int test_5_float_range ()
	{
		// The float opcodes hold a float result, so they overflow and underflow
		// where the double form still has range left.  sqrt is correctly
		// rounded at both widths, so the two agree bit for bit.
		return Ok (float.IsPositiveInfinity (MathF.Exp (F (100.0f))))
		     + Ok (!double.IsInfinity (Math.Exp (D (100.0))))
		     + Ok (float.IsPositiveInfinity (MathF.Cosh (F (100.0f))))
		     + Ok (MathF.Exp (F (-200.0f)) == 0.0f)
		     + Ok (MathF.Sqrt (F (2.0f)) == (float) Math.Sqrt (D (2.0)));
	}

	//
	// Round, Min and Max.  The transform has no opcode for these, so they stay
	// ordinary calls.
	//

	public static int test_4_round_r8 ()
	{
		// Math.Round is round-half-to-even, so 2.5 goes down and 3.5 goes up.
		return NearD (Math.Round (D (2.5)), 2.0)
		     + NearD (Math.Round (D (3.5)), 4.0)
		     + NearD (Math.Round (D (-2.5)), -2.0)
		     + NearD (Math.Round (D (1.4)), 1.0);
	}

	public static int test_4_round_r4 ()
	{
		return NearF (MathF.Round (F (2.5f)), 2.0f)
		     + NearF (MathF.Round (F (3.5f)), 4.0f)
		     + NearF (MathF.Round (F (-2.5f)), -2.0f)
		     + NearF (MathF.Round (F (1.4f)), 1.0f);
	}

	public static int test_6_min_max_r8 ()
	{
		return NearD (Math.Min (D (1.0), D (2.0)), 1.0)
		     + NearD (Math.Max (D (1.0), D (2.0)), 2.0)
		     + Ok (double.IsNaN (Math.Min (D (double.NaN), D (1.0))))
		     + Ok (double.IsNaN (Math.Max (D (1.0), D (double.NaN))))
		     + Ok (double.IsNegativeInfinity (Math.Min (D (double.NegativeInfinity), D (0.0))))
		     + Ok (double.IsPositiveInfinity (Math.Max (D (double.PositiveInfinity), D (0.0))));
	}

	public static int test_6_min_max_r4 ()
	{
		return NearF (Math.Min (F (1.0f), F (2.0f)), 1.0f)
		     + NearF (Math.Max (F (1.0f), F (2.0f)), 2.0f)
		     + Ok (float.IsNaN (Math.Min (F (float.NaN), F (1.0f))))
		     + Ok (float.IsNaN (Math.Max (F (1.0f), F (float.NaN))))
		     + Ok (float.IsNegativeInfinity (Math.Min (F (float.NegativeInfinity), F (0.0f))))
		     + Ok (float.IsPositiveInfinity (Math.Max (F (float.PositiveInfinity), F (0.0f))));
	}

	public static int test_4_min_max_int ()
	{
		return Ok (Math.Min (I (-3), I (2)) == -3)
		     + Ok (Math.Max (I (-3), I (2)) == 2)
		     + Ok (Math.Min (L (-3), L (2)) == -3)
		     + Ok (Math.Max (L (int.MaxValue + 1L), L (2)) == int.MaxValue + 1L);
	}

	public static int test_4_abs_int ()
	{
		int overflowed = 0;
		try {
			Math.Abs (I (int.MinValue));
		} catch (OverflowException) {
			overflowed = 1;
		}

		return Ok (Math.Abs (I (-5)) == 5)
		     + Ok (Math.Abs (I (5)) == 5)
		     + Ok (Math.Abs (L (-5L)) == 5L)
		     + overflowed;
	}

	public static int test_5_nan_in_nan_out ()
	{
		return Ok (double.IsNaN (Math.Pow (D (double.NaN), D (2.0))))
		     + Ok (double.IsNaN (Math.Exp (D (double.NaN))))
		     + Ok (double.IsNaN (Math.Sqrt (D (double.NaN))))
		     + Ok (double.IsNaN (Math.Floor (D (double.NaN))))
		     + Ok (float.IsNaN (MathF.Sin (F (float.NaN))));
	}
}
