using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * Environment.CurrentManagedThreadId compiled as a read of the thread's TLS.
 *
 * The backend answers the property off mono_tls_get_thread_extern () and a field
 * on the MonoInternalThread, under a declaration marked memory(none) and
 * speculatable. So LLVM can share one call between the sites a method holds, and
 * move one onto a path that had none.
 *
 * Reads () holds two of them with a call in between, which is the shape the
 * sharing acts on. It compares both against Thread.CurrentThread.ManagedThreadId
 * in the same frame. That spelling keeps its own calls, so the two are a
 * differential on the field the backend reads.
 *
 * A shared call that outlived its thread would show up as the second thread
 * reporting the first one's id. The interpreter runs the property as written, so
 * the tier-0 answer is the third arm.
 */

class Program {
	const int tier2 = 2;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Opaque (int value)
	{
		return value;
	}

	/// The id this thread reports, or -1 where the two spellings disagree.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Reads ()
	{
		int first = Environment.CurrentManagedThreadId;
		int declared = Opaque (Thread.CurrentThread.ManagedThreadId);
		int second = Environment.CurrentManagedThreadId;

		if (first != second || first != declared)
			return -1;

		return first;
	}

	static bool Promote (string name)
	{
		MethodInfo target = typeof (Program).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier 2", name);
		return false;
	}

	public static int Main ()
	{
		int interpreted = Reads ();

		if (interpreted < 1) {
			Console.WriteLine ("FAIL: tier 0 answered {0}", interpreted);
			return 1;
		}

		if (!Promote ("Reads") || !Promote ("Opaque"))
			return 1;

		int compiled = Reads ();

		if (compiled != interpreted) {
			Console.WriteLine ("FAIL: tier 2 answered {0}, tier 0 answered {1}",
			                   compiled, interpreted);
			return 1;
		}

		if (compiled != Thread.CurrentThread.ManagedThreadId) {
			Console.WriteLine ("FAIL: the property answered {0}, the thread reports {1}",
			                   compiled, Thread.CurrentThread.ManagedThreadId);
			return 1;
		}

		int other = 0;
		Thread second = new Thread (() => other = Reads ());

		second.Start ();
		second.Join ();

		if (other < 1) {
			Console.WriteLine ("FAIL: the second thread answered {0}", other);
			return 1;
		}

		if (other == compiled) {
			Console.WriteLine ("FAIL: both threads answered {0}", other);
			return 1;
		}

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
