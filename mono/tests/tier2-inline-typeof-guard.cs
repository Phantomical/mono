using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Each Pick method has an arm padded beyond the inline limit. When a generic
 * instantiation makes that arm unreachable, its effective size fits and the
 * tier-2 cost model can inline it.
 *
 * The exception stack trace identifies inlined methods because their frame
 * and caller report the same native offset.
 *
 * Caller<object> is a control: calls from a shared body into a shared generic
 * class are not inline candidates. Shared-type analysis is covered by the
 * ILAnalyzer unit tests.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

// typeof (T) == typeof (int): true for int and false for double.
static class Picker<T> {
	public static int Pick (T t, bool blowUp)
	{
		if (blowUp)
			throw new InvalidOperationException ("blew up");

		if (typeof (T) == typeof (int))
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		a += 9; a += 10; a += 11; a += 12; a += 13; a += 14; a += 15; a += 16;
		a += 17; a += 18; a += 19; a += 20; a += 21; a += 22; a += 23; a += 24;

		return a;
	}
}

static class Caller<T> {
	public static int Call (T t, int k, bool blowUp)
	{
		return Picker<T>.Pick (t, blowUp) + k;
	}
}

// typeof (T).IsValueType: true for int.
static class ValuePicker<T> {
	public static int Pick (T t, bool blowUp)
	{
		if (blowUp)
			throw new InvalidOperationException ("blew up");

		if (typeof (T).IsValueType)
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		a += 9; a += 10; a += 11; a += 12; a += 13; a += 14; a += 15; a += 16;
		a += 17; a += 18; a += 19; a += 20; a += 21; a += 22; a += 23; a += 24;

		return a;
	}
}

static class ValueCaller<T> {
	public static int Call (T t, int k, bool blowUp)
	{
		return ValuePicker<T>.Pick (t, blowUp) + k;
	}
}

// default (T) != null boxes a T, and a boxed int is an object.
static class BoxPicker<T> {
	public static int Pick (T t, bool blowUp)
	{
		if (blowUp)
			throw new InvalidOperationException ("blew up");

		if (default (T) != null)
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		a += 9; a += 10; a += 11; a += 12; a += 13; a += 14; a += 15; a += 16;
		a += 17; a += 18; a += 19; a += 20; a += 21; a += 22; a += 23; a += 24;

		return a;
	}
}

static class BoxCaller<T> {
	public static int Call (T t, int k, bool blowUp)
	{
		return BoxPicker<T>.Pick (t, blowUp) + k;
	}
}

// The decided guards leave an integer in a local, and the switch that reads
// it keeps one case: a short one for int and for long, the padded default
// for double.
static class SwitchPicker<T> {
	public static int Pick (T t, bool blowUp)
	{
		if (blowUp)
			throw new InvalidOperationException ("blew up");

		int k = typeof (T) == typeof (int) ? 1 : typeof (T) == typeof (long) ? 2 : typeof (T) == typeof (short) ? 3 : 0;

		switch (k) {
		case 1:
			return 1;
		case 2:
			return 2;
		case 3:
			return 3;
		}

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		a += 9; a += 10; a += 11; a += 12; a += 13; a += 14; a += 15; a += 16;
		a += 17; a += 18; a += 19; a += 20; a += 21; a += 22; a += 23; a += 24;

		return a;
	}
}

static class SwitchCaller<T> {
	public static int Call (T t, int k, bool blowUp)
	{
		return SwitchPicker<T>.Pick (t, blowUp) + k;
	}
}

class Program {
	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/// Whether Pick and its caller share the native offset of an inlined call.
	static bool RunsInsideRoot (Exception e, Type root)
	{
		string picker = root.Name.Replace ("Caller", "Picker");
		string arg = root.GetGenericArguments ()[0].Name;
		StackTrace st = new StackTrace (e, false);
		int in_pick = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;

			Type declaring = m.DeclaringType;

			if (declaring.Name == picker && m.Name == "Pick")
				in_pick = f.GetNativeOffset ();
			if (declaring.Name == root.Name && m.Name == "Call"
			    && declaring.GetGenericArguments ()[0].Name == arg)
				in_root = f.GetNativeOffset ();
		}

		return in_pick >= 0 && in_pick == in_root;
	}

	/// One root and what its Pick is expected to do at tier 2.
	class Case {
		public Type Root;
		public Func<bool, int> Call;
		public int Answer;
		public bool Inlines;
		public string Why;
	}

	static readonly List<Case> cases = new List<Case> {
		new Case { Root = typeof (Caller<int>), Call = b => Caller<int>.Call (0, 4, b), Answer = 5,
			Inlines = true, Why = "typeof (int) == typeof (int) closes the padded arm" },
		new Case { Root = typeof (Caller<double>), Call = b => Caller<double>.Call (0, 4, b), Answer = 304,
			Inlines = false, Why = "typeof (double) == typeof (int) keeps the padded arm" },
		new Case { Root = typeof (Caller<object>), Call = b => Caller<object>.Call (null, 4, b), Answer = 304,
			Inlines = false, Why = "a shared body's call into a shared class is no inline site" },

		new Case { Root = typeof (ValueCaller<int>), Call = b => ValueCaller<int>.Call (0, 4, b), Answer = 5,
			Inlines = true, Why = "int is a value type, which closes the padded arm" },

		new Case { Root = typeof (BoxCaller<int>), Call = b => BoxCaller<int>.Call (0, 4, b), Answer = 5,
			Inlines = true, Why = "a boxed int is never null, which closes the padded arm" },

		new Case { Root = typeof (SwitchCaller<int>), Call = b => SwitchCaller<int>.Call (0, 4, b), Answer = 5,
			Inlines = true, Why = "the switch keeps case 1 for int" },
		new Case { Root = typeof (SwitchCaller<long>), Call = b => SwitchCaller<long>.Call (0, 4, b), Answer = 6,
			Inlines = true, Why = "the switch keeps case 2 for long" },
		new Case { Root = typeof (SwitchCaller<double>), Call = b => SwitchCaller<double>.Call (0, 4, b), Answer = 304,
			Inlines = false, Why = "the switch keeps the padded default for double" },
	};

	static bool Promote (int tier)
	{
		foreach (Case c in cases) {
			MethodInfo call = c.Root.GetMethod ("Call");

			if (!Mono.Tiering.MonoTier.PromoteNow (call.MethodHandle.Value, tier)) {
				Console.WriteLine ("FAIL: {0} would not compile at tier {1}", c.Root, tier);
				return false;
			}
		}

		return true;
	}

	public static int Main ()
	{
		if (!Promote (3))
			return 1;

		foreach (Case c in cases)
			Check (c.Call (false) == c.Answer, c.Root + " answers before tier 2");

		for (int i = 0; i < 20000; i++)
			foreach (Case c in cases)
				c.Call (false);

		if (!Promote (4))
			return 1;

		foreach (Case c in cases) {
			bool ran = false, inlined = false;

			try {
				c.Call (true);
			} catch (InvalidOperationException e) {
				ran = true;
				inlined = RunsInsideRoot (e, c.Root);
			}

			Check (ran, c.Root + " threw at tier 2");
			Check (inlined == c.Inlines, c.Root + ": " + c.Why);
			Check (c.Call (false) == c.Answer, c.Root + " answers at tier 2");
		}

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
