using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/* Exercise both LLVM folds before and after tiering. */

class SomeClass { }

struct PlainStruct { public int x; }

struct RefStruct { public object o; }

enum SomeEnum { A, B }

class Program {
	const int tier2 = 4;

	static bool ClassIsValueType () { return typeof (SomeClass).IsValueType; }
	static bool StructIsValueType () { return typeof (PlainStruct).IsValueType; }
	static bool EnumIsValueType () { return typeof (SomeEnum).IsValueType; }
	static bool NullableIsValueType () { return typeof (PlainStruct?).IsValueType; }

	static bool ContainsRefs<T> ()
	{
		return RuntimeHelpers.IsReferenceOrContainsReferences<T> ();
	}

	static bool Promote (MethodInfo target)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier 2", target);
		return false;
	}

	static bool CheckAnswer (MethodInfo target, string label, bool expected)
	{
		bool before = (bool) target.Invoke (null, null);

		if (before != expected) {
			Console.WriteLine ("FAIL: {0} answered {1} at tier 0, expected {2}",
			                   label, before, expected);
			return false;
		}

		if (!Promote (target))
			return false;

		bool after = (bool) target.Invoke (null, null);

		if (after != expected) {
			Console.WriteLine ("FAIL: {0} answered {1} once promoted, expected {2}",
			                   label, after, expected);
			return false;
		}

		return true;
	}

	static bool Check (string name, bool expected)
	{
		MethodInfo target = typeof (Program).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return CheckAnswer (target, name, expected);
	}

	static bool CheckContainsRefs (Type t, bool expected)
	{
		MethodInfo generic = typeof (Program).GetMethod (
			"ContainsRefs", BindingFlags.Static | BindingFlags.NonPublic);
		MethodInfo closed = generic.MakeGenericMethod (t);

		return CheckAnswer (closed, "ContainsRefs<" + t + "> ()", expected);
	}

	public static int Main ()
	{
		bool ok = true;

		ok &= Check ("ClassIsValueType", false);
		ok &= Check ("StructIsValueType", true);
		ok &= Check ("EnumIsValueType", true);
		ok &= Check ("NullableIsValueType", true);

		ok &= CheckContainsRefs (typeof (SomeClass), true);
		ok &= CheckContainsRefs (typeof (string), true);
		ok &= CheckContainsRefs (typeof (int), false);
		ok &= CheckContainsRefs (typeof (PlainStruct), false);
		ok &= CheckContainsRefs (typeof (RefStruct), true);

		if (!ok)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}
