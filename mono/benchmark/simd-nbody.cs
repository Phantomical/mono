// Measures System.Numerics.Vector4 arithmetic (SimdNBody:Step) under the
// vector-IR lowering the backend writes for it. Not a correctness test: a run
// only proves the kernel executes and reports its own timing.
//
// Step's O(N^2) inner loop accumulates enough backward-branch weight to
// promote out of tier 0 within the warmup phase. CMakeLists.txt pins tier 2
// off, so the measured phase stays at tier 1, where the lowering folds into
// its caller.

using System;
using System.Numerics;

class SimdNBody
{
	const int N = 128;
	static Vector4[] pos = new Vector4[N];
	static Vector4[] vel = new Vector4[N];
	static float[] mass = new float[N];

	static void Init ()
	{
		var rnd = new Random (42);
		for (int i = 0; i < N; i++) {
			pos[i] = new Vector4 (
				(float) rnd.NextDouble () * 100f,
				(float) rnd.NextDouble () * 100f,
				(float) rnd.NextDouble () * 100f,
				0f);
			vel[i] = Vector4.Zero;
			mass[i] = 1f + (float) rnd.NextDouble ();
		}
	}

	static void Step (float dt)
	{
		const float softening = 0.1f;
		for (int i = 0; i < N; i++) {
			Vector4 acc = Vector4.Zero;
			Vector4 pi = pos[i];
			for (int j = 0; j < N; j++) {
				if (j == i)
					continue;
				Vector4 d = pos[j] - pi;
				float distSq = Vector4.Dot (d, d) + softening;
				float invDist = 1f / (float) Math.Sqrt (distSq);
				float invDist3 = invDist * invDist * invDist;
				acc += d * (mass[j] * invDist3);
			}
			vel[i] += acc * dt;
		}
		for (int i = 0; i < N; i++)
			pos[i] += vel[i] * dt;
	}

	static double Checksum ()
	{
		double s = 0;
		for (int i = 0; i < N; i++)
			s += pos[i].X + pos[i].Y + pos[i].Z;
		return s;
	}

	static int Main (string[] args)
	{
		int warmup = args.Length > 0 ? int.Parse (args[0]) : 1500;
		int measured = args.Length > 1 ? int.Parse (args[1]) : 1000;
		float dt = 0.001f;

		Init ();

		for (int i = 0; i < warmup; i++)
			Step (dt);

		DateTime start = DateTime.Now;
		for (int i = 0; i < measured; i++)
			Step (dt);
		double elapsedMs = (DateTime.Now - start).TotalMilliseconds;

		Console.WriteLine ("checksum=" + Checksum ());
		Console.WriteLine ("measured_steps=" + measured);
		Console.WriteLine ("elapsed_ms=" + elapsedMs);
		Console.WriteLine ("us_per_step=" + (elapsedMs * 1000.0 / measured));
		return 0;
	}
}
