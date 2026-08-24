using System;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/*
 * A frame executing a method's code has to keep that code's allocation alive for as
 * long as it stands. For a dynamic method the allocation is owned by the DynamicMethod
 * the body was emitted from - the reference queue frees the compiled body once that
 * object dies - so the question is whether the object survives a collection made from
 * inside its own body when nothing else refers to it.
 *
 * dynamic-method-gc-in-body.cs poses that for one plain delegate call. This poses it
 * for the shapes where a per-frame answer differs from a per-thread one: two different
 * dynamic methods on the stack at once, where a single "what is running" slot would
 * lose the outer one, and several frames of the same one, where the innermost returning
 * must not release what the frames above it are still running.
 *
 * Each phase has a control that drops a delegate of the same shape at the same stack
 * depth and shows it really does get collected, because the stack is scanned
 * conservatively and a phase that passed only because some earlier frame's leftover
 * word still pointed at the delegate would prove nothing.
 *
 * A control that fails says such a word exists. The failure occurs on about one run in
 * a hundred on a loaded box. MONO_TEST_GC_TRAP stops that run on SIGTRAP at the point
 * the control reads its answer, which is where a search for the word starts. Trap ()
 * describes what the stop keeps.
 */
class DynamicMethodGcInFrame
{
	class Trigger { ~Trigger () { } }

	[DllImport ("libc")]
	static extern int raise (int sig);

	[DllImport ("libc")]
	static extern unsafe IntPtr write (int fd, byte *buf, IntPtr count);

	const int SIGTRAP = 5;

	/* Trap () makes as few calls as it can, so it reads this rather than the environment. */
	static readonly bool trap_armed =
		Environment.GetEnvironmentVariable ("MONO_TEST_GC_TRAP") != null;
	static bool trapped;

	/*
	 * Gives the address a reference points at. A search of a core matches on this value.
	 *
	 * MonoTypedRef is { MonoType *type; gpointer value; MonoClass *klass; }, so value is
	 * the second word. That word addresses the local variable the argument arrived in.
	 */
	static unsafe IntPtr AddressOf (object o)
	{
		TypedReference tr = __makeref (o);

		return *(IntPtr *) ((IntPtr *) &tr) [1];
	}

	/*
	 * Writes the address through write (2). Console allocates and runs deep, and it
	 * writes over the frames that Trap () keeps.
	 */
	static unsafe void ReportAddress (IntPtr addr)
	{
		byte *buf = stackalloc byte [64];
		ulong v = (ulong) addr.ToInt64 ();
		int n = 0, shift = 60;

		foreach (char c in "pinned object at 0x")
			buf [n++] = (byte) c;

		while (shift > 0 && ((v >> shift) & 0xf) == 0)
			shift -= 4;
		for (; shift >= 0; shift -= 4)
			buf [n++] = (byte) "0123456789abcdef" [(int) ((v >> shift) & 0xf)];

		buf [n++] = (byte) '\n';
		write (2, buf, (IntPtr) n);
	}

