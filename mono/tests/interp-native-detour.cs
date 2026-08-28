using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// A native patcher asks for a method's entry address and writes a jump over it.
// MonoMod redirects a method this way.
//
// A method that starts in the interpreter has one entry address all the same.
// GetFunctionPointer mints the redirectable stub without a compile, and hands
// out the address a compiled ldftn names. So a patch written over that address
// takes effect for every compiled caller, at any tier the method reaches.
//
// A patch written this way does not reach an interpreted caller, which calls
// through the callee's InterpMethod and touches no stub. A patcher that needs
// both engines asks for the redirect through mono_install_method_detour ()
// instead, which mono/unit-tests/gtest/runtime/test-detour.cpp covers. Harmony
// is patched to take that route.
//
// The jump below is amd64, which matches the scope of this corpus.
class Test {
	const int PROT_READ = 1;
	const int PROT_WRITE = 2;
	const int PROT_EXEC = 4;

	const uint PAGE_EXECUTE_READWRITE = 0x40;

	/* MonoTier::tier1, as PromoteNow takes it. */
	const int tier1 = 2;

	[DllImport ("libc", SetLastError = true)]
	static extern int mprotect (IntPtr addr, ulong len, int prot);

	[DllImport ("kernel32", SetLastError = true)]
	static extern bool VirtualProtect (IntPtr addr, UIntPtr len, uint prot, out uint old);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Patched () { return 1; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Replacement () { return 2; }

	// The call under test. It is compiled before it runs, so the call reaches
	// Patched through the stub the patch was written over.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallPatched () { return Patched (); }

	static MethodInfo MethodOf (string name)
	{
		return typeof (Test).GetMethod (name,
			BindingFlags.NonPublic | BindingFlags.Static);
	}

	static IntPtr AddressOf (string name)
	{
		return MethodOf (name).MethodHandle.GetFunctionPointer ();
	}

	static void MakeWritable (IntPtr start, ulong length)
	{
		if (Environment.OSVersion.Platform == PlatformID.Unix
		    || Environment.OSVersion.Platform == PlatformID.MacOSX) {
			if (mprotect (start, length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
				throw new Exception ("mprotect refused the page holding the entry: errno "
						     + Marshal.GetLastWin32Error ());
			return;
		}

		uint old;

		if (!VirtualProtect (start, new UIntPtr (length), PAGE_EXECUTE_READWRITE, out old))
			throw new Exception ("VirtualProtect refused the page holding the entry: error "
					     + Marshal.GetLastWin32Error ());
	}

	// movabs $target, %rax ; jmp *%rax - twelve bytes, which is what fits in the
	// sixteen a stub block holds.
	static void WriteDetour (IntPtr at, IntPtr target)
	{
		const long page = 4096;
		IntPtr start = new IntPtr (at.ToInt64 () & ~(page - 1));
		ulong length = (ulong) (at.ToInt64 () + 16 - start.ToInt64 ());

		MakeWritable (start, length);

		Marshal.WriteByte (at, 0, 0x48);
		Marshal.WriteByte (at, 1, 0xB8);
		Marshal.WriteInt64 (at, 2, target.ToInt64 ());
		Marshal.WriteByte (at, 10, 0xFF);
		Marshal.WriteByte (at, 11, 0xE0);
	}

	public static int Main ()
	{
		IntPtr original = AddressOf ("Patched");
		IntPtr replacement = AddressOf ("Replacement");

		// A write over one address must not reach the other, which it would if a
		// shared entry were handed out in place of a per-method one.
		if (original == replacement) {
			Console.WriteLine ("FAILED: two methods were given one address");
			return 1;
		}

		WriteDetour (original, replacement);

		// Calling the method in a loop races the compile queue, so the test asks
		// for the tier outright.
		if (!Mono.Tiering.MonoTier.PromoteNow (MethodOf ("CallPatched").MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAILED: CallPatched would not compile");
			return 1;
		}

		int got = CallPatched ();

		if (got != 2) {
			Console.WriteLine ("FAILED: the patched method returned " + got
					   + ", so the call did not go through the patched address");
			return 1;
		}

		return 0;
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}
