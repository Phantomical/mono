using System;

/*
 * The managed half of test-type-init-recursion.cpp. Its only job is a cctor
 * that fails, so mono_runtime_class_init_full () has a real
 * TypeInitializationException to build; the test reenters this class's own
 * class-init check from inside that construction itself, through
 * mono_test_hook_type_init_exception_ctor ().
 */
public class TypeInitRecursion
{
	static TypeInitRecursion ()
	{
		throw new InvalidOperationException ("deliberate failure for test-type-init-recursion.cpp");
	}

	/* The assembly is built as an executable, and never run as one. */
	public static int Main ()
	{
		return 0;
	}
}
