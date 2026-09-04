using System;
using System.Reflection;

// --llvm-opt=-mono-tier0-classic=StaticRgctx filters StaticRgctxBox<T>'s own
// static cctor and StaticRgctxSharedStatic<T> to the classic compiler. Both
// are reference-sharable statics, so classic tier0 compiles each as a shared
// body that reads its own runtime generic context off a register, since
// neither has a "this" to read it from instead.
//
// mono_jit_runtime_invoke () reaches a compiled method through no call site
// of its own - class init running a static cctor, MethodInfo.Invoke, a
// delegate target, ldftn - and never told mini_add_method_trampoline () that
// a callee like this needs that register loaded first. Before the fix, class
// init crashed reading garbage in place of the context, and
// MethodInfo.MakeGenericMethod (...).Invoke () crashed inside
// mono_method_fill_runtime_generic_context () on a method's first-ever entry.
public class Tier0ClassicStaticRgctxTest
{
	sealed class Widget { }

	static class StaticRgctxBox<T>
	{
		public static readonly string Tag;

		static StaticRgctxBox ()
		{
			Tag = typeof (T).Name;
		}
	}

	static string StaticRgctxSharedStatic<T> (T value) where T : class
	{
		return value == null ? "null" : value.GetType ().Name;
	}

	// Reflection crosses into a compiled runtime-invoke wrapper, the same way
	// tier0-classic-gsharedvt.cs's GsharedShareEnter does. That is what gives
	// StaticRgctxBox<Widget>'s cctor a chance to answer the filter.
	static string StaticRgctxEnterCctor ()
	{
		return StaticRgctxBox<Widget>.Tag;
	}

	public static int Main ()
	{
		MethodInfo enterCctor = typeof (Tier0ClassicStaticRgctxTest).GetMethod (
			"StaticRgctxEnterCctor", BindingFlags.Static | BindingFlags.NonPublic);
		string cctorResult = (string) enterCctor.Invoke (null, null);

		MethodInfo open = typeof (Tier0ClassicStaticRgctxTest).GetMethod (
			"StaticRgctxSharedStatic", BindingFlags.Static | BindingFlags.NonPublic);
		MethodInfo closed = open.MakeGenericMethod (typeof (string));
		string invokeResult = (string) closed.Invoke (null, new object[] { "hi" });

		int failures = 0;

		if (cctorResult != "Widget") {
			Console.WriteLine ("FAIL: static cctor, got {0}", cctorResult);
			failures++;
		}
		if (invokeResult != "String") {
			Console.WriteLine ("FAIL: MakeGenericMethod invoke, got {0}", invokeResult);
			failures++;
		}

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
