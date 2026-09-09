using System;
using System.Runtime.CompilerServices;

/*
 * emit_object_alloc () and emit_vector_alloc () (mono/llvm/method-to-llvm/)
 * ask mono_gc_alloc_obj_shape () and mono_gc_alloc_vector_shape () for an
 * allocation shape once Boehm answers null for a managed allocator. Both
 * read the target MonoClass directly.
 *
 * A shared body's target is the compile-time canonical form
 * get_shared_type () (mini-generic-sharing.c) builds for each
 * reference-typed generic parameter. Its own fields carry the VAR/MVAR
 * placeholder rather than a concrete type. mono_gc_alloc_obj_shape () walks
 * those fields through mono_class_compute_gc_descriptor () and aborts in
 * compute_class_bitmap () (object.c) with "Invalid type ... for field".
 *
 * depends_on_context () (generic-sharing.cpp) is the predicate vtable_for ()
 * (fields.cpp) already gates the vtable operand with. Both allocation sites
 * now gate the shape lookup the same way, answering GENERIC instead, which
 * reads the shape off the vtable the RGCTX already resolved.
 *
 * Cell<T> shares one body across every reference instantiation of T.
 * MakeNext () allocates a fresh Cell<T>, the newobj target a shared body
 * sees as the canonical form. MakeArray<T> allocates a T[] the same way.
 * Both run against two distinct reference instantiations, string and
 * object. That way the RGCTX fills a slot a real caller asks for, rather
 * than one that only ever serves a single type.
 *
 * `-mono-tier0-filter=0` (runtime-suites.cmake) routes the shared body
 * through the backend from its first call. The classic compiler builds a
 * vtable through mono_class_vtable () for the concrete instantiation
 * instead, and never reaches either lookup above.
 */

public class GsharedAllocShape
{
	class Cell<T> where T : class
	{
		public T Value;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public Cell<T> MakeNext (T value)
		{
			Cell<T> next = new Cell<T> ();
			next.Value = value;
			return next;
		}
	}

	static int failures;

	static void Check (string what, bool ok)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL {0}", what);
		++failures;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T[] MakeArray<T> (T value, int n) where T : class
	{
		T[] arr = new T[n];
		arr[0] = value;
		return arr;
	}

	public static int Main ()
	{
		Cell<string> strHead = new Cell<string> ();
		Cell<string> strNext = strHead.MakeNext ("a");
		Check ("Cell<string>.MakeNext value", strNext.Value == "a");

		object obj = new object ();
		Cell<object> objHead = new Cell<object> ();
		Cell<object> objNext = objHead.MakeNext (obj);
		Check ("Cell<object>.MakeNext value", ReferenceEquals (objNext.Value, obj));

		string[] strArr = MakeArray ("b", 4);
		Check ("MakeArray<string> length", strArr.Length == 4);
		Check ("MakeArray<string> value", strArr[0] == "b");

		object[] objArr = MakeArray (obj, 4);
		Check ("MakeArray<object> length", objArr.Length == 4);
		Check ("MakeArray<object> value", ReferenceEquals (objArr[0], obj));

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
