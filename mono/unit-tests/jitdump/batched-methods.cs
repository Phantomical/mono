// Enough distinct methods to fill a tier-1 compile batch, so that the jit dump
// this run writes holds records for several methods of one linked object.
//
// Each instantiation over a value type gets a body of its own, which is how one
// short source reaches that many methods.  Every body carries a loop, because
// the trivial-inline pre-pass refuses control flow.  A body it folds in leaves
// no record of its own.

using System;

struct K0 { }
struct K1 { }
struct K2 { }
struct K3 { }
struct K4 { }
struct K5 { }
struct K6 { }
struct K7 { }
struct K8 { }
struct K9 { }
struct K10 { }
struct K11 { }
struct K12 { }
struct K13 { }
struct K14 { }
struct K15 { }

static class Work<T> {
	public static int Sum (int n)
	{
		int total = 0;

		for (int i = 0; i < n; i++)
			total += i * 3 + (i >> 1);
		return total;
	}

	public static int Mix (int n)
	{
		int total = 1;

		for (int i = 1; i <= n; i++)
			total = (total * 31 + i) % 1000003;
		return total;
	}

	public static int Fold (int n)
	{
		int total = n;

		for (int i = 0; i < n; i++)
			total = total < 0 ? total + i : total - i;
		return total;
	}

	public static int Scan (int n)
	{
		int total = 0;

		for (int i = 0; i < n; i++)
			for (int j = 0; j < 3; j++)
				total += (i ^ j) + 1;
		return total;
	}
}

class Driver {
	static int Run<T> ()
	{
		int acc = 0;

		for (int i = 0; i < 400; i++)
			acc += Work<T>.Sum (7) + Work<T>.Mix (7) + Work<T>.Fold (7)
			       + Work<T>.Scan (7);
		return acc;
	}

	static void Main ()
	{
		int acc = Run<K0> () + Run<K1> () + Run<K2> () + Run<K3> ()
		          + Run<K4> () + Run<K5> () + Run<K6> () + Run<K7> ()
		          + Run<K8> () + Run<K9> () + Run<K10> () + Run<K11> ()
		          + Run<K12> () + Run<K13> () + Run<K14> () + Run<K15> ();

		Console.WriteLine (acc);
	}
}
