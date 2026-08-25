// The translator answers RuntimeTypeHandle.GetCorElementType () and
// RuntimeTypeHandle.GetElementType () out of memory instead of entering the
// icall. Each case below is one the emitters must agree with the icall on, and
// each runs enough times to reach the compiled tiers.

using System;
using System.Reflection;
using System.Reflection.Emit;

public class ReflectionElementType {
	static int fails;

	static void Check (string what, object got, object want)
	{
		if (Equals (got, want))
			return;

		Console.WriteLine ("FAILED: {0}: got {1}, wanted {2}",
				what, got ?? "null", want ?? "null");
		++fails;
	}

	static string Raises (Action body)
	{
		try {
			body ();
			return "none";
		} catch (Exception e) {
			return e.GetType ().Name;
		}
	}

	// The tag the two emitters read. Every one of these reaches
	// GetCorElementType, and the answers must not move between tiers.
	static void Tags ()
	{
		Check ("int is primitive", typeof (int).IsPrimitive, true);
		Check ("string is not primitive", typeof (string).IsPrimitive, false);
		Check ("int[] is an array", typeof (int[]).IsArray, true);
		Check ("int[,] is an array", typeof (int[,]).IsArray, true);
		Check ("int is not an array", typeof (int).IsArray, false);
		Check ("int* is a pointer", typeof (int).MakePointerType ().IsPointer, true);
		Check ("int[] is not a pointer", typeof (int[]).IsPointer, false);
		Check ("int& is byref", typeof (int).MakeByRefType ().IsByRef, true);
		Check ("int is not byref", typeof (int).IsByRef, false);
		Check ("int[] has an element type", typeof (int[]).HasElementType, true);
		Check ("string has no element type", typeof (string).HasElementType, false);
	}

	// GetElementType (). The first is the shape the emitter answers inline and
	// the rest are the ones it leaves on the icall.
	static void Elements ()
	{
		Check ("element of int[]", typeof (int[]).GetElementType (), typeof (int));
		Check ("element of string[]", typeof (string[]).GetElementType (), typeof (string));
		Check ("element of int[][]", typeof (int[][]).GetElementType (), typeof (int[]));
		Check ("element of int[,]", typeof (int[,]).GetElementType (), typeof (int));
		Check ("element of int*", typeof (int).MakePointerType ().GetElementType (),
			typeof (int));
		Check ("element of int&", typeof (int).MakeByRefType ().GetElementType (),
			typeof (int));
		Check ("element of int", typeof (int).GetElementType (), null);
		Check ("element of a generic instance",
			typeof (System.Collections.Generic.List<int>).GetElementType (), null);

		// A rank-one array with a lower bound is MONO_TYPE_ARRAY rather than
		// MONO_TYPE_SZARRAY, so it takes the declined edge.
		Array bound = Array.CreateInstance (typeof (int), new int [] { 3 }, new int [] { 1 });

		Check ("element of a bound rank-one array", bound.GetType ().GetElementType (),
			typeof (int));
		Check ("a bound rank-one array is not an szarray",
			bound.GetType () == typeof (int[]), false);
	}

	// What task 257 is about: Array.GetValue () and SetValue () ask both
	// questions once for each element.
	static void Values ()
	{
		int[] numbers = { 10, 20, 30 };

		Check ("GetValue on int[]", numbers.GetValue (1), 20);
		numbers.SetValue (99, 1);
		Check ("SetValue on int[]", numbers[1], 99);

		string[] words = { "a", "b" };

		Check ("GetValue on string[]", words.GetValue (0), "a");

		Array pointers = Array.CreateInstance (typeof (int).MakePointerType (), 2);

		Check ("GetValue on a pointer array",
			Raises (() => pointers.GetValue (0)), "NotSupportedException");
		Check ("SetValue on a pointer array",
			Raises (() => pointers.SetValue (null, 0)), "NotSupportedException");

		Array square = Array.CreateInstance (typeof (int), 2, 2);

		Check ("GetValue with one index on a rank-two array",
			Raises (() => square.GetValue (0)), "ArgumentException");

		Array bound = Array.CreateInstance (typeof (int), new int [] { 3 }, new int [] { 1 });

		bound.SetValue (7, 1);
		Check ("GetValue on a bound rank-one array", bound.GetValue (1), 7);
		Check ("GetValue below the lower bound",
			Raises (() => bound.GetValue (0)), "IndexOutOfRangeException");
	}

	// A module Reflection.Emit built is what the dynamic-image guard is for.
	// The vtable of such a class can hold an object the icall does not answer
	// with, so the site has to keep its call.
	static void Emitted ()
	{
		AssemblyBuilder assembly = AppDomain.CurrentDomain.DefineDynamicAssembly (
			new AssemblyName ("ReflectionElementTypeEmitted"), AssemblyBuilderAccess.Run);
		ModuleBuilder module = assembly.DefineDynamicModule ("Emitted");
		TypeBuilder builder = module.DefineType ("Emitted.Point", TypeAttributes.Public);
		Type built = builder.CreateType ();
		Array array = Array.CreateInstance (built, 2);

		Check ("element of an emitted array", array.GetType ().GetElementType (), built);
		Check ("an emitted class is not an array", built.IsArray, false);
		Check ("GetValue on an emitted array", array.GetValue (0), null);
	}

	public static int Main ()
	{
		// Once for the answers, then enough times to promote the bodies that
		// hold them and ask again against compiled code.
		Tags ();
		Elements ();
		Values ();
		Emitted ();

		int before = fails;

		for (int i = 0; i < 20000; ++i) {
			Tags ();
			Elements ();
			Values ();
		}

		if (fails != before)
			Console.WriteLine ("FAILED: the answers moved once the bodies compiled");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
