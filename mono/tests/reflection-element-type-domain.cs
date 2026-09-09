// RuntimeTypeHandle.GetElementType () reads an element class's vtable out of
// MonoClassRuntimeInfo::domain_vtables [domain]. That array exists once any
// domain has built a vtable for the class, but a domain's own slot in it
// stays null until that domain has. The translator answered with a
// NullReferenceException instead of falling back to the icall. The read was
// speculated past the null check meant to catch exactly that case.

using System;
using System.Threading;

public class ReflectionElementTypeDomain {
	// Named only through Fresher.Build (), never by a typeof () anywhere in
	// this file, so nothing but that one call ever resolves this class.
	public class Fresh {
	}

	public class Fresher : MarshalByRefObject {
		// Building an array does not build Fresh's own vtable. Allocating
		// one does, giving Fresh a MonoClassRuntimeInfo with this domain's
		// own vtable slot filled in and the root domain's left null.
		public void Build ()
		{
			GC.KeepAlive (new Fresh ());
		}
	}

	// object's vtable exists in every domain from startup, so this never
	// takes the path under test. It exists only to promote
	// RuntimeTypeHandle.GetElementType () to a compiled tier in the root
	// domain before Fresh is ever named there.
	static string ElementTypeName (Type elementType)
	{
		Array array = Array.CreateInstance (elementType, 0);
		Type element = array.GetType ().GetElementType ();

		return element.FullName;
	}

	public static int Main ()
	{
		for (int i = 0; i < 200000; ++i)
			ElementTypeName (typeof (object));

		// The promotion this warm-up asked for compiles on a background
		// thread. Sleep gives it time to publish before the real check runs
		// against the old, unpromoted body.
		Thread.Sleep (2000);

		AppDomain domain = AppDomain.CreateDomain ("reflection-element-type-domain");
		Fresher fresher = (Fresher) domain.CreateInstanceAndUnwrap (
			typeof (Fresher).Assembly.FullName, typeof (Fresher).FullName);

		fresher.Build ();

		string wanted = typeof (Fresh).FullName;
		string got = ElementTypeName (typeof (Fresh));

		AppDomain.Unload (domain);

		if (got != wanted) {
			Console.WriteLine ("FAILED: got {0}, wanted {1}", got, wanted);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
