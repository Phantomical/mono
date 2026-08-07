// A CCW method returning a non-string array is something the marshaller cannot
// emit. That has to come out as a MarshalDirectiveException on the thread that
// asked for the wrapper, not as a runtime abort.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

[ComVisible (true)]
[Guid ("20000000-0000-0000-0000-000000000001")]
[InterfaceType (ComInterfaceType.InterfaceIsIUnknown)]
public interface IArrayReturner
{
	// PreserveSig keeps the array as the wrapper's return value; without it the
	// return moves to a trailing out parameter and takes a different path.
	[MethodImpl (MethodImplOptions.PreserveSig)]
	int[] GetInts ();
}

[ComVisible (true)]
public class ArrayReturner : IArrayReturner
{
	[MethodImpl (MethodImplOptions.PreserveSig)]
	public int[] GetInts ()
	{
		return new int [] { 1, 2, 3 };
	}
}

public class Tests
{
	public static int Main ()
	{
		try {
			Marshal.GetComInterfaceForObject (new ArrayReturner (), typeof (IArrayReturner));
		} catch (MarshalDirectiveException) {
			return 0;
		} catch (Exception e) {
			Console.Error.WriteLine ("Expected a MarshalDirectiveException, got {0}", e);
			return 2;
		}

		Console.Error.WriteLine ("Building the CCW was expected to fail");
		return 1;
	}
}
