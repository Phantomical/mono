// The pointer opcodes: ldind and stind in every width, ldloca and ldarga,
// localloc, cpblk, initblk, ldobj and stobj, sizeof, and pointer arithmetic.
//
// Operands go through a NoInlining helper, so the transform cannot fold the
// answer and leave the opcode untested.
//
// The byte order tests read a wide local through a narrow pointer, so they
// assume a little endian target.

using System;
using System.Runtime.CompilerServices;

public class UnsafeOps {

	[MethodImpl (MethodImplOptions.NoInlining)] static int IdI4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float IdR4 (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdR8 (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object IdObj (object x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string IdStr (string x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static unsafe void* IdPtr (void* x) { return x; }

	struct UnsafePair { public int lo, hi; }
	struct UnsafeMixed { public byte b; public long l; }
	struct UnsafeBytes3 { public byte a, b, c; }
	class UnsafeCell { public int v; public object o; }

	// ---- ldind, one test per width ----

	// Each raw value has bits above the width under test, so a load that reads
	// too many bytes gives a different answer.

	public static unsafe int test_1_ldind_i1_sign_extends ()
	{
		int raw = IdI4 (0x3ff);
		sbyte* p = (sbyte*) &raw;
		return *p == -1 ? 1 : 0;
	}

	public static unsafe int test_255_ldind_u1 ()
	{
		int raw = IdI4 (0x3ff);
		byte* p = (byte*) &raw;
		return *p;
	}

	public static unsafe int test_1_ldind_i2_sign_extends ()
	{
		int raw = IdI4 (0x3ffff);
		short* p = (short*) &raw;
		return *p == -1 ? 1 : 0;
	}

	public static unsafe int test_65535_ldind_u2 ()
	{
		int raw = IdI4 (0x3ffff);
		ushort* p = (ushort*) &raw;
		return *p;
	}

	public static unsafe int test_5_ldind_i4 ()
	{
		int v = IdI4 (5);
		int* p = &v;
		return *p;
	}

	public static unsafe int test_1_ldind_u4 ()
	{
		// ldind.u4 and ldind.i4 push the same 32 bits, so this pins the load
		// width and the zero extension on the way to int64, nothing more.
		int v = IdI4 (-1);
		uint* p = (uint*) &v;
		return (long) *p == 4294967295L ? 1 : 0;
	}

	public static unsafe int test_7_ldind_i8 ()
	{
		long v = IdI8 (0x100000007L);
		long* p = &v;
		return *p == 0x100000007L ? 7 : 0;
	}

	public static unsafe int test_9_ldind_i ()
	{
		int v = IdI4 (9);
		void* q = &v;
		void** pp = &q;
		return *(int*) *pp;
	}

	public static unsafe int test_3_ldind_r4 ()
	{
		float f = IdR4 (3.5f);
		float* p = &f;
		return (int) *p;
	}

	public static unsafe int test_4_ldind_r8 ()
	{
		double d = IdR8 (4.5);
		double* p = &d;
		return (int) *p;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int LoadRef (ref object o) { return ((string) o).Length; }

	public static int test_3_ldind_ref ()
	{
		object o = IdObj ("abc");
		return LoadRef (ref o);
	}

	// ---- stind, one test per width ----

	// Each target has bits set above the width under test, so a store that
	// writes too many bytes clears them and changes the answer.

	public static unsafe int test_322_stind_i1 ()
	{
		int raw = IdI4 (0x100);
		byte* p = (byte*) &raw;
		*p = (byte) IdI4 (66);
		return raw;
	}

	public static unsafe int test_66049_stind_i2 ()
	{
		int raw = IdI4 (0x10000);
		short* p = (short*) &raw;
		*p = (short) IdI4 (513);
		return raw;
	}

	public static unsafe int test_11_stind_i4 ()
	{
		int v = 0;
		int* p = &v;
		*p = IdI4 (11);
		return v;
	}

	public static unsafe int test_12_stind_i8 ()
	{
		long v = IdI8 (-1);
		long* p = &v;
		*p = IdI8 (12);
		return v == 12 ? 12 : 0;
	}

	public static unsafe int test_13_stind_i ()
	{
		int v = IdI4 (13);
		void* q = null;
		void** pp = &q;
		*pp = &v;
		return *(int*) q;
	}

	public static unsafe int test_2_stind_r4 ()
	{
		float f = 0;
		float* p = &f;
		*p = (float) IdR8 (2.5);
		return (int) f;
	}

	public static unsafe int test_6_stind_r8 ()
	{
		double d = 0;
		double* p = &d;
		*p = IdR8 (6.25);
		return (int) d;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void StoreRef (ref object slot, object val) { slot = val; }

	public static int test_4_stind_ref ()
	{
		object o = null;
		StoreRef (ref o, IdObj ("abcd"));
		return ((string) o).Length;
	}

	public static int test_5_stind_ref_into_a_heap_field ()
	{
		// A slot on the heap, so the store goes through the write barrier.
		UnsafeCell c = new UnsafeCell ();
		StoreRef (ref c.o, IdObj ("abcde"));
		return ((string) c.o).Length;
	}

	// ---- local and argument addresses ----

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int AddThrough (ref int a, int b) { a += b; return a; }

	public static int test_9_ldloca_of_a_local ()
	{
		int x = IdI4 (4);
		AddThrough (ref x, IdI4 (5));
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int Bump (int a) { int* p = &a; *p = *p + 1; return a; }

	public static int test_8_ldarga_of_an_argument ()
	{
		return Bump (IdI4 (7));
	}

	public static int test_9_ref_local ()
	{
		int x = IdI4 (4);
		ref int r = ref x;
		r = r + 5;
		return x;
	}

	public static unsafe int test_17_pointer_to_pointer ()
	{
		int v = IdI4 (17);
		int* p = &v;
		int** pp = &p;
		return **pp;
	}

	public static unsafe int test_30_struct_field_through_a_pointer ()
	{
		UnsafePair s = default;
		UnsafePair* q = &s;
		q->lo = IdI4 (10);
		q->hi = IdI4 (20);
		return s.lo + s.hi;
	}

	// ---- ldobj, stobj, initobj ----

	public static unsafe int test_21_ldobj_and_stobj ()
	{
		UnsafeMixed a = default;
		a.b = (byte) IdI4 (1);
		a.l = IdI8 (20);
		UnsafeMixed b = default;
		UnsafeMixed* pa = &a, pb = &b;
		*pb = *pa;
		return b.b + (int) b.l;
	}

	public static unsafe int test_1_initobj_through_a_pointer ()
	{
		UnsafeMixed m = default;
		m.b = (byte) IdI4 (1);
		m.l = IdI8 (2);
		UnsafeMixed* p = &m;
		*p = default;
		return m.b == 0 && m.l == 0 ? 1 : 0;
	}

	// ---- localloc ----

	public static unsafe int test_496_stackalloc_read_back ()
	{
		int n = IdI4 (32);
		byte* p = stackalloc byte[n];
		for (int i = 0; i < 32; i++)
			p[i] = (byte) i;

		int total = 0;
		for (int i = 0; i < 32; i++)
			total += p[i];
		return total;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int DirtyLeaf (int n)
	{
		byte* p = stackalloc byte[n];
		for (int i = 0; i < n; i++)
			p[i] = 0xff;
		return p[n - 1];
	}

	public static unsafe int test_1_stackalloc_is_zeroed ()
	{
		// The callee fills the memory the interpreter hands this frame next, so
		// a zero below is the .locals init flag and not luck.  A compiled frame
		// gets its buffer at another address, which the fill does not reach.
		if (DirtyLeaf (IdI4 (64)) != 0xff)
			return 0;

		int n = IdI4 (64);
		byte* p = stackalloc byte[n];
		for (int i = 0; i < 64; i++)
			if (p[i] != 0)
				return 0;
		return 1;
	}

	public static unsafe int test_1_stackalloc_zero_length ()
	{
		// A request for no bytes still gives back a pointer, not null.
		int n = IdI4 (0);
		byte* p = stackalloc byte[n];
		return p != null ? 1 : 0;
	}

	public static unsafe int test_100_stackalloc_in_a_loop ()
	{
		// Each pass gets memory of its own.  The pass before wrote its index,
		// and that index is still there.
		int total = 0;
		int* prev = null;
		for (int i = 0; i < 100; i++) {
			int* p = stackalloc int[IdI4 (2)];
			p[0] = i;
			p[1] = 1;
			if (prev != null && prev[0] != i - 1)
				return 0;
			total += p[1];
			prev = p;
		}
		return total;
	}

	public static unsafe int test_10_stackalloc_crosses_a_fragment ()
	{
		// The frame data allocator starts with one 8KB fragment, so ten 1KB
		// buffers held at once need a second one.  The first buffer has to
		// survive the change.
		int total = 0;
		byte* first = null;
		for (int i = 0; i < 10; i++) {
			byte* p = stackalloc byte[IdI4 (1024)];
			p[1023] = (byte) (i + 1);
			if (i == 0)
				first = p;
			total++;
		}
		return first[1023] == 1 ? total : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe long LocallocLeaf (int n)
	{
		byte* p = stackalloc byte[n];
		p[n - 1] = (byte) n;
		return (long) p;
	}

	public static int test_100_localloc_is_released_on_return ()
	{
		// Every call gets the same address back, which says the frame gave the
		// memory up when it returned.
		long first = LocallocLeaf (IdI4 (512));
		int total = 0;
		for (int i = 0; i < 100; i++)
			if (LocallocLeaf (IdI4 (512)) == first)
				total++;
		return total;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int LocallocRecurse (int n)
	{
		byte* p = stackalloc byte[32];
		p[0] = (byte) n;
		int deeper = n == 0 ? 0 : LocallocRecurse (n - 1);
		return p[0] + deeper;
	}

	public static int test_15_localloc_survives_a_callee ()
	{
		return LocallocRecurse (IdI4 (5));
	}

	public static unsafe int test_5_stackalloc_survives_a_catch ()
	{
		byte* p = stackalloc byte[16];
		p[0] = (byte) IdI4 (5);
		try {
			throw new InvalidOperationException ();
		} catch (InvalidOperationException) {
		}
		return p[0];
	}

	public static unsafe int test_45_two_stack_buffers_do_not_overlap ()
	{
		byte* src = stackalloc byte[16];
		byte* dst = stackalloc byte[16];

		// If the two buffers shared memory, the fill would put 0xff into src as
		// well and the total would not be 45.
		for (int i = 0; i < 10; i++) {
			src[i] = (byte) i;
			dst[i] = 0xff;
		}
		for (int i = 0; i < 10; i++)
			dst[i] = src[i];

		int total = 0;
		for (int i = 0; i < 10; i++)
			total += dst[i];
		return total;
	}

	// ---- cpblk and initblk ----

	public static unsafe int test_55_cpblk_from_a_constant_blob ()
	{
		// Roslyn turns a stackalloc with distinct constant initialisers into
		// localloc plus cpblk out of a metadata blob.
		byte* p = stackalloc byte[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
		int total = 0;
		for (int i = 0; i < 10; i++)
			total += p[i];
		return total;
	}

	public static unsafe int test_153_initblk_with_a_value ()
	{
		// One repeated value in the initialiser gives initblk instead.
		byte* p = stackalloc byte[] { 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9 };
		int total = 0;
		for (int i = 0; i < 17; i++)
			total += p[i];
		return total;
	}

	// ---- fixed ----

	public static unsafe int test_10_fixed_over_an_array ()
	{
		int[] a = new int[] { 1, 2, 3, 4 };
		fixed (int* p = a) {
			int total = 0;
			for (int i = 0; i < 4; i++)
				total += p[i];
			return total;
		}
	}

	public static unsafe int test_1_fixed_over_an_empty_array ()
	{
		// An array with no elements has no first element to point at, so the
		// pointer is null.
		int[] a = new int[IdI4 (0)];
		fixed (int* p = a)
			return p == null ? 1 : 0;
	}

	public static unsafe int test_7_fixed_over_an_array_element ()
	{
		int[] a = new int[3];
		fixed (int* p = &a[1])
			*p = IdI4 (7);
		return a[1];
	}

	public static unsafe int test_294_fixed_over_a_string ()
	{
		string s = IdStr ("abc");
		fixed (char* p = s)
			return p[0] + p[1] + p[2];
	}

	public static unsafe int test_1_fixed_string_keeps_its_terminator ()
	{
		string s = IdStr ("hi");
		fixed (char* p = s)
			return p[2] == '\0' ? 1 : 0;
	}

	public static unsafe int test_23_fixed_over_a_field ()
	{
		UnsafeCell c = new UnsafeCell ();
		fixed (int* p = &c.v)
			*p = IdI4 (23);
		return c.v;
	}

	// ---- sizeof ----

	public static unsafe int test_8_sizeof_pointer ()
	{
		return sizeof (void*);
	}

	public static unsafe int test_27_sizeof_structs ()
	{
		return sizeof (UnsafePair) + sizeof (UnsafeMixed) + sizeof (UnsafeBytes3);
	}

	// ---- pointer arithmetic ----

	public static unsafe int test_3_pointer_add_scales_by_element ()
	{
		int* p = stackalloc int[4];
		for (int i = 0; i < 4; i++)
			p[i] = i;
		int* q = p + IdI4 (3);
		return *q;
	}

	public static unsafe int test_5_pointer_subtract_scales_by_element ()
	{
		byte* p = stackalloc byte[8];
		for (int i = 0; i < 8; i++)
			p[i] = (byte) (i + 1);
		byte* end = p + 8;
		return *(end - IdI4 (4));
	}

	public static unsafe int test_3_pointer_difference_is_in_elements ()
	{
		long* p = stackalloc long[8];
		long* q = p + IdI4 (3);
		return (int) (q - p);
	}

	public static unsafe int test_1_pointer_compare ()
	{
		int* p = stackalloc int[4];
		int* q = p + IdI4 (1);
		return q > p && p < q && q != p ? 1 : 0;
	}

	public static unsafe int test_1_pointer_to_integer_round_trip ()
	{
		int v = IdI4 (42);
		int* p = &v;
		long bits = (long) p;
		int* q = (int*) bits;
		return *q == 42 ? 1 : 0;
	}

	public static unsafe int test_1_misaligned_i8_round_trip ()
	{
		byte* buf = stackalloc byte[16];
		long* p = (long*) (buf + IdI4 (1));
		*p = IdI8 (0x0102030405060708L);
		return *p == 0x0102030405060708L ? 1 : 0;
	}

	public static unsafe int test_1_misaligned_r8_round_trip ()
	{
		byte* buf = stackalloc byte[16];
		double* p = (double*) (buf + IdI4 (3));
		*p = IdR8 (1.5);
		return *p == 1.5 ? 1 : 0;
	}

	// ---- null pointers ----

	public static unsafe int test_1_ldind_i1_null_throws ()
	{
		try {
			sbyte* p = (sbyte*) IdPtr (null);
			return *p;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_ldind_i4_null_throws ()
	{
		try {
			int* p = (int*) IdPtr (null);
			return *p;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_ldind_i8_null_throws ()
	{
		try {
			long* p = (long*) IdPtr (null);
			return (int) *p;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_ldind_r8_null_throws ()
	{
		try {
			double* p = (double*) IdPtr (null);
			return (int) *p;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_ldind_i_null_throws ()
	{
		try {
			void** p = (void**) IdPtr (null);
			return *p == null ? 0 : 2;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_stind_i4_null_throws ()
	{
		try {
			int* p = (int*) IdPtr (null);
			*p = IdI4 (1);
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_stind_i1_null_throws ()
	{
		try {
			byte* p = (byte*) IdPtr (null);
			*p = (byte) IdI4 (1);
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}
}
