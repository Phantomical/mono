using System;

// --llvm-opt=-mono-tier0-classic=ConvRUn puts the two conversions below on the
// classic compiler. An unsigned int32 to a float is `conv.r.un`, which amd64
// has neither a machine description entry nor a codegen case for:
// MONO_ARCH_EMULATE_CONV_R8_UN names it as one the emulation table answers,
// and a table missing it aborts the process out of mini-codegen.c.
//
// Each value has its top bit set, so a signed conversion answers a negative
// number rather than aborting.
public class Tier0ClassicConvRUnTest
{
	static double ConvRUnToDouble (uint value)
	{
		return (double) value;
	}

	static float ConvRUnToFloat (uint value)
	{
		return (float) value;
	}

	static double ConvRUnFromLong (ulong value)
	{
		return (double) value;
	}

	public static int Main ()
	{
		double d = ConvRUnToDouble (0xf0000000u);

		if (d != 4026531840.0) {
			Console.WriteLine ("FAIL: uint to double gave {0}", d);
			return 1;
		}

		float f = ConvRUnToFloat (0x80000000u);

		if (f != 2147483648.0f) {
			Console.WriteLine ("FAIL: uint to float gave {0}", f);
			return 1;
		}

		double l = ConvRUnFromLong (0x8000000000000000ul);

		if (l != 9223372036854775808.0) {
			Console.WriteLine ("FAIL: ulong to double gave {0}", l);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
