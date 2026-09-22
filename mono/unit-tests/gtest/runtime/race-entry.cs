using System;

/* Managed methods used by the first-entry race tests. */
public class RaceEntry
{
	public static void Callee ()
	{
	}

	public static void Caller ()
	{
		Callee ();
	}

	public static int Main ()
	{
		return 0;
	}
}

/* The class initializer re-enters Callee during its first compile. */
public class ReentrantEntry
{
	static ReentrantEntry ()
	{
		Callee ();
	}

	public static void Callee ()
	{
	}

	public static void Caller ()
	{
		Callee ();
	}
}

/* Keep a method whose record starts unpublished for the lookup test. */
public class LookupFastPath
{
	public static void Target ()
	{
	}
}
