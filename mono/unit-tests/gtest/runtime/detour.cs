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

	public static int Main ()
	{
		return 0;
	}
}
