using System.Runtime.CompilerServices;

/*
 * The methods test-etw-profiler.cpp reads MethodFlags and the IL map off.
 *
 * Each case that promotes a method through the tiers gets one of its own,
 * since a promotion is never undone - see detour.cs's Target/LateTarget pair
 * for the same reason.
 */
public class Etw
{
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Probe (int x)
	{
		if (x > 0)
			return x + 1;
		return x - 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int ProbeMap (int x)
	{
		if (x > 0)
			return x + 1;
		return x - 1;
	}

	/* Its own instantiation - is_inflated is false on the definition below and
	 * true on the MonoMethod test-etw-profiler.cpp inflates from it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static T Generic<T> (T x)
	{
		return x;
	}

	public class Nested
	{
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Method (int x)
		{
			return x;
		}
	}

	public static int Main ()
	{
		return 0;
	}
}

/* A method on a reference instantiation of a generic class - detour.cs's
 * Shared<T> is the same shape. */
public class Boxed<T>
{
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static T Method (T x)
	{
		return x;
	}
}