	/*
	 * Stops the run where a failure occurs. A debugger attached to the run breaks here,
	 * and a run with no debugger writes a core. mono installs no SIGTRAP handler, so
	 * neither the break nor the core goes through the crash reporter.
	 *
	 * A search of the core for the reported address must cover every mapped segment
	 * rather than the standing frames. A collection scans from its own frame to the base
	 * of the stack. The word that kept the object is therefore above where that scan
	 * started, and below the stack pointer as it is now. It sits in a frame which has
	 * since returned, and one caught run had it 832 bytes down. Any call made from here
	 * writes over that word, so write (2) reports the address and the stop follows at
	 * once.
	 *
	 * seen names the object the caller expected to be gone. A null seen says the failure
	 * is the opposite one: an object went away. That case has no address to report, so
	 * the stop keeps the stack alone.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Trap (WeakReference seen)
	{
		if (!trap_armed || trapped)
			return;

		trapped = true;

		object survivor = seen == null ? null : seen.Target;

		if (survivor != null)
			ReportAddress (AddressOf (survivor));
		raise (SIGTRAP);
	}

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
	 * A dynamic method that calls BODY and returns MARK, so that a caller can tell the
	 * code really ran to the end rather than being replaced by something that returns
	 * a plausible number.
	 */
	static DynamicMethod Emit (string name, string body, int mark)
	{
		DynamicMethod dm = new DynamicMethod (name, typeof (int),
		                                      new Type[] { typeof (object) },
		                                      typeof (DynamicMethodGcInFrame).Module, true);
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Call, typeof (DynamicMethodGcInFrame).GetMethod (body));
		il.Emit (OpCodes.Ldc_I4, mark);
		il.Emit (OpCodes.Ret);
		return dm;
	}

	/* A closed static delegate, which is the shape whose stub replaces the receiver. */
	static Func<int> Bind (DynamicMethod dm)
	{
		return (Func<int>) dm.CreateDelegate (typeof (Func<int>), new object ());
	}

	static Func<int> outer_slot, inner_slot;
	static WeakReference outer_seen, inner_seen;

	/* The delegate reaches the taking frame and nowhere else. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static Func<int> TakeOuter ()
	{
		Func<int> d = outer_slot;

		outer_slot = null;
		return d;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Func<int> TakeInner ()
	{
		Func<int> d = inner_slot;

		inner_slot = null;
		return d;
	}

	/*
	 * Called from the outer dynamic method's body: enters the inner one, whose body
	 * collects. Both bodies are then on the stack with nothing but their own frames
	 * left to keep them there.
	 */
	public static void CallInner ()
	{
		if (TakeInner () () != 4242)
			throw new Exception ("the inner body did not run to its end");
	}

	public static void CollectHere ()
	{
		Collect ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FireNested ()
	{
		return TakeOuter () ();
	}

	/* Run at the depth the bodies reach, so the same stretch of stack is in view. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool StandNested ()
	{
		Collect ();
		if (!outer_seen.IsAlive && !inner_seen.IsAlive)
			return false;

		Trap (outer_seen.IsAlive ? outer_seen : inner_seen);
		return true;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void MintNested ()
	{
		DynamicMethod outer = Emit ("gc_in_frame_outer", "CallInner", 2121);
		DynamicMethod inner = Emit ("gc_in_frame_inner", "CollectHere", 4242);
		Func<int> od = Bind (outer), id = Bind (inner);

		outer_seen = new WeakReference (outer);
		inner_seen = new WeakReference (inner);

		/* Settle both invoke_impl fields before the delegates become uprooted. */
		inner_slot = id;
		od ();

		inner_slot = id;
		outer_slot = od;
	}

	static int Nested ()
	{
		MintNested ();
		outer_slot = inner_slot = null;
		if (StandNested ())
			return 1;

		MintNested ();
		if (FireNested () != 2121)
			return 2;
		if (!outer_seen.IsAlive)
			return 3;
		if (!inner_seen.IsAlive)
			return 4;
		return 0;
	}

	static Func<int>[] depth_slots = new Func<int>[3];
	static int depth_top;
	static Func<int>[] minted;
	static WeakReference watched;
	static bool watched_alive;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Func<int> TakeDepth ()
	{
		if (depth_top == 0)
			return null;

		Func<int> d = depth_slots [--depth_top];

		depth_slots [depth_top] = null;
		return d;
	}

	/*
	 * Called from the deep body: re-enters it through the next delegate down, or, at the
	 * bottom, collects and reads WATCHED. Every frame between here and the top is running
	 * the same code and each of them has to be holding it.
	 */
	public static void Descend ()
	{
		Func<int> d = TakeDepth ();

		if (d == null) {
			Collect ();
			watched_alive = watched != null && watched.IsAlive;
			if (watched_alive)
				Trap (watched);
			return;
		}
		if (d () != 5150)
			throw new Exception ("a recursive body did not run to its end");
	}

	/*
	 * Mint a dynamic method, settle every delegate over it, and load the descent. The
	 * spare copies live in a static rather than a local while the warm-up runs, so that
	 * dropping them afterwards is the test's own doing and not a scan's.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static WeakReference MintDeep ()
	{
		DynamicMethod dm = Emit ("gc_in_frame_deep", "Descend", 5150);

		minted = new Func<int> [depth_slots.Length];
		for (int i = 0; i < minted.Length; i++)
			minted [i] = Bind (dm);

		watched = null;
		Array.Copy (minted, depth_slots, minted.Length);
		depth_top = minted.Length;
		depth_slots [0] ();

		for (int i = 0; i < minted.Length; i++)
			depth_slots [i] = minted [i];
		depth_top = minted.Length;
		minted = null;
		return new WeakReference (dm);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FireDeep ()
	{
		return TakeDepth () ();
	}

	/*
	 * One descent answers both halves. The dynamic method the descent is running has to
	 * come back alive; an identically shaped one, minted and dropped just before it and
	 * so occupying the same stretch of stack, has to be gone by the time the bottom is
	 * reached - otherwise a live frame is not what the first result is measuring.
	 */
	static int Deep ()
	{
		WeakReference dropped = MintDeep ();

		Array.Clear (depth_slots, 0, depth_slots.Length);
		depth_top = 0;

		WeakReference running = MintDeep ();

		watched = dropped;
		if (FireDeep () != 5150)
			return 6;
		if (watched_alive)
			return 5;
		if (!running.IsAlive)
			return 7;
		return 0;
	}

	static string Explain (int code)
	{
		switch (code) {
		case 1: return "a dropped delegate over a nested dynamic method was not collected";
		case 2: return "the outer body did not survive the collection its callee made";
		case 3: return "the outer dynamic method was collected while its own body was running";
		case 4: return "the inner dynamic method was collected while its own body was running";
		case 5: return "dropped delegates over a recursive dynamic method were not collected";
		case 6: return "a recursive body did not survive the collection it made";
		case 7: return "a dynamic method was collected while several frames were running it";
		}
		return "unknown";
	}

	public static int Main ()
	{
		int code = Nested ();

		if (code == 0)
			code = Deep ();
		if (code != 0) {
			Console.WriteLine (Explain (code));

			/* The controls stop where their evidence is. The other codes arrive here. */
			Trap (null);
		}
		return code;
	}
}
