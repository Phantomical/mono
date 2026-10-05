// Use C#: ilasm drops the `pinned` modifier from local signatures.

public static unsafe class Pinned
{
	public static int Escape (byte* p)
	{
		return 0;
	}

	public static void Store (byte[] a)
	{
		fixed (byte* p = a)
			Escape (p);
	}

	public static int Return (byte[] a)
	{
		fixed (byte* p = a)
			return Escape (p);
	}
}
