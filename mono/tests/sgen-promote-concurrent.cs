using System;

/* Verifies that references in objects promoted during concurrent marking stay alive. */

class Node {
	public Node next, other;
	public long value;
}

class Program {
	static Node Tree (int depth)
	{
		if (depth == 0)
			return new Node { value = 1 };
		return new Node { next = Tree (depth - 1), other = Tree (depth - 1), value = 1 };
	}

	static long Sum (Node n)
	{
		long sum = 0;
		for (; n != null; n = n.next) {
			sum += n.value;
			if (n.other != null)
				sum += n.other.value;
		}
		return sum;
	}

	static int Main ()
	{
		var rng = new Random (1);

		// Enough live data that a concurrent mark spans many nursery collections.
		var forest = new Node [32];
		for (int i = 0; i < forest.Length; i++)
			forest [i] = Tree (14);

		var slots = new Node [1 << 15];
		var expected = new long [slots.Length];
		for (long iter = 0; iter < 20000000; iter++) {
			var old = forest [rng.Next (forest.Length)];
			var head = new Node { value = 1, other = old.next };
			head.next = new Node { value = 1, next = new Node { value = 1, other = old.other }, other = head };

			int slot = rng.Next (slots.Length);
			slots [slot] = head;
			expected [slot] = Sum (head);

			// Old-generation garbage, so major collections keep starting.
			if ((iter & 0xfff) == 0)
				forest [rng.Next (forest.Length)] = Tree (11);
		}

		for (int i = 0; i < slots.Length; i++) {
			if (slots [i] != null && Sum (slots [i]) != expected [i]) {
				Console.WriteLine ("slot {0}: read {1}, expected {2}", i, Sum (slots [i]), expected [i]);
				return 1;
			}
		}
		return 0;
	}
}
