// castclass, isinst and unbox.any.
//
// The transform picks the opcode from the target class alone. A non-variant
// interface gets the interface pair, and a plain class gets the "common" pair.
// Arrays, nullables, MarshalByRefObject and variant generic interfaces get the
// general pair. The type a cast names therefore selects the handler under test.
//
// Operands go through IdO, a NoInlining helper. C# emits no cast opcode for a
// conversion it proves at compile time, and the transform must not know the
// object's class either.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

public class Casts {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdO (object o) { return o; }

	class CastsBase { public virtual int Tag () { return 1; } }
	class CastsMiddle : CastsBase { public override int Tag () { return 2; } }
	sealed class CastsLeaf : CastsMiddle { public override int Tag () { return 3; } }
	class CastsOther { }

	interface ICastsMarker { int Mark (); }
	class CastsMarked : CastsBase, ICastsMarker { public int Mark () { return 7; } }
	class CastsMarkedChild : CastsMarked { }

	struct CastsPoint : ICastsMarker { public int X; public int Mark () { return 9; } }

	interface ICastsPair<T> { }
	class CastsPairImpl : ICastsPair<int> { }

	interface ICastsSource<out T> { }
	class CastsSourceImpl : ICastsSource<string> { }

	interface ICastsSink<in T> { }
	class CastsSinkImpl : ICastsSink<CastsMiddle> { }

	class CastsCell<T> { public T Value; }

	class CastsRemote : MarshalByRefObject { public int Mark () { return 11; } }

	enum CastsColor { Red = 3, Green = 4 }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T CastsTo<T> (object o) where T : class { return (T) o; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T CastsAs<T> (object o) where T : class { return o as T; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T CastsUnboxTo<T> (object o) { return (T) o; }

	public static int test_1_castclass_null_succeeds ()
	{
		object o = IdO (null);

		if ((CastsBase) o != null)
			return 0;
		if ((ICastsMarker) o != null)
			return 0;
		if ((int[]) o != null)
			return 0;
		return 1;
	}

	public static int test_1_isinst_null_is_null ()
	{
		object o = IdO (null);

		if (o as CastsBase != null)
			return 0;
		if (o as ICastsMarker != null)
			return 0;
		if (o as int[] != null)
			return 0;
		return 1;
	}

	public static int test_3_castclass_common_exact ()
	{
		return ((CastsLeaf) IdO (new CastsLeaf ())).Tag ();
	}

	public static int test_2_castclass_common_upcast ()
	{
		return ((CastsBase) IdO (new CastsMiddle ())).Tag ();
	}

