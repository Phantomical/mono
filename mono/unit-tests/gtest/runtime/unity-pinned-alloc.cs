/*
 * The managed half of test-unity-pinned-alloc.cpp. mono_class_setup_parent ()
 * matches UnityEngine.Object by namespace and name, not by image, so this
 * local stand-in sets alloc_pinned the same as the real type would.
 */
namespace UnityEngine
{
	public class Object
	{
	}
}

public class Derived : UnityEngine.Object
{
}

public class GrandChild : Derived
{
}

public class Control
{
}

public class GenericDerived<T> : UnityEngine.Object
{
}

public class ThroughGeneric : GenericDerived<int>
{
}

public class UnityPinnedAllocHost
{
	public static object MakeDerived ()
	{
		return new Derived ();
	}

	public static object MakeControl ()
	{
		return new Control ();
	}

	public static object MakeGenericInstance ()
	{
		return new GenericDerived<int> ();
	}

	/* The assembly is built as an executable, and never run as one. */
	public static int Main ()
	{
		return 0;
	}
}
