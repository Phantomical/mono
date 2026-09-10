using System;
using System.Runtime.CompilerServices;

/*
 * mono_class_create_runtime_vtable () (object.c) nulls a non-root domain's
 * vtable descriptor for Boehm, since the vtable's mempool does not outlive
 * the domain. emit_object_alloc () (boxing.cpp) picked the TYPED shape from
 * the class's own descriptor, which stays real regardless of domain.
 * GC_GCJ_MALLOC () then trusted that domain's vtable without checking it
 * back, so a TYPED object allocated in a non-root domain read a null
 * descriptor and had none of its fields traced.
 *
 * Holder is an ordinary reference-typed class -- no finalizer, no weak
 * fields, not a MarshalByRefObject -- so its shape answers TYPED.
 * Allocating one in a second AppDomain, with -mono-tier0-filter=0 forcing
 * the allocation through the backend instead of classic tier 0, reaches
 * the shape choice this gates. A WeakReference to the object only
 * Holder.Payload reaches answers whether the collector traced that field.
 * Boehm tracks a WeakReference through its own disappearing link, so the
 * answer does not depend on Holder's own shape.
 */
public class BoehmDomainAllocShape
{
	class Holder
	{
		public object Payload;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Holder MakeHolder ()
	{
		return new Holder ();
	}

	// Spends the stack RunInChildDomain () used setting Payload up, so a
	// leftover word there does not keep it alive by accident.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Churn ()
	{
		for (int i = 0; i < 64; i++)
			GC.KeepAlive (new object[16]);
	}

	static void RunInChildDomain ()
	{
		Holder h = MakeHolder ();
		object payload = new object ();
		h.Payload = payload;

		WeakReference wr = new WeakReference (payload);
		payload = null;

		Churn ();

		for (int i = 0; i < 8; i++)
			GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();

		bool ok = wr.IsAlive && ReferenceEquals (h.Payload, wr.Target);

		Console.WriteLine (ok ? "OK" : "FAIL payload lost");
		Environment.Exit (ok ? 0 : 1);
	}

	public static int Main ()
	{
		AppDomain domain = AppDomain.CreateDomain ("child");
		domain.DoCallBack (RunInChildDomain);
		return 0;
	}
}
