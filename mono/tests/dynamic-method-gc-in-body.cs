using System;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

/*
 * A delegate over a dynamic method is the only thing keeping that method's code
 * alive: the DynamicMethod hangs off the delegate's original_method_info, and once
 * it is collected the reference queue frees the compiled body underneath it. The
 * delegate's Invoke goes through an arch stub that overwrites the receiver with
 * delegate->target before jumping, so the running body does not root the delegate
 * either - the calling activation is the last thing that can, and it has to go on
 * doing so for as long as the body runs.
 *
 * A collection that runs from inside the body is what asks the question, and the
 * answer has to be that the delegate is still there. Reaching that point takes some
 * care: the stack is scanned conservatively, so a test that leaves the delegate
 * anywhere else - a live local, or a word some earlier frame wrote and nothing has
 * overwritten - would pin it and pass without ever posing the question. Hence the
 * first phase, which drops a delegate of exactly the same shape and collects it from
 * the depth the body runs at, to show that one built this way really can go away.
 */
class DynamicMethodGcInBody
{
	/* Enough of a body to be worth compiling, and to have a value worth checking. */
	const int Steps = 200;

	static Func<int> slot;
	static WeakReference minted;
	static int rounds;

	/* Something finalizable, so WaitForPendingFinalizers has a reason to wake the
	 * finalizer thread - which is also what drains the reference queue. */
	class Trigger { ~Trigger () { } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Collect ()
	{
		for (int i = 0; i < 4; i++) {
			new Trigger ();
			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}
	}

	static int Expected ()
	{
		int acc = 0;

		for (int i = 0; i < Steps; i++)
			acc += i;
		return acc;
	}

	/*
	 * Called from inside the dynamic method's own body. The first call is the
	 * one that settles invoke_impl on the arch stub; the second is the one made with
	 * nothing but the caller's frame holding the delegate.
	 */
	public static void Body ()
	{
		if (++rounds == 2)
			Collect ();
	}

	/*
	 * A closed static delegate: the dynamic method takes the target as its first
	 * argument, which is the shape mono_delegate_trampoline serves with the stub that
	 * replaces the receiver.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Mint ()
	{
		DynamicMethod dm = new DynamicMethod ("gc_in_body", typeof (int),
		                                      new Type[] { typeof (object) },
		                                      typeof (DynamicMethodGcInBody).Module, true);
		ILGenerator il = dm.GetILGenerator ();
		LocalBuilder acc = il.DeclareLocal (typeof (int));

		il.Emit (OpCodes.Ldc_I4_0);
		il.Emit (OpCodes.Stloc, acc);
		il.Emit (OpCodes.Call, typeof (DynamicMethodGcInBody).GetMethod ("Body"));
		for (int i = 0; i < Steps; i++) {
			il.Emit (OpCodes.Ldloc, acc);
			il.Emit (OpCodes.Ldc_I4, i);
			il.Emit (OpCodes.Add);
			il.Emit (OpCodes.Stloc, acc);
		}
		il.Emit (OpCodes.Ldloc, acc);
		il.Emit (OpCodes.Ret);

		Func<int> d = (Func<int>) dm.CreateDelegate (typeof (Func<int>), new object ());

		minted = new WeakReference (dm);
		d ();
		slot = d;
	}

	/* The delegate reaches Fire's frame and nowhere else. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static Func<int> Take ()
	{
		Func<int> d = slot;

		slot = null;
		return d;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Fire ()
	{
		return Take () ();
	}

	/*
	 * The control, run at the depth Body reaches so that it sees the same stretch of
	 * stack: nothing is holding the delegate here either, and here it does go away.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Stand ()
	{
		return Settle ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Settle ()
	{
		Collect ();
		return minted.IsAlive;
	}

	public static int Main ()
	{
		/*
		 * The premise: a delegate built this way, with nothing left holding it, does
		 * get collected - and the dynamic method underneath it with it. Without this
		 * the second phase would prove nothing, since a delegate that cannot be
		 * collected survives a call whatever the call site does.
		 */
		Mint ();
		slot = null;
		if (Stand ()) {
			Console.WriteLine ("a dropped delegate over a dynamic method was not collected");
			return 1;
		}

		/* And now the same thing with the body running. */
		rounds = 0;
		Mint ();

		if (Fire () != Expected ()) {
			Console.WriteLine ("the body did not survive the collection it made");
			return 2;
		}

		if (!minted.IsAlive) {
			Console.WriteLine ("the delegate was collected while its own body was running");
			return 3;
		}

		return 0;
	}
}
