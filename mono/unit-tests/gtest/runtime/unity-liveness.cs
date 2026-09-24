/*
 * The managed half of test-unity-liveness.cpp. Every Build method returns how
 * many distinct Tracked instances it made reachable from LivenessRoots'
 * statics.
 */
using System.Collections.Generic;

public class Tracked
{
	public Tracked next;
	public object payload;
}

public struct Pair
{
	public Tracked a;
	public int x;
	public Tracked b;
}

public class Holder<T>
{
	public T value;
}

public class Part
{
	public List<Module> modules = new List<Module> ();
	public Tracked tag = new Tracked ();
	public string name = "part";
	public double[] samples = new double[8];
}

public class Module
{
	public Part part;
	public object[] state = new object[4];
	public Dictionary<string, Tracked> fields = new Dictionary<string, Tracked> ();
}

public class LivenessRoots
{
	static Tracked[] array;
	static Tracked chain;
	static Pair[] pairs;
	static Pair single;
	static Dictionary<int, Tracked> map;
	static object[] shared;
	static Holder<Tracked> holder;
	static List<Part> vessels;

	public static int Build (int scale)
	{
		int count = 0;

		array = new Tracked[scale];
		for (int i = 0; i < array.Length; i++)
			array[i] = new Tracked ();
		count += array.Length;

		chain = new Tracked ();
		Tracked tail = chain;
		for (int i = 1; i < scale; i++) {
			tail.next = new Tracked ();
			tail = tail.next;
		}
		tail.next = chain;
		count += scale;

		pairs = new Pair[scale / 2];
		for (int i = 0; i < pairs.Length; i++) {
			pairs[i].a = new Tracked ();
			pairs[i].b = new Tracked ();
		}
		count += 2 * pairs.Length;

		single.a = new Tracked ();
		single.b = new Tracked ();
		count += 2;

		map = new Dictionary<int, Tracked> ();
		for (int i = 0; i < scale / 4; i++)
			map[i] = new Tracked ();
		count += map.Count;

		shared = new object[scale];
		for (int i = 0; i < shared.Length; i++)
			shared[i] = (i & 1) != 0 ? array[i] : map[i % map.Count];

		holder = new Holder<Tracked> ();
		holder.value = new Tracked ();
		Tracked[] inner = new Tracked[scale / 8];
		for (int i = 0; i < inner.Length; i++)
			inner[i] = new Tracked ();
		holder.value.payload = inner;
		count += 1 + inner.Length;

		return count;
	}

	/* A shape closer to a game's heap than Build's: many small objects, each
	 * reaching a few containers of its own. */
	public static int BuildVessels (int scale)
	{
		vessels = new List<Part> ();
		for (int i = 0; i < scale; i++) {
			Part part = new Part ();
			for (int j = 0; j < 10; j++) {
				Module module = new Module ();
				module.part = part;
				module.state[0] = new Tracked ();
				module.fields["a"] = new Tracked ();
				module.fields["b"] = new Tracked ();
				part.modules.Add (module);
			}
			vessels.Add (part);
		}
		return scale * 31;
	}

	/* Reaches @fresh new Tracked instances, and the first ten of Build's array. */
	public static Tracked[] MakeDetached (int fresh)
	{
		Tracked[] detached = new Tracked[fresh + 10];
		for (int i = 0; i < fresh; i++)
			detached[i] = new Tracked ();
		for (int i = 0; i < 10; i++)
			detached[fresh + i] = array[i];
		return detached;
	}

	public static void Drop ()
	{
		array = null;
		chain = null;
		pairs = null;
		single = default (Pair);
		map = null;
		shared = null;
		holder = null;
		vessels = null;
	}

	/* The assembly is built as an executable, and never run as one. */
	public static int Main ()
	{
		return 0;
	}
}
