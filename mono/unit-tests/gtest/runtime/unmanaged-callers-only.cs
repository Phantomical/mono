using System;
using System.Runtime.CompilerServices;

/*
 * The attribute this tree's corlib does not declare. mono matches it by
 * namespace and name rather than by identity (mono_method_has_unmanaged_-
 * callers_only_attribute), so the one an assembly declares for itself counts.
 */
namespace System.Runtime.InteropServices
{
	/*
	 * No CallConvs field, deliberately. The runtime looks that field up by name
	 * to settle the calling convention, and a class declared without it is what
	 * a program that hand-rolls this attribute gives it. Adding the field here
	 * would leave the field-less shape uncovered.
	 */
	[AttributeUsage (AttributeTargets.Method)]
	public sealed class UnmanagedCallersOnlyAttribute : Attribute
	{
	}
}

/*
 * The methods test-unmanaged-callers-only.cpp calls from C, and the managed
 * callers that have to be refused.
 */
public class UnmanagedCallers
{
	[System.Runtime.InteropServices.UnmanagedCallersOnly]
	public static int Add (int x, int y)
	{
		return x + y;
	}

	/*
	 * A second one, so a case can ask whether two methods get two addresses.
	 * It answers a different sum, so a caller of the wrong one is visible in
	 * the value rather than only in the address.
	 */
	[System.Runtime.InteropServices.UnmanagedCallersOnly]
	public static int Subtract (int x, int y)
	{
		return x - y;
	}

	/*
	 * NoInlining keeps the call site in this method. A body the interpreter or
	 * the trivial inliner copied into its caller is refused where it lands
	 * instead, which is a different site from the one these cases name.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int CallAdd (int x, int y)
	{
		return Add (x, y);
	}

	public delegate int Binary (int x, int y);

	/* ldftn Add, then newobj Binary::.ctor: the pair the transform reads. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Binary MakeDelegate ()
	{
		return new Binary (Add);
	}

	/* The same call written against a method with no attribute, so a case can
	 * tell a refusal of Add () apart from a refusal of every static call. */
	public static int Plain (int x, int y)
	{
		return x + y;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int CallPlain (int x, int y)
	{
		return Plain (x, y);
	}

	/* The assembly is built as an .exe, and the cases load it rather than run it. */
	public static int Main ()
	{
		return 0;
	}
}
