// Measures Mono.Simd.Vector4f arithmetic (SimdMatMul:Multiply) under the
// vector-IR lowering the backend writes for it. Not a correctness test: a run
// only proves the kernel executes and reports its own timing.
//
// Multiply has no loop of its own, so it promotes out of tier 0 purely on
// call count. The warmup phase clears that threshold well before the
// measured phase starts. CMakeLists.txt pins tier 2 off, so the measured
// phase stays at tier 1, where the lowering folds into its caller.

using System;
using Mono.Simd;

class SimdMatMul
{
	struct Mat4
	{
		public Vector4f R0, R1, R2, R3;
	}

	static Mat4 Multiply (ref Mat4 a, ref Mat4 b)
	{
		Mat4 r;
		r.R0 = a.R0.X * b.R0 + a.R0.Y * b.R1 + a.R0.Z * b.R2 + a.R0.W * b.R3;
		r.R1 = a.R1.X * b.R0 + a.R1.Y * b.R1 + a.R1.Z * b.R2 + a.R1.W * b.R3;
		r.R2 = a.R2.X * b.R0 + a.R2.Y * b.R1 + a.R2.Z * b.R2 + a.R2.W * b.R3;
		r.R3 = a.R3.X * b.R0 + a.R3.Y * b.R1 + a.R3.Z * b.R2 + a.R3.W * b.R3;
		return r;
	}

	static int Main (string[] args)
	{
		int warmup = args.Length > 0 ? int.Parse (args[0]) : 200000;
		int measured = args.Length > 1 ? int.Parse (args[1]) : 2000000;

		var a = new Mat4 {
			R0 = new Vector4f (1, 2, 3, 4),
			R1 = new Vector4f (5, 6, 7, 8),
			R2 = new Vector4f (9, 10, 11, 12),
			R3 = new Vector4f (13, 14, 15, 16),
		};
		var b = new Mat4 {
			R0 = new Vector4f (0.1f, 0.2f, 0.3f, 0.4f),
			R1 = new Vector4f (0.5f, 0.6f, 0.7f, 0.8f),
			R2 = new Vector4f (0.9f, 1.0f, 1.1f, 1.2f),
			R3 = new Vector4f (1.3f, 1.4f, 1.5f, 1.6f),
		};
		Mat4 acc = a;

		for (int i = 0; i < warmup; i++) {
			acc = Multiply (ref acc, ref b);
			// b's entries grow the product by roughly 100x per multiply.
			// Resetting every 16 iterations keeps it well short of
			// float.MaxValue and a run of pure Infinity arithmetic.
			if ((i & 0xF) == 0)
				acc = a;
		}

		DateTime start = DateTime.Now;
		for (int i = 0; i < measured; i++) {
			acc = Multiply (ref acc, ref b);
			if ((i & 0xF) == 0)
				acc = a;
		}
		double elapsedMs = (DateTime.Now - start).TotalMilliseconds;

		Console.WriteLine ("checksum=" + (acc.R0.X + acc.R1.Y + acc.R2.Z + acc.R3.W));
		Console.WriteLine ("measured_iters=" + measured);
		Console.WriteLine ("elapsed_ms=" + elapsedMs);
		Console.WriteLine ("ns_per_iter=" + (elapsedMs * 1e6 / measured));
		return 0;
	}
}
