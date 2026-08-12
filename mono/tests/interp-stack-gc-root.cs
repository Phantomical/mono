using System;
using System.Runtime.CompilerServices;

/*
 * A local of a managed method is a root for as long as its frame stands. Under the
 * interpreter that local is not on the native stack at all - it is a slot in the
 * interpreter's own value stack, a separate mapping - so a collector that only knows
 * about native stacks cannot see it, and an object whose last reference is such a slot
 * is collected while the frame is still using it.
 *
 * Each phase pairs the object it is asking about with a second one of the same shape,
 * allocated the same way at the same depth and then dropped, and requires that one to
 * be gone. Both stacks are scanned conservatively, so a phase that passed only because
 * some leftover word still pointed at the object would prove nothing; the dropped one
 * failing to die says the run is too pinned to be answering the question.
 */
class InterpStackGcRoot
{
	const int Magic = 0x5150;

	class Payload
	{
		public int magic = Magic;
		public Payload chain;
	}

	class Trigger { ~Trigger () { } }

	/* Finalizable garbage each round, so WaitForPendingFinalizers has work to wake for. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Collect ()
	{
		for (int i = 0; i < 4; i++) {
			new Trigger ();
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}
	}

	/*
	 * Walk down and back through a chain of frames that touch nothing but their own
	 * allocations, so that whatever the callee stack and the machine registers were
	 * holding when the payload was minted is overwritten before anything is counted.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Scrub (int depth)
	{
		Payload a = new Payload (), b = new Payload ();

		a.chain = b;
		b.magic = depth;
		if (depth > 0)
			b.magic += Scrub (depth - 1);
		return a.chain.magic & 0xff;
	}

	static Payload staged;
	static WeakReference watched, dropped;

	/* The payload leaves through the return value and the static is cleared, so the
	 * caller's slot is the only reference to it that the managed program has. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static Payload Take ()
	{
		Payload p = staged;

		staged = null;
		return p;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Stage ()
	{
		staged = new Payload ();
		watched = new WeakReference (staged);
	}

	struct W8 { public long a, b, c, d, e, f, g, h; }
	struct Block { public W8 a, b, c, d, e, f, g, h; }

	/*
	 * Zero the stack that the WeakReference constructor and its callees left. After
	 * Drop () returns, those frames still hold words that name the dropped payload.
	 * Both stacks get a conservative scan, so one such word keeps the payload alive,
	 * and the phase then reports a run that it cannot measure. The locals below are
	 * 1024 words, which is more than that subtree uses.
	 *
	 * Scrub () cannot do this. Scrub () gets enough calls to reach tier 1, and a
	 * compiled Scrub () writes the native stack, but the words to clear are on the
	 * interpreter stack. Wipe () gets one call for each Drop () call, so the two
	 * methods stay in the same engine and write the same stack.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Wipe ()
	{
		Block b0 = default, b1 = default, b2 = default, b3 = default;
		Block b4 = default, b5 = default, b6 = default, b7 = default;
		Block b8 = default, b9 = default, ba = default, bb = default;
		Block bc = default, bd = default, be = default, bf = default;

		return b0.a.a + b1.b.b + b2.c.c + b3.d.d + b4.e.e + b5.f.f + b6.g.g + b7.h.h
		     + b8.a.b + b9.b.c + ba.c.d + bb.d.e + bc.e.f + bd.f.g + be.g.h + bf.h.a;
	}

	/* Mint one of the same shape in a frame that is popped before anything collects. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Drop ()
	{
		Payload p = new Payload ();

		dropped = new WeakReference (p);
		p = null;
		Wipe ();
	}

	static int Check (Payload live)
	{
		if (dropped.IsAlive)
			return 1;
		if (!watched.IsAlive)
			return 2;
		if (live.magic != Magic)
			return 3;
		return 0;
	}

	/* The payload's only reference is this frame's local, and the frame that collects
	 * is its immediate callee. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Shallow ()
	{
		Stage ();

		Payload live = Take ();

		Drop ();
		Scrub (24);
		Collect ();

		int code = Check (live);

		GC.KeepAlive (live);
		return code;
	}

	/* Collects with the holding frame far enough down the interpreter stack that a scan
	 * covering only the frames near the top would miss it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Descend (int depth)
	{
		if (depth > 0) {
			Descend (depth - 1);
			return;
		}

		Drop ();
		Scrub (24);
		Collect ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Deep ()
	{
		Stage ();

		Payload live = Take ();

		Descend (32);

		int code = Check (live);

		GC.KeepAlive (live);
		return code == 0 ? 0 : code + 3;
	}

	static string Explain (int code)
	{
		switch (code) {
		case 1: return "a dropped payload was not collected - the run is too pinned to measure";
		case 2: return "a payload held only by a live frame's local was collected";
		case 3: return "a payload held only by a live frame's local was overwritten";
		case 4: return "a dropped payload was not collected from a deep frame";
		case 5: return "a payload held by a deep live frame's local was collected";
		case 6: return "a payload held by a deep live frame's local was overwritten";
		}
		return "unknown";
	}

	public static int Main ()
	{
		int code = Shallow ();

		if (code == 0)
			code = Deep ();
		if (code != 0)
			Console.WriteLine (Explain (code));
		return code;
	}
}
