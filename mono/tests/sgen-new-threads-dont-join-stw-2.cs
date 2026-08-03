
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Threading;

class Driver
{
	public static void Main ()
	{
		BlockingCollection<Thread> threads = new BlockingCollection<Thread> (128);

		Thread producer = new Thread (new ThreadStart (() => {
			DateTime start = DateTime.Now;

			for (TestTimeout timeout = TestTimeout.Start(TimeSpan.FromSeconds(TestTimeout.IsStressTest ? 120 : 5)); timeout.HaveTimeLeft;) {
				Thread worker = new Thread (new ThreadStart (() => {
					HashSet<string> hashset = new HashSet<string> ();
					for (int i = 0; i < 50000; ++i) {
						hashset.Add(string.Concat (i, i));
						if (i % 10 == 0)
							Thread.Yield ();
					}
				}));

				worker.Start ();

				threads.Add (worker);

				Console.WriteLine ("Started thread {0} ({1} running concurrently)", worker.ManagedThreadId, threads.Count);
			}

			threads.CompleteAdding ();
		}));

		// GetConsumingEnumerable rather than "while (!IsCompleted) Take ()": the latter
		// races, because CompleteAdding can land in the window between the check and a
		// Take that is already blocked, and a blocked Take woken that way throws.
		Thread consumer = new Thread (new ThreadStart(() => {
			foreach (Thread worker in threads.GetConsumingEnumerable ()) {
				worker.Join ();
				Console.WriteLine ("Joined thread {0}", worker.ManagedThreadId);
			}
		}));

		producer.Start ();
		consumer.Start ();

		producer.Join ();
		consumer.Join ();
	}
}
