// The opcodes the interpreter has for atomics, barriers and thread control.
//
// Every test here joins whatever it starts, so none of them depends on timing.

using System;
using System.Runtime.CompilerServices;
using System.Threading;

[Instrumented]
public class Threading {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }

	static int shared;
	static long sharedLong;
	static object sharedReference;
	static volatile int flag;

	public static int test_9_interlocked_exchange_i4 ()
	{
		shared = 4;
		int old = Interlocked.Exchange (ref shared, Id (9));
		return old == 4 ? shared : 0;
	}

	public static int test_9_interlocked_exchange_i8 ()
	{
		sharedLong = 4;
		long old = Interlocked.Exchange (ref sharedLong, 0x900000000L);
		return old == 4L && sharedLong == 0x900000000L ? 9 : 0;
	}

	public static int test_7_interlocked_compare_exchange ()
	{
		shared = 3;
		int seen = Interlocked.CompareExchange (ref shared, 7, 3);
		int missed = Interlocked.CompareExchange (ref shared, 8, 3);
		return seen == 3 && missed == 7 ? shared : 0;
	}

	public static int test_5_interlocked_increment_and_add ()
	{
		shared = 0;
		Interlocked.Increment (ref shared);
		Interlocked.Add (ref shared, Id (5));
		Interlocked.Decrement (ref shared);
		return shared;
	}

	public static int test_1_interlocked_exchange_reference ()
	{
		sharedReference = "old";
		object old = Interlocked.Exchange (ref sharedReference, "new");
		return (string) old == "old" && (string) sharedReference == "new" ? 1 : 0;
	}

	public static int test_3_memory_barrier ()
	{
		shared = 0;
		Thread.MemoryBarrier ();
		shared = Id (3);
		Thread.MemoryBarrier ();
		return shared;
	}

	public static int test_6_volatile_field ()
	{
		flag = 0;
		flag = Id (6);
		return flag;
	}

	public static int test_8_volatile_read_write ()
	{
		Volatile.Write (ref shared, Id (8));
		return Volatile.Read (ref shared);
	}

	public static int test_4_monitor_enter_and_exit ()
	{
		object gate = new object ();
		int result;
		lock (gate) {
			result = Id (4);
		}
		return result;
	}

	// The lock is released by the finally the lock statement emits, so the
	// exception has to pass through it.
	public static int test_1_monitor_released_on_exception ()
	{
		object gate = new object ();
		try {
			lock (gate) {
				throw new InvalidOperationException ();
			}
		} catch (InvalidOperationException) {
		}
		return Monitor.TryEnter (gate) ? 1 : 0;
	}

	public static int test_10_worker_thread_computes ()
	{
		int result = 0;
		Thread t = new Thread (() => result = Id (5) + Id (5));
		t.Start ();
		t.Join ();
		return result;
	}

	public static int test_135_several_threads_accumulate ()
	{
		int total = 0;
		Thread [] workers = new Thread [3];
		for (int i = 0; i < workers.Length; i++) {
			int mine = i;
			workers [i] = new Thread (() => {
				for (int j = 0; j <= 5; j++)
					Interlocked.Add (ref total, mine * 5 + j);
			});
		}
		foreach (Thread t in workers)
			t.Start ();
		foreach (Thread t in workers)
			t.Join ();
		return total;
	}

	// An interrupt is delivered at the wait, which is one of the points the
	// interpreter checks for a pending request.
	public static int test_1_thread_interrupt ()
	{
		int caught = 0;
		ManualResetEvent ready = new ManualResetEvent (false);
		Thread t = new Thread (() => {
			try {
				ready.Set ();
				Thread.Sleep (30000);
			} catch (ThreadInterruptedException) {
				caught = 1;
			}
		});
		t.Start ();
		ready.WaitOne ();
		t.Interrupt ();
		t.Join ();
		return caught;
	}

	public static int test_2_thread_abort ()
	{
		int caught = 0;
		ManualResetEvent ready = new ManualResetEvent (false);
		Thread t = new Thread (() => {
			try {
				ready.Set ();
				Thread.Sleep (30000);
			} catch (ThreadAbortException) {
				caught = 2;
				Thread.ResetAbort ();
			}
		});
		t.Start ();
		ready.WaitOne ();
		t.Abort ();
		t.Join ();
		return caught;
	}

	static volatile bool started;
	static volatile bool stop;
	static long sink;

	// An abort delivered to a thread that is running IL rather than waiting.
	// The interpreter takes it at one of the checkpoints it polls between
	// instructions, which is a different path from the one a wait uses.
	public static int test_2_abort_in_an_interpreted_loop ()
	{
		int caught = 0;
		Thread t = new Thread (() => {
			try {
				started = true;
				long acc = 0;
				for (long i = 0; i < 20000000 && !stop; i++)
					acc += i;
				sink = acc;
			} catch (ThreadAbortException) {
				caught = 2;
				Thread.ResetAbort ();
			}
		});
		t.Start ();
		while (!started)
			Thread.Sleep (1);
		t.Abort ();
		// The loop reads `stop` so that a request that never lands ends the test
		// instead of running it to the bound.
		if (!t.Join (1000)) {
			stop = true;
			t.Join ();
		}
		return caught;
	}

	public static int test_1_thread_local_storage ()
	{
		ThreadLocal<int> local = new ThreadLocal<int> (() => 11);
		int other = 0;
		Thread t = new Thread (() => other = local.Value);
		local.Value = 5;
		t.Start ();
		t.Join ();
		return local.Value == 5 && other == 11 ? 1 : 0;
	}
}