	public static int test_1_castclass_common_unrelated_throws ()
	{
		try {
			return ((CastsBase) IdO (new CastsOther ())).Tag ();
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_1_castclass_common_downcast_throws ()
	{
		try {
			return ((CastsLeaf) IdO (new CastsMiddle ())).Tag ();
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_3_isinst_common_matches ()
	{
		CastsBase b = IdO (new CastsLeaf ()) as CastsBase;
		return b == null ? 0 : b.Tag ();
	}

	public static int test_1_isinst_common_fails ()
	{
		if (IdO (new CastsOther ()) as CastsBase != null)
			return 0;
		if (IdO (new CastsMiddle ()) as CastsLeaf != null)
			return 0;
		return 1;
	}

	public static int test_7_castclass_interface ()
	{
		return ((ICastsMarker) IdO (new CastsMarked ())).Mark ();
	}

	public static int test_7_castclass_interface_through_base ()
	{
		return ((ICastsMarker) IdO (new CastsMarkedChild ())).Mark ();
	}

	public static int test_1_isinst_interface_fails ()
	{
		return IdO (new CastsMiddle ()) as ICastsMarker == null ? 1 : 0;
	}

	public static int test_1_castclass_interface_throws ()
	{
		try {
			return ((ICastsMarker) IdO (new CastsMiddle ())).Mark ();
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_9_castclass_interface_on_boxed_struct ()
	{
		return ((ICastsMarker) IdO (new CastsPoint ())).Mark ();
	}

	public static int test_1_isinst_interface_on_boxed_struct_fails ()
	{
		return IdO (new CastsPoint ()) as ICastsPair<int> == null ? 1 : 0;
	}

	public static int test_1_castclass_generic_interface ()
	{
		object o = IdO (new CastsPairImpl ());

		if ((ICastsPair<int>) o == null)
			return 0;
		try {
			return (ICastsPair<string>) o == null ? 0 : 2;
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_1_isinst_generic_interface_wrong_argument ()
	{
		return IdO (new CastsPairImpl ()) as ICastsPair<string> == null ? 1 : 0;
	}

	public static int test_1_castclass_covariant_interface ()
	{
		object o = IdO (new CastsSourceImpl ());

		if ((ICastsSource<object>) o == null)
			return 0;
		try {
			// out T reaches a base of string, and CastsBase is not one.
			return (ICastsSource<CastsBase>) o == null ? 0 : 2;
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_1_isinst_covariant_interface_value_argument ()
	{
		// Variance applies to reference type arguments only.
		return IdO (new CastsSourceImpl ()) as ICastsSource<int> == null ? 1 : 0;
	}

	public static int test_1_castclass_contravariant_interface ()
	{
		object o = IdO (new CastsSinkImpl ());

		if ((ICastsSink<CastsLeaf>) o == null)
			return 0;
		try {
			// in T reaches a class derived from CastsMiddle, not a base of it.
			return (ICastsSink<CastsBase>) o == null ? 0 : 2;
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_5_castclass_generic_class ()
	{
		CastsCell<int> cell = new CastsCell<int> ();
		cell.Value = 5;
		return ((CastsCell<int>) IdO (cell)).Value;
	}

	public static int test_1_isinst_generic_class_wrong_argument ()
	{
		return IdO (new CastsCell<string> ()) as CastsCell<object> == null ? 1 : 0;
	}

	public static int test_1_castclass_generic_class_throws ()
	{
		try {
			return ((CastsCell<long>) IdO (new CastsCell<int> ())) == null ? 0 : 2;
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_2_castclass_array_exact ()
	{
		return ((int[]) IdO (new int[2])).Length;
	}

	public static int test_2_castclass_array_covariant ()
	{
		return ((object[]) IdO (new string[2])).Length;
	}

	public static int test_1_isinst_array_covariant ()
	{
		return IdO (new CastsLeaf[2]) as CastsBase[] == null ? 0 : 1;
	}

	public static int test_1_castclass_array_throws ()
	{
		try {
			return ((string[]) IdO (new object[2])).Length;
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_1_isinst_array_rank_mismatch ()
	{
		if (IdO (new int[2, 2]) as int[] != null)
			return 0;
		if (IdO (new int[2]) as int[,] != null)
			return 0;
		return 1;
	}

	public static int test_1_isinst_array_element_equivalence ()
	{
		// Signed and unsigned elements of one width are the same element type
		// for a cast. Two different widths are not.
		if (IdO (new uint[2]) as int[] == null)
			return 0;
		if (IdO (new int[2]) as short[] != null)
			return 0;
		return 1;
	}

	public static int test_3_castclass_to_system_array ()
	{
		return ((Array) IdO (new int[3])).Length;
	}

	public static int test_3_castclass_array_special_interface ()
	{
		return ((IList<int>) IdO (new int[] { 3, 4 }))[0];
	}

	public static int test_1_isinst_array_special_interface_fails ()
	{
		return IdO (new int[2]) as IList<string> == null ? 1 : 0;
	}

	public static int test_1_isinst_covariant_array_special_interface ()
	{
		return IdO (new string[2]) as IList<object> == null ? 0 : 1;
	}

	public static int test_1_isinst_array_as_variant_enumerable ()
	{
		return IdO (new string[2]) as IEnumerable<object> == null ? 0 : 1;
	}

	public static int test_11_castclass_marshalbyref ()
	{
		return ((CastsRemote) IdO (new CastsRemote ())).Mark ();
	}

	public static int test_1_isinst_marshalbyref_fails ()
	{
		return IdO (new CastsOther ()) as CastsRemote == null ? 1 : 0;
	}

	public static int test_42_unbox_any_int ()
	{
		return (int) IdO (42);
	}

	public static int test_1_unbox_any_width_mismatch_throws ()
	{
		try {
			return (int) IdO ((byte) 3);
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_1_unbox_any_null_throws ()
	{
		try {
			return (int) IdO (null);
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_3_unbox_any_struct ()
	{
		CastsPoint p = new CastsPoint ();
		p.X = 3;
		return ((CastsPoint) IdO (p)).X;
	}

	public static int test_4_unbox_any_enum_and_underlying ()
	{
		// unbox accepts the underlying type of an enum in both directions.
		// isinst refuses the pair, in test_1_isinst_boxed_value_types.
		int n = (int) IdO (CastsColor.Green);
		CastsColor c = (CastsColor) IdO (4);
		return n == (int) c ? n : 0;
	}

	public static int test_1_isinst_boxed_value_types ()
	{
		if (!(IdO (7) is int))
			return 0;
		if (!(IdO (7) is ValueType))
			return 0;
		if (IdO (7) is string)
			return 0;
		if (IdO (CastsColor.Green) is int)
			return 0;
		if (!(IdO (CastsColor.Green) is Enum))
			return 0;
		return 1;
	}

	public static int test_7_nullable_unbox_any ()
	{
		int? n = (int?) IdO (7);
		return n.Value;
	}

	public static int test_1_nullable_unbox_any_null_and_mismatch ()
	{
		int? a = (int?) IdO (null);

		if (a.HasValue)
			return 0;
		try {
			int? b = (int?) IdO ((long) 1);
			return b.HasValue ? 0 : 2;
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_3_generic_type_parameter_cast ()
	{
		if (CastsAs<CastsOther> (IdO (new CastsLeaf ())) != null)
			return 0;
		return CastsTo<CastsLeaf> (IdO (new CastsLeaf ())).Tag ();
	}

	public static int test_42_generic_type_parameter_unbox ()
	{
		return CastsUnboxTo<int> (IdO (42));
	}

	public static int test_3_castclass_string ()
	{
		return ((string) IdO ("abc")).Length;
	}
}
