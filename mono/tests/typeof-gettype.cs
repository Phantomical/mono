using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;

/*
 * What typeof (T) and object.GetType () answer, over the shapes the generated
 * code either answers itself or leaves to the runtime.
 *
 * The compiled tiers fold typeof (T) into the address of the System.Type the
 * domain holds, and read GetType () out of the receiver's vtable. The
 * interpreter answers both without a call as well. So what these cases pin is
 * that the three engines agree, and that one type has one object however a
 * site reaches it.
 *
 * A shared generic body cannot hold the address, because each instantiation
 * has a type of its own. It reads the runtime generic context instead, which
 * is why every probe below runs against several instantiations.
 *
 * Every probe is entered enough times to reach both compiled tiers.
 */

interface IAlpha { }

class Base { }
class Derived : Base, IAlpha { }

struct Val { public int n; }

class Gen<T> {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Type Of () { return typeof (T); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Type OfArray () { return typeof (T[]); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Type OfList () { return typeof (List<T>); }
}

class Remote : MarshalByRefObject { }

class Proxy : RealProxy {
	public Proxy () : base (typeof (Remote)) { }

	public override IMessage Invoke (IMessage request)
	{
		throw new NotSupportedException ("the proxy answers no call");
	}
}

static class Program {
	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/* typeof, which the compiled tiers answer with a constant. */
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfBase () { return typeof (Base); }
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfVal () { return typeof (Val); }
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfArray () { return typeof (Base[]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfInterface () { return typeof (IAlpha); }
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfInstance () { return typeof (List<Base>); }
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfDefinition () { return typeof (Gen<>); }
	[MethodImpl (MethodImplOptions.NoInlining)] static Type OfInt () { return typeof (int); }

	/* GetType, which the compiled tiers read off the object's vtable. */
	[MethodImpl (MethodImplOptions.NoInlining)] static Type TypeOf (object o) { return o.GetType (); }

	/* The constrained. spelling, which a generic parameter reaches GetType through. */
	[MethodImpl (MethodImplOptions.NoInlining)] static Type TypeOfParam<T> (T t) { return t.GetType (); }

	/* The handle by itself, which no call follows and no fold takes. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static RuntimeTypeHandle HandleOfBase () { return typeof (Base).TypeHandle; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Type ThroughHandle (RuntimeTypeHandle h) { return Type.GetTypeFromHandle (h); }

	/*
	 * Each shape describes itself, which is what says the object is the right
	 * one rather than merely a stable one. Reading these properties allocates,
	 * so they stay out of the loop below.
	 */
	static void Shapes ()
	{
		Check (OfBase ().FullName == "Base", "typeof (Base)");
		Check (OfVal ().IsValueType, "typeof (Val) is a value type");
		Check (OfArray ().IsArray, "typeof (Base[]) is an array");
		Check (OfInterface ().IsInterface, "typeof (IAlpha) is an interface");
		Check (OfInstance ().IsGenericType, "typeof (List<Base>) is a generic instance");
		Check (OfDefinition ().IsGenericTypeDefinition, "typeof (Gen<>) is a definition");
		Check (TypeOf (OfBase ()).Name == "RuntimeType", "a type object is a RuntimeType");
		Check (TypeOf (new Base ()).FullName == "Base", "GetType () on a Base");
		Check (Gen<Derived>.Of ().FullName == "Derived", "shared: typeof (T) is Derived");
	}

	static int Run (int i)
	{
		object b = new Base (), d = new Derived ();
		object boxed = (object) new Val { n = i };
		object array = new Base [2];
		object list = new List<Base> ();
		object text = "text";

		/* One type has one object, however the site reaches it. */
		Check ((object) OfBase () == (object) typeof (Base), "typeof (Base) is one object");
		Check ((object) OfInt () == (object) typeof (int), "typeof (int) is one object");
		Check (OfBase () != OfVal (), "two types are two objects");

		/* GetType, against the typeof of the same class. */
		Check (TypeOf (b) == OfBase (), "new Base ().GetType ()");
		Check (TypeOf (d) == typeof (Derived), "the class, not the base");
		Check (TypeOf (d) != OfBase (), "a subclass is not its base");
		Check (TypeOf (boxed) == OfVal (), "a boxed value answers its own class");
		Check (TypeOf (array) == OfArray (), "an array answers its own class");
		Check (TypeOf (list) == OfInstance (), "a generic instance answers its own class");
		Check (TypeOf (text) == typeof (string), "a string answers its own class");

		/* A type object is itself an object, and its own class answers too. */
		Check (TypeOf (OfBase ()) == TypeOf (OfVal ()), "two type objects share a class");

		/* The constrained. spelling, on both a reference and a value type. */
		Check (TypeOfParam<Base> ((Base) b) == OfBase (), "constrained: a reference type");
		Check (TypeOfParam<Derived> ((Derived) d) == typeof (Derived), "constrained: a subclass");
		Check (TypeOfParam<Val> (new Val { n = i }) == OfVal (), "constrained: a value type");
		Check (TypeOfParam<int> (i) == OfInt (), "constrained: an int");

		/*
		 * A shared body serves every reference instantiation, so it reads its
		 * type out of the runtime generic context instead of holding one.
		 */
		Check (Gen<Base>.Of () == OfBase (), "shared: typeof (T) is Base");
		Check (Gen<Derived>.Of () == typeof (Derived), "shared: typeof (T) is a subclass");
		Check (Gen<string>.Of () == typeof (string), "shared: typeof (T) is string");
		Check (Gen<Base>.OfArray () == OfArray (), "shared: typeof (T[])");
		Check (Gen<Base>.OfList () == OfInstance (), "shared: typeof (List<T>)");

		/* A value type instantiation gets a body of its own. */
		Check (Gen<Val>.Of () == OfVal (), "unshared: typeof (T) is Val");
		Check (Gen<int>.Of () == OfInt (), "unshared: typeof (T) is int");

		/* The handle on its own, which keeps its call to the runtime. */
		Check (ThroughHandle (HandleOfBase ()) == OfBase (), "a handle through Type.GetTypeFromHandle");

		return fails;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Type OfNull ()
	{
		object o = null;
		return o.GetType ();
	}

	/*
	 * typeof on a type that Reflection.Emit has not created yet. The runtime
	 * answers with the builder's own object rather than with a pinned
	 * RuntimeType, so a moving collector can move it and the compiled tiers
	 * have to leave the site alone there. The answer is the same object either
	 * way, which is what this asks, because a test that reads the shape would
	 * read a different one under each collector.
	 */
	static Func<Type> EmitTypeOfUncreated (out TypeBuilder built)
	{
		AssemblyName name = new AssemblyName ("typeof-gettype-emit");
		AssemblyBuilder assembly = AppDomain.CurrentDomain.DefineDynamicAssembly (
			name, AssemblyBuilderAccess.Run);
		ModuleBuilder module = assembly.DefineDynamicModule ("m");

		built = module.DefineType ("Emitted", TypeAttributes.Public);

		DynamicMethod emitted = new DynamicMethod (
			"OfEmitted", typeof (Type), Type.EmptyTypes, typeof (Program), true);
		ILGenerator il = emitted.GetILGenerator ();

		il.Emit (OpCodes.Ldtoken, built);
		il.Emit (OpCodes.Call, typeof (Type).GetMethod ("GetTypeFromHandle"));
		il.Emit (OpCodes.Ret);

		return (Func<Type>) emitted.CreateDelegate (typeof (Func<Type>));
	}

	public static int Main ()
	{
		Shapes ();

		/* Enough entries to leave the interpreter and both compiled tiers. */
		for (int i = 0; i < 30000; ++i) {
			if (Run (i) != 0) {
				Console.WriteLine ("stopped at iteration {0}", i);
				return 1;
			}
		}

		/* A receiver of null raises, whichever engine reads the vtable. */
		try {
			OfNull ();
			Console.WriteLine ("FAIL: GetType () on null did not throw");
			++fails;
		} catch (NullReferenceException) {
		}

		/*
		 * A transparent proxy answers the type it stands for. Its vtable is a
		 * copy of that class's, so the read gives the same answer the remoting
		 * wrapper does, and no call reaches the RealProxy above.
		 */
		object proxy = new Proxy ().GetTransparentProxy ();

		Check (proxy.GetType () == typeof (Remote), "a transparent proxy answers Remote");
		Check (TypeOf (proxy) == typeof (Remote), "a proxy through a compiled caller");

		/* A type Reflection.Emit has not created, entered enough to compile. */
		TypeBuilder built;
		Func<Type> of_emitted = EmitTypeOfUncreated (out built);

		for (int i = 0; i < 30000; ++i)
			if (!ReferenceEquals (of_emitted (), built)) {
				Console.WriteLine ("FAIL: typeof on an uncreated builder type,"
				                   + " iteration {0}", i);
				++fails;
				break;
			}

		/* The shapes again, now that both compiled tiers have the bodies. */
		Shapes ();

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
