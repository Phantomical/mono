// Collections taken while interpreted frames are live.
//
// The interpreter's stack is not the native one, so the collector reaches the
// references in it through the interpreter rather than by scanning registers.
// Each test here holds references in locals, in arguments and in stack
// temporaries across a collection and then reads them back.

using System;
using System.Runtime.CompilerServices;
using System.Threading;

public class GCInteropNode {
	public GCInteropNode Next;
	public int Value;
}

public class GCInterop {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Collect ()
	{
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Deep (int depth, GCInteropNode held)
	{
		if (depth == 0) {
			Collect ();
			return held.Value;
		}

		return Deep (depth - 1, held);
	}

	public static int test_7_reference_in_a_local_survives ()
	{
		GCInteropNode n = new GCInteropNode { Value = 7 };
		Collect ();
		return n.Value;
	}

	// The reference is only in an argument of a frame well below the collection.
	public static int test_9_reference_in_a_deep_frame_survives ()
	{
		return Deep (Id (32), new GCInteropNode { Value = 9 });
	}

	public static int test_5_reference_in_an_array_survives ()
	{
		GCInteropNode [] a = new GCInteropNode [4];
		a [2] = new GCInteropNode { Value = 5 };
		Collect ();
		return a [2].Value;
	}

	public static int test_10_chain_survives ()
	{
		GCInteropNode head = null;
		for (int i = 0; i < 10; i++)
			head = new GCInteropNode { Next = head, Value = i };

		Collect ();

		int count = 0;
		for (GCInteropNode n = head; n != null; n = n.Next)
			count++;
		return count;
	}

	public static int test_3_boxed_value_survives ()
	{
		object boxed = Id (3);
		Collect ();
		return (int) boxed;
	}

	public static int test_1_weak_reference_clears ()
	{
		WeakReference w = Allocate ();
		Collect ();
		return w.IsAlive ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static WeakReference Allocate ()
	{
		return new WeakReference (new GCInteropNode { Value = 1 });
	}

	public static int test_4_string_built_across_a_collection ()
	{
		string s = "ab";
		Collect ();
		s += "cd";
		Collect ();
		return s.Length;
	}

	// One other thread, joined at once: the collector has two interpreted stacks
	// to walk rather than one.
	public static int test_6_second_thread_stack_is_walked ()
	{
		int result = 0;
		Thread t = new Thread (() => {
			GCInteropNode n = new GCInteropNode { Value = 6 };
			Collect ();
			result = n.Value;
		});
		t.Start ();
		t.Join ();
		return result;
	}

	public static int test_2_finalizer_runs ()
	{
		GCInteropFinalized.Count = 0;
		MakeFinalized ();
		Collect ();
		return GCInteropFinalized.Count == 0 ? 0 : 2;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void MakeFinalized ()
	{
		GCInteropFinalized unused = new GCInteropFinalized ();
	}
}

public class GCInteropFinalized {
	public static int Count;
	~GCInteropFinalized () { Count++; }
}
