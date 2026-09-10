/*
 * The managed half of test-remset-missing.cpp: a class with one reference
 * field, whose offset the test stores into directly.
 */
public class RemsetHolder
{
	public object Slot;

	/* The assembly is built as an executable, and never run as one. */
	public static int Main ()
	{
		return 0;
	}
}
