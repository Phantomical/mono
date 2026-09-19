using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Only faults in the implicit null-check range should be raised as
 * NullReferenceException. Run each case in a child process because an access
 * outside that range should terminate the process.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	// Keep LLVM from folding these addresses into pointer constants before the
	// backend inserts the implicit null check.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe void *NullValue () => (void *) 0;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe void *WildValue () => (void *) 0x123456789000L;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe void DerefNull ()
	{
		int *p = (int *) NullValue ();
		*p = 42;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe void DerefWild ()
	{
		int *p = (int *) WildValue ();
		*p = 42;
	}

	static int fails;

	static void Fail (string what, string tier, string how)
	{
		Console.WriteLine ("FAIL: {0} at {1} {2}", what, tier, how);
		++fails;
	}

	static int RunChild (string method_name, int tier)
	{
		MethodInfo target = typeof (Program).GetMethod (method_name,
			BindingFlags.Static | BindingFlags.NonPublic);

		if (tier != 0 && !Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier))
			return 2;

		try {
			target.Invoke (null, null);
		} catch (TargetInvocationException e) when (e.InnerException is NullReferenceException) {
			return 0;
		}

		return 3;
	}

	// Mono uses exit code 1 or 255 for unhandled managed exceptions. Native
	// faults use a different nonzero code; 2 and 3 are child protocol errors.
	static void CheckExit (string method_name, string tier_name, int code, string stderr)
	{
		string how = null;

		if (code == 2)
			how = "could not promote to this tier";
		else if (code == 3)
			how = "returned instead of throwing or crashing";
		else if (method_name == "DerefNull" && code != 0)
			how = "exited " + code + " instead of catching its NullReferenceException";
		else if (method_name == "DerefWild" && (code == 0 || code == 1 || code == 255))
			how = "exited " + code + " instead of crashing";

		if (how != null)
			Fail (method_name, tier_name, "child " + how + ":\n" + stderr);
	}

	static void RunOne (string method_name, string tier_name, int tier)
	{
		string self = Process.GetCurrentProcess ().MainModule.FileName;
		string here = Assembly.GetExecutingAssembly ().Location;

		var psi = new ProcessStartInfo (self, string.Format ("\"{0}\" {1} {2}", here, method_name, tier)) {
			UseShellExecute = false,
			RedirectStandardOutput = true,
			RedirectStandardError = true,
		};

		using (Process child = Process.Start (psi)) {
			string stderr = child.StandardError.ReadToEnd ();
			child.StandardOutput.ReadToEnd ();
			child.WaitForExit ();

			CheckExit (method_name, tier_name, child.ExitCode, stderr);
		}
	}

	public static int Main (string [] args)
	{
		if (args.Length == 2)
			return RunChild (args [0], int.Parse (args [1]));

		RunOne ("DerefNull", "tier 0", 0);
		RunOne ("DerefWild", "tier 0", 0);
		RunOne ("DerefNull", "tier 1", 3);
		RunOne ("DerefWild", "tier 1", 3);
		RunOne ("DerefNull", "tier 2", 4);
		RunOne ("DerefWild", "tier 2", 4);

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
