// The debugger-facing surface that works with no debugger attached.
//
// System.Diagnostics.Debugger.Break () becomes MINT_BREAK, which asks the
// debugger callbacks for a user break. Without an agent that is a no-op, and
// what these tests say is that the opcode returns rather than stopping.

using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

public class DebugOps {

	[MethodImpl (MethodImplOptions.NoInlining)] static int DbgId (int x) { return x; }

	public static int test_5_break_returns ()
	{
		int before = DbgId (5);
		Debugger.Break ();
		return before;
	}

	public static int test_3_break_inside_a_loop ()
	{
		int seen = 0;
		for (int i = 0; i < 3; i++) {
			Debugger.Break ();
			seen++;
		}
		return seen;
	}

	public static int test_7_break_inside_a_try ()
	{
		try {
			Debugger.Break ();
			return DbgId (7);
		} finally {
			Debugger.Break ();
		}
	}

	public static int test_0_is_attached ()
	{
		return Debugger.IsAttached ? 1 : 0;
	}

	public static int test_0_is_logging ()
	{
		return Debugger.IsLogging () ? 1 : 0;
	}

	public static int test_4_log_returns ()
	{
		Debugger.Log (0, "interp", "");
		return DbgId (4);
	}

	// A break between two calls, so the opcode sits in the middle of a block
	// rather than at its start.
	public static int test_9_break_between_calls ()
	{
		int a = DbgId (4);
		Debugger.Break ();
		int b = DbgId (5);
		return a + b;
	}

	public static int test_1_notify_of_cross_thread_dependency ()
	{
		Debugger.NotifyOfCrossThreadDependency ();
		return 1;
	}

	// The stack trace a frame reports, which is built from the same line table
	// the sequence points come from.
	public static int test_1_stack_trace_names_this_method ()
	{
		StackTrace trace = new StackTrace ();
		StackFrame frame = trace.GetFrame (0);
		return frame != null &&
		       frame.GetMethod ().Name == "test_1_stack_trace_names_this_method" ? 1 : 0;
	}

	public static int test_1_stack_trace_reaches_the_caller ()
	{
		return Depth2 ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Depth2 ()
	{
		StackTrace trace = new StackTrace ();
		for (int i = 0; i < trace.FrameCount; i++)
			if (trace.GetFrame (i).GetMethod ().Name == "test_1_stack_trace_reaches_the_caller")
				return 1;
		return 0;
	}

	public static int test_1_exception_carries_a_stack_trace ()
	{
		try {
			Thrower ();
			return 0;
		} catch (InvalidOperationException e) {
			return e.StackTrace != null && e.StackTrace.Contains ("Thrower") ? 1 : 0;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Thrower () { throw new InvalidOperationException (); }
}
