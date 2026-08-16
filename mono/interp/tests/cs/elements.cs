// The token forms of ldelem, stelem and ldelema. The interpreter picks the mint
// opcode from mint_type () of the token. C# names a primitive element with
// ldelem.i4 and its siblings, so the narrow arms of that mapping are reached only
// through a generic element type. Each width therefore reads and writes through a
// generic method here. Enums, a nullable, an interface, IntPtr and a readonly.
// address cover the remaining shapes mint_type () can see.
//
// arrays.cs holds the token forms on a struct, and the bounds, null and covariance
// checks the emitted opcodes share.
//
// Operands come out of NoInlining helpers, so the transform cannot fold an index or
// a value away.

using System;
using System.Runtime.CompilerServices;

public class Elements {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long IdL (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double IdD (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdO (object x) { return x; }

	// The element operations, each in a generic method, which is what makes the
	// opcode take a type token.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static T ElGet<T> (T[] a, int i) { return a[i]; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ElSet<T> (T[] a, int i, T v) { a[i] = v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ref T ElRef<T> (T[] a, int i) { return ref a[i]; }

	// A ref readonly return is what makes C# put a readonly. prefix on the
	// ldelema.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static ref readonly T ElPeek<T> (T[] a, int i) { return ref a[i]; }

	enum ElSByteEnum : sbyte { Zero = 0, Neg = -1 }
	enum ElByteEnum : byte { Zero = 0, Top = 255 }
	enum ElShortEnum : short { Zero = 0, Neg = -1 }
	enum ElUShortEnum : ushort { Zero = 0, Top = 65535 }
	enum ElIntEnum { Zero = 0, Big = 1000000 }
	enum ElLongEnum : long { Zero = 0, Big = 0x1122334455667788L }

	struct ElPair {
		public int X;
		public long Y;
	}

	interface IElNamed {
		int Weight ();
	}

	class ElRock : IElNamed {
		public int Weight () { return 5; }
	}

	class ElStone : IElNamed {
		public int Weight () { return 7; }
	}

	class ElBase {
		public virtual int Sides () { return 3; }
	}

	class ElLeaf : ElBase {
		public override int Sides () { return 4; }
	}

	// ------------------------------------------------------------------
	// A generic element type, one width per test
	// ------------------------------------------------------------------

	public static int test_1_generic_element_sbyte ()
	{
		sbyte[] a = new sbyte[Id (2)];

		ElSet<sbyte> (a, Id (1), (sbyte) Id (-1));
		return ElGet<sbyte> (a, Id (1)) == -1 ? 1 : 0;
	}

	public static int test_255_generic_element_byte ()
	{
		byte[] a = new byte[Id (2)];

		ElSet<byte> (a, Id (1), (byte) Id (255));
		return ElGet<byte> (a, Id (1));
	}

	public static int test_1_generic_element_bool ()
	{
		bool[] a = new bool[Id (2)];

		ElSet<bool> (a, Id (1), true);
		return ElGet<bool> (a, Id (1)) && !ElGet<bool> (a, Id (0)) ? 1 : 0;
	}

	public static int test_1_generic_element_short ()
	{
		short[] a = new short[Id (2)];

		ElSet<short> (a, Id (0), (short) Id (-1));
		return ElGet<short> (a, Id (0)) == -1 ? 1 : 0;
	}

	public static int test_65535_generic_element_ushort ()
	{
		ushort[] a = new ushort[Id (2)];

		ElSet<ushort> (a, Id (0), (ushort) Id (65535));
		return ElGet<ushort> (a, Id (0));
	}

	public static int test_1_generic_element_char ()
	{
		char[] a = new char[Id (2)];

		ElSet<char> (a, Id (1), (char) Id (0xffff));
		return ElGet<char> (a, Id (1)) == '\uffff' ? 1 : 0;
	}

	public static int test_7_generic_element_int ()
	{
		int[] a = new int[Id (3)];

		ElSet<int> (a, Id (2), Id (7));
		return ElGet<int> (a, Id (2));
	}

	public static int test_1_generic_element_uint ()
	{
		uint[] a = new uint[Id (2)];

		ElSet<uint> (a, Id (0), (uint) Id (-1));
		return ElGet<uint> (a, Id (0)) == uint.MaxValue ? 1 : 0;
	}

	public static int test_1_generic_element_long ()
	{
		long[] a = new long[Id (2)];

		ElSet<long> (a, Id (1), IdL (0x1122334455667788L));
		return ElGet<long> (a, Id (1)) == 0x1122334455667788L ? 1 : 0;
	}

	// 0.1 has no exact float. An element that still equals the double would show
	// that the store kept eight bytes.
	public static int test_1_generic_element_float ()
	{
		float[] a = new float[Id (2)];

		ElSet<float> (a, Id (1), (float) IdD (0.1));
		return ElGet<float> (a, Id (1)) == (float) IdD (0.1)
			&& (double) ElGet<float> (a, Id (1)) != 0.1 ? 1 : 0;
	}

	public static int test_3_generic_element_double ()
	{
		double[] a = new double[Id (2)];

		ElSet<double> (a, Id (0), IdD (1.5));
		ElSet<double> (a, Id (1), IdD (1.5));
		return (int) (ElGet<double> (a, Id (0)) + ElGet<double> (a, Id (1)));
	}

	public static int test_4_generic_element_class ()
	{
		ElBase[] a = new ElBase[Id (2)];

		ElSet<ElBase> (a, Id (1), new ElLeaf ());
		return ElGet<ElBase> (a, Id (1)).Sides ();
	}

	public static int test_12_generic_element_interface ()
	{
		IElNamed[] a = new IElNamed[Id (2)];

		ElSet<IElNamed> (a, Id (0), new ElRock ());
		ElSet<IElNamed> (a, Id (1), new ElStone ());
		return ElGet<IElNamed> (a, Id (0)).Weight () + ElGet<IElNamed> (a, Id (1)).Weight ();
	}

	public static int test_1_generic_element_string ()
	{
		string[] a = new string[Id (2)];

		ElSet<string> (a, Id (1), (string) IdO ("kept"));
		return ElGet<string> (a, Id (1)) == "kept" ? 1 : 0;
	}

	// A generic instance reaches mint_type () through its container class, which
	// is a different path from a plain struct.
	public static int test_1_generic_element_nullable ()
	{
		int?[] a = new int?[Id (2)];

		ElSet<int?> (a, Id (1), Id (5));
		int? v = ElGet<int?> (a, Id (1));
		int? empty = ElGet<int?> (a, Id (0));
		return v.HasValue && v.Value == 5 && !empty.HasValue ? 1 : 0;
	}

	// MINT_TYPE_I is a name for the pointer-wide arm, so IntPtr must round-trip
	// all eight bytes here and four on a 32-bit port.
	public static int test_1_generic_element_intptr ()
	{
		IntPtr[] a = new IntPtr[Id (2)];

		ElSet<IntPtr> (a, Id (1), new IntPtr (IdL (0x1122334455667788L)));
		return ElGet<IntPtr> (a, Id (1)).ToInt64 () == 0x1122334455667788L ? 1 : 0;
	}

	// ------------------------------------------------------------------
	// An enum of each width, which mint_type () re-dispatches on the
	// underlying type
	// ------------------------------------------------------------------

	public static int test_1_generic_element_sbyte_enum ()
	{
		ElSByteEnum[] a = new ElSByteEnum[Id (2)];

		ElSet<ElSByteEnum> (a, Id (1), (ElSByteEnum) Id (-1));
		return ElGet<ElSByteEnum> (a, Id (1)) == ElSByteEnum.Neg ? 1 : 0;
	}

	public static int test_255_generic_element_byte_enum ()
	{
		ElByteEnum[] a = new ElByteEnum[Id (2)];

		ElSet<ElByteEnum> (a, Id (1), (ElByteEnum) Id (255));
		return (int) ElGet<ElByteEnum> (a, Id (1));
	}

	public static int test_1_generic_element_short_enum ()
	{
		ElShortEnum[] a = new ElShortEnum[Id (2)];

		ElSet<ElShortEnum> (a, Id (0), (ElShortEnum) Id (-1));
		return (short) ElGet<ElShortEnum> (a, Id (0)) == -1 ? 1 : 0;
	}

	public static int test_65535_generic_element_ushort_enum ()
	{
		ElUShortEnum[] a = new ElUShortEnum[Id (2)];

		ElSet<ElUShortEnum> (a, Id (0), (ElUShortEnum) Id (65535));
		return (ushort) ElGet<ElUShortEnum> (a, Id (0));
	}

	public static int test_1_generic_element_int_enum ()
	{
		ElIntEnum[] a = new ElIntEnum[Id (2)];

		ElSet<ElIntEnum> (a, Id (1), (ElIntEnum) Id (1000000));
		return ElGet<ElIntEnum> (a, Id (1)) == ElIntEnum.Big ? 1 : 0;
	}

	public static int test_1_generic_element_long_enum ()
	{
		ElLongEnum[] a = new ElLongEnum[Id (2)];

		ElSet<ElLongEnum> (a, Id (1), (ElLongEnum) IdL (0x1122334455667788L));
		return ElGet<ElLongEnum> (a, Id (1)) == ElLongEnum.Big ? 1 : 0;
	}

	// ------------------------------------------------------------------
	// ldelema with a type token
	// ------------------------------------------------------------------

	// The managed pointer leaves the frame that made it, so the caller stores
	// through an address the callee computed.
	public static int test_5_generic_address_of_struct ()
	{
		ElPair[] a = new ElPair[Id (2)];

		ElRef<ElPair> (a, Id (0)).X = Id (5);
		return a[Id (0)].X;
	}

	public static int test_1_generic_address_of_enum ()
	{
		ElLongEnum[] a = new ElLongEnum[Id (2)];

		ElRef<ElLongEnum> (a, Id (1)) = (ElLongEnum) IdL (0x1122334455667788L);
		return a[Id (1)] == ElLongEnum.Big ? 1 : 0;
	}

	public static int test_1_generic_address_of_nullable ()
	{
		int?[] a = new int?[Id (2)];

		ElRef<int?> (a, Id (1)) = Id (6);
		return a[Id (1)].Value == 6 ? 1 : 0;
	}

	// An interface element type sends the transform through mono_class_setup_vtable
	// on an interface, and the address through the type check.
	public static int test_1_generic_address_of_interface ()
	{
		IElNamed[] a = new IElNamed[Id (2)];

		ElRef<IElNamed> (a, Id (0)) = new ElStone ();
		return a[Id (0)].Weight () == 7 ? 1 : 0;
	}

	// A readonly. prefix drops the type check, so a reference element takes the
	// same arm a value type does. Reading through the address is then legal where
	// an object& into a string[] is not.
	public static int test_1_readonly_address_skips_the_type_check ()
	{
		object[] a = new string[Id (2)];

		a[Id (0)] = (string) IdO ("only readable");
		return (string) ElPeek<object> (a, Id (0)) == "only readable" ? 1 : 0;
	}

	// ------------------------------------------------------------------
	// The null check the wide load carries on its own
	// ------------------------------------------------------------------

	public static int test_1_generic_ldelem_on_null_throws ()
	{
		ElPair[] a = (ElPair[]) IdO (null);

		try {
			return (int) ElGet<ElPair> (a, Id (0)).Y;
		} catch (NullReferenceException) {
			return 1;
		}
	}
}
