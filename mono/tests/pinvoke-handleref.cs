using System;
using System.Runtime.InteropServices;

// A HandleRef argument is two words as the managed type and one word once the
// marshalling wrapper has extracted the handle. Every case here passes values
// the native side can recognise, so a wrong answer says which argument moved.
//
// Signatures of six parameters or fewer that are all pointer-sized take a
// specialised interpreter opcode; anything else goes through the general
// pinvoke path. Both are covered, because only the general one walks the
// argument list.
class Tests {
	[DllImport ("libtest")]
	static extern int mono_test_handleref_7 (HandleRef a, IntPtr b, IntPtr c, IntPtr d,
	                                         IntPtr e, IntPtr f, IntPtr g);
	[DllImport ("libtest")]
	static extern int mono_test_handleref_7 (IntPtr a, IntPtr b, IntPtr c, IntPtr d,
	                                         IntPtr e, IntPtr f, IntPtr g);
	[DllImport ("libtest")]
	static extern int mono_test_handleref_6 (HandleRef a, IntPtr b, IntPtr c, IntPtr d, IntPtr e, IntPtr f);
	[DllImport ("libtest")]
	static extern int mono_test_handleref_float (HandleRef a, HandleRef b, int c, float d, out IntPtr o);
	[DllImport ("libtest")]
	static extern int mono_test_handleref_float (IntPtr a, IntPtr b, int c, float d, out IntPtr o);
	[DllImport ("libtest")]
	static extern int mono_test_handleref_mid (IntPtr a, HandleRef b, IntPtr c, int d, float e);

	static readonly IntPtr A = (IntPtr) 0xa1, B = (IntPtr) 0xb2, C = (IntPtr) 0xc3;
	static readonly IntPtr D = (IntPtr) 0xd4, E = (IntPtr) 0xe5, F = (IntPtr) 0xf6, G = (IntPtr) 0x97;

	static int failures;

	static void Check (string what, int bad)
	{
		if (bad == 0)
			return;
		Console.WriteLine ("{0}: arguments {1} did not arrive (mask 0x{2:x})", what, Mask (bad), bad);
		failures++;
	}

	static string Mask (int bad)
	{
		string s = "";
		for (int i = 0; i < 8; i++)
			if ((bad & (1 << i)) != 0)
				s += (s.Length > 0 ? "," : "") + i;
		return s;
	}

	// Seven pointer-sized arguments: past the specialised opcode, so this runs
	// on the general path without a float anywhere in the signature.
	static void SevenArgs ()
	{
		Check ("handleref, 7 args", mono_test_handleref_7 (new HandleRef (null, A), B, C, D, E, F, G));
		Check ("intptr, 7 args (control)", mono_test_handleref_7 (A, B, C, D, E, F, G));
	}

	// The shape System.Drawing calls GdipCreateCustomLineCap with. A float is
	// enough on its own to keep the signature off the specialised opcode.
	static void WithFloat ()
	{
		IntPtr o = IntPtr.Zero;
		int bad = mono_test_handleref_float (new HandleRef (null, A), new HandleRef (null, B), 7, 1.0f, out o);
		Check ("handleref + float", bad);
		if (bad == 0 && o != (IntPtr) 0x5150) {
			Console.WriteLine ("handleref + float: out parameter came back as 0x{0:x}", (long) o);
			failures++;
		}

		o = IntPtr.Zero;
		Check ("intptr + float (control)", mono_test_handleref_float (A, B, 7, 1.0f, out o));
		if (o != (IntPtr) 0x5150) {
			Console.WriteLine ("intptr + float (control): out parameter came back as 0x{0:x}", (long) o);
			failures++;
		}
	}

	// A HandleRef that is not the first argument shifts only what follows it.
	static void InTheMiddle ()
	{
		Check ("handleref in the middle", mono_test_handleref_mid (A, new HandleRef (null, B), C, 7, 1.0f));
	}

	// Six pointer-sized arguments still reach the specialised opcode, so this
	// passes today and guards against a fix that only moves the problem.
	static void FastPath ()
	{
		Check ("handleref, 6 args (fast opcode)",
		       mono_test_handleref_6 (new HandleRef (null, A), B, C, D, E, F));
	}

	static int Main ()
	{
		SevenArgs ();
		WithFloat ();
		InTheMiddle ();
		FastPath ();

		if (failures != 0) {
			Console.WriteLine ("{0} case(s) failed", failures);
			return 1;
		}
		Console.WriteLine ("OK");
		return 0;
	}
}
