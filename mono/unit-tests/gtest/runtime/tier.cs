using System.Runtime.CompilerServices;

/*
 * The methods test-tier.cpp promotes and reads MonoJitInfo::tier off.
 * Neither is called from managed code, so no inliner folds either one in
 * and no test detours either one.
 */
public class Tier
{
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Probe (int x)
	{
		return x + 1;
	}

	/* The filter clause gives the compile a body of its own that the record
	 * never sees - see method-to-llvm.cpp's $filter functions. The throw
	 * stays reachable so the compiler cannot prove the clause dead and drop
	 * it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int WithFilter (int x)
	{
		try {
			if (x < 0)
				throw new System.Exception ();
			return x;
		} catch (System.Exception) when (x != 0) {
			return -1;
		}
	}

	public static int Main ()
	{
		return 0;
	}
}
