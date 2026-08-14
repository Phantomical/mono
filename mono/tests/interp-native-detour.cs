using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// A native patcher asks for a method's entry address and writes a jump over it.
// Harmony and MonoMod redirect a method this way, and Unity mod code is built on
// them, so a patch has to reach the method whichever engine runs it.
//
// An interpreted caller reaches its callee through the callee's InterpMethod and
// touches no stub, so a jump written over the stub reached nothing. A method
// whose address has gone to native code is called through that address instead.
//
// The jump below is amd64, and the page permissions are POSIX. Both match the
// scope of this corpus.
class Test {
	const int PROT_READ = 1;
	const int PROT_WRITE = 2;
	const int PROT_EXEC = 4;

	[DllImport ("libc", SetLastError = true)]
	static extern int mprotect (IntPtr addr, ulong len, int prot);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Patched () { return 1; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Replacement () { return 2; }

	// The call under test. Its first calls are interpreted, which is the arm the
	// patch used to be lost on.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallPatched () { return Patched (); }

	static IntPtr AddressOf (string name)
	{
		MethodInfo m = typeof (Test).GetMethod (name,
			BindingFlags.NonPublic | BindingFlags.Static);

		return m.MethodHandle.GetFunctionPointer ();
	}

	// movabs $target, %rax ; jmp *%rax - twelve bytes, which is what fits in the
	// sixteen a stub block holds.
	static void WriteDetour (IntPtr at, IntPtr target)
	{
		const long page = 4096;
		IntPtr start = new IntPtr (at.ToInt64 () & ~(page - 1));
		ulong length = (ulong) (at.ToInt64 () + 16 - start.ToInt64 ());

		if (mprotect (start, length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
			throw new Exception ("mprotect refused the page holding the entry: errno "
					     + Marshal.GetLastWin32Error ());

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

		int got = CallPatched ();

		if (got != 2) {
			Console.WriteLine ("FAILED: the patched method returned " + got
					   + ", so the call did not go through the patched address");
			return 1;
		}

		return 0;
	}
}
