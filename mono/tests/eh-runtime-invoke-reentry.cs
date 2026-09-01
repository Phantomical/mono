using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

/*
 * An exception raised under a nested mono_runtime_invoke () that a native
 * caller reached through a P/Invoke, with an interp/compiled/interp round
 * trip directly under it:
 *
 *   B                interpreted, P/Invokes into libtest
 *     [libtest.c, no managed frame at all]
 *       mono_runtime_invoke (embedding API, &exc)
 *         Wrapper        the runtime-invoke wrapper this reenters through
 *           InterpMe_outer  interpreted, calls Middle via do_jit_call
 *             Middle          compiled, calls InterpMe_inner via an interp
 *                             entry thunk
 *               InterpMe_inner  interpreted, throws
 *
 * Nothing catches in managed code, so the throw has to reach the wrapper's
 * own catch (Exception) and come back out through mono_runtime_invoke's exc
 * out-param, which is the boundary that call exists for. Run with
 * MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=InterpMe so InterpMe_outer,
 * InterpMe_inner and B all stay interpreted while Middle compiles.
 */
class Reenter {
	[DllImport ("libtest", EntryPoint = "mono_test_reenter_invoke_by_name")]
	static extern bool mono_test_reenter_invoke_by_name (
		string assm_name, string name_space, string name, string meth_name);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void InterpMe_inner ()
	{
		throw new InvalidOperationException ("from the inner interpreted frame");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Middle ()
	{
		InterpMe_inner ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void InterpMe_outer ()
	{
		Middle ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool InterpMe_B (string assemblyPath)
	{
		return mono_test_reenter_invoke_by_name (
			assemblyPath, "", "Reenter", "InterpMe_outer");
	}

	const int tier1 = 2;

	public static int Main ()
	{
		MethodInfo mi = typeof (Reenter).GetMethod ("Middle",
			BindingFlags.Static | BindingFlags.NonPublic);
		if (!Mono.Tiering.MonoTier.PromoteNow (mi.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Middle would not compile at tier 1");
			return 1;
		}

		string assemblyPath = typeof (Reenter).Assembly.Location;
		bool caught = InterpMe_B (assemblyPath);

		if (!caught) {
			Console.WriteLine ("FAIL: mono_runtime_invoke's exc out-param stayed NULL");
			return 1;
		}

		Console.WriteLine ("survived");
		return 0;
	}
}
