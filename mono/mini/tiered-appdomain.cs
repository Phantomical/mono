// Regression test for task #28: the background tier-1 compile worker racing
// main-thread Reflection.Emit and AppDomain create/unload.
//
// Two distinct corruptions this exercises, both of which crashed the runtime
// before the fix (SIGSEGV / SIGABRT / a garbage AssemblyBuilderAccess value):
//
//   1. The worker compiling a Reflection.Emit body (a DynamicMethod, an emitted
//      type's method, or the runtime-invoke wrapper MethodInfo.Invoke builds for
//      one) off-thread, concurrently with the main thread still emitting into and
//      moving that dynamic metadata. EmitOne () below drives this in every domain,
//      including the root domain.
//
//   2. The worker promoting a method in a transient child app-domain: its
//      per-domain tier-1 code and trampolines are freed when the domain unloads,
//      but the process-global direct-call symbol cache keeps handing their
//      dangling addresses to later callers. thread_start () creates and unloads
//      child domains under promotion pressure to drive this.
//
// The fix keeps both classes of method at tier 0. This test just has to run to
// completion without crashing under MONO_TIERED with promotions firing on the
// first call (MONO_TIERED_CALL_THRESHOLD=1); it exits non-zero on any failure so
// the harness fails on either a managed error or a native crash.

using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Formatters.Binary;
using System.Threading;

[Serializable] public class L0 { public int a; }
[Serializable] public class L1 : L0 { public int b; }
[Serializable] public class L2 : L1 { public int c; }
[Serializable] public class L3 : L2 { public int d; }

public class TieredAppDomain {
	static int threads = 4;
	static int domains = 10;
	static int churn = 5;
	public static int errors = 0;

	static int seq;

	// Reflection.Emit an assembly + type + method, bake it, invoke it (which
	// creates and drives a runtime-invoke wrapper), then do the same with a
	// DynamicMethod. All of this is dynamic metadata the worker must not compile.
	public static void EmitOne ()
	{
		int i = Interlocked.Increment (ref seq);
		var ab = AppDomain.CurrentDomain.DefineDynamicAssembly (new AssemblyName ("Dyn" + i), AssemblyBuilderAccess.Run);
		var mb = ab.DefineDynamicModule ("m");
		var tb = mb.DefineType ("T" + i, TypeAttributes.Public | TypeAttributes.Serializable, typeof (L2));
		tb.DefineField ("f", typeof (int), FieldAttributes.Public);
		var meth = tb.DefineMethod ("Go", MethodAttributes.Public | MethodAttributes.Static, typeof (Type), Type.EmptyTypes);
		var il = meth.GetILGenerator ();
		il.Emit (OpCodes.Ldtoken, typeof (object));
		il.Emit (OpCodes.Call, typeof (Type).GetMethod ("GetTypeFromHandle"));
		il.Emit (OpCodes.Ret);
		var t = tb.CreateType ();
		t.GetMethod ("Go").Invoke (null, null);
		FormatterServices.GetSerializableMembers (t);
		FormatterServices.GetSerializableMembers (t.BaseType);

		var dm = new DynamicMethod ("D" + i, typeof (Type), Type.EmptyTypes);
		var dil = dm.GetILGenerator ();
		dil.Emit (OpCodes.Ldtoken, typeof (object));
		dil.Emit (OpCodes.Call, typeof (Type).GetMethod ("GetTypeFromHandle"));
		dil.Emit (OpCodes.Ret);
		dm.Invoke (null, null);
	}

	public static void Serialize ()
	{
		var bf = new BinaryFormatter ();
		using (var ms = new MemoryStream ()) {
			bf.Serialize (ms, new L3 { a = 1, b = 2, c = 3, d = 4 });
			ms.Position = 0;
			var o = (L3) bf.Deserialize (ms);
			if (o.d != 4)
				throw new Exception ("bad roundtrip");
		}
	}

	public static void worker ()
	{
		ArrayList list = new ArrayList ();
		for (int i = 0; i < churn; ++i) {
			EmitOne ();
			Serialize ();
			// Allocation pressure so the GC runs while the worker compiles.
			for (int k = 0; k < 200; k++) { list.Add (new object ()); list.Add (new String ('x', 34)); }
			if ((i % 3) == 0) list.Clear ();
		}
	}

	static void thread_start ()
	{
		for (int i = 0; i < domains; ++i) {
			AppDomain ad = AppDomain.CreateDomain ("Test-" + Thread.CurrentThread.ManagedThreadId + "-" + i);
			try {
				ad.DoCallBack (new CrossAppDomainDelegate (worker));
			} catch (Exception e) {
				Interlocked.Increment (ref errors);
				Console.WriteLine ("callback error: " + e);
			}
			try {
				AppDomain.Unload (ad);
			} catch (Exception) {
				Interlocked.Increment (ref errors);
			}
			// Keep the root domain busy emitting and promoting too.
			EmitOne ();
			Serialize ();
		}
	}

	static int Main (string[] args)
	{
		if (args.Length > 0) threads = int.Parse (args [0]);
		if (args.Length > 1) domains = int.Parse (args [1]);
		if (args.Length > 2) churn = int.Parse (args [2]);

		Thread[] ta = new Thread [threads];
		for (int i = 0; i < threads; ++i) { ta [i] = new Thread (new ThreadStart (thread_start)); ta [i].Start (); }
		for (int i = 0; i < threads; ++i) ta [i].Join ();

		if (errors != 0) {
			Console.WriteLine ("FAILED errors=" + errors);
			return 1;
		}
		Console.WriteLine ("PASS");
		return 0;
	}
}
