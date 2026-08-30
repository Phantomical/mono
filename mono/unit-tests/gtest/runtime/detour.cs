using System.Runtime.CompilerServices;

/*
 * The methods test-detour.cpp installs a detour over, and the callers that have
 * to reach it.
 */
public class Detour
{
	/*
	 * NoInlining is what keeps the interpreter from copying this body into its
	 * caller. A jump written over an entry address cannot reach a copy.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Target (int x)
	{
		return x + 1;
	}

	/* Small enough for the interpreter to inline, which is the arm that misses. */
	public static int Inlined (int x)
	{
		return x + 1;
	}

	public static int CallTarget (int x)
	{
		return Target (x);
	}

	/*
	 * A second Target/CallTarget pair. CallLateTarget calls it once before
	 * LateTarget gets a detour. That first call is what settles
	 * resolve_code_type ()'s answer while LateTarget still has no code.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int LateTarget (int x)
	{
		return x + 1;
	}

	public static int CallLateTarget (int x)
	{
		return LateTarget (x);
	}

	public static int CallInlined (int x)
	{
		return Inlined (x);
	}

	/*
	 * An instantiation, for the arm that asks whether an interpreted caller
	 * reaches a generic callee's entry at all. T is in the method's context
	 * and not in its signature, so the same native body detours this one and
	 * Target ().
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int GenericTarget<T> (int x)
	{
		return x + 1;
	}

	public static int CallGenericTarget (int x)
	{
		return GenericTarget<string> (x);
	}

	/* Interpreted callers of two instantiations that one body serves. Each
	 * builds its own receiver, so the entry they reach takes one. */
	public static int CallSharedOverObject (int x)
	{
		return new Shared<object> ().Read (x);
	}

	public static int CallSharedOverException (int x)
	{
		return new Shared<System.Exception> ().Read (x);
	}

	public static int Main ()
	{
		return 0;
	}
}

/*
 * An instance method on a generic class. Every reference instantiation of
 * Read () is served by one body, and that body reads the context it needs out
 * of the receiver - so an instantiation's entry is the shared body itself,
 * with no stub in front of it.
 */
public class Shared<T> where T : class
{
	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Read (int x)
	{
		return x + 1;
	}
}

/*
 * The other half of the crossing: a static method has no receiver, so a shared
 * body reads its context out of a register that the instantiation's own stub
 * writes. Name () answers what that context resolved to, which is what tells a
 * stub that wrote the wrong one from a stub that worked.
 */
public class SharedStatic<T> where T : class
{
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static string Name ()
	{
		return typeof (T).Name;
	}
}
