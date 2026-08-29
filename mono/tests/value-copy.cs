using System;
using System.Runtime.CompilerServices;

/*
 * What a copy of a value type that holds references must not change.
 *
 * The translator writes such a copy as `mono.gc.wbarrier.value.copy`, which
 * lowers to the collector's own copy-and-mark icall. A fold replaces that call
 * with a bare memcpy where the IR settles the destination to a frame slot, which
 * owes no card. No C# producer reaches the fold yet, so every copy below runs
 * through the icall.
 *
 * Two things have to hold, and the steps below split them.
 *
 * The copy itself: every field arrives, and a reference field arrives as the
 * object the source named. Each arm reads all three fields back and counts the
 * rounds that agree.
 *
 * The cards: a minor collection reaches a destination outside the nursery only
 * through its card. `elements` is what reaches that case. It is larger than
 * SGEN_MAX_SMALL_OBJ_SIZE, so SGen puts it in the large object space, where it
 * is old from birth. A smaller holder does not work. This backend scans a frame
 * conservatively, so an array a local names stays pinned in the nursery for as
 * long as the method runs, and a nursery destination owes no card.
 *
 * The `runtime-value-copy` suite runs the program under
 * `MONO_GC_DEBUG=check-remset-consistency`, which walks the old heap at each
 * minor collection and aborts on an old-to-young reference no remembered set
 * holds. That is what fails on a missing card. The counts below do not: a
 * collection that reclaims a Cell leaves its bytes readable, so the arm can
 * still find the value it wrote.
 *
 * Scrub () runs before each collection because of the same conservative scan. A
 * dead slot still holding a reference pins the object it names, and the
 * consistency check excuses a pinned target.
 *
 * The boxes and the holder cover the copy alone. Both are fresh objects, so
 * they are in the nursery and owe no card for what they hold.
 */

struct Pair {
	public object first;
	public string second;
	public int tag;
}

class Cell {
	public int value;

	public Cell (int value) { this.value = value; }
}

class Holder {
	public Pair pair;
}

class ValueCopy {
	const int N = 300;

	/* 600 * sizeof (Pair) is over SGEN_MAX_SMALL_OBJ_SIZE, which is 8000. */
	const int Slots = 600;

	static int failures;

	static void Check (string what, bool ok)
	{
		if (!ok) {
			Console.WriteLine ("FAILED: {0}", what);
			failures++;
		}
	}

	static string Name (int i)
	{
		return "pair-" + i;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Pair Make (int i)
	{
		Pair p;

		p.first = new Cell (i);
		p.second = Name (i);
		p.tag = i;
		return p;
	}

	/* stobj through the address of an element of an old array. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ArrayStep (Pair[] a, int at, int i)
	{
		a[at] = Make (i);
	}

	/* stfld of a value type. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void FieldStep (Holder h, int i)
	{
		h.pair = Make (i);
	}

	/* box of a value type, then a reference store of the box. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void BoxStep (object[] boxes, int at, int i)
	{
		boxes[at] = Make (i);
	}

	/* One old array element to another, which reads the heap on both ends. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void MoveStep (Pair[] a, int from, int to)
	{
		a[to] = a[from];
	}

	/*
	 * Overwrites the words the frames above left behind, so a reference a dead
	 * slot still holds does not pin the object it names.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Scrub (int depth)
	{
		long a = depth, b = ~depth, c = depth * 3, d = depth * 5;
		long e = depth * 7, f = depth * 11, g = depth * 13, h = depth * 17;
		long sum = a + b + c + d + e + f + g + h;

		return depth == 0 ? sum : sum + Scrub (depth - 1);
	}

	static bool Holds (Pair p, int i)
	{
		Cell cell = p.first as Cell;

		return cell != null && cell.value == i && p.second == Name (i) && p.tag == i;
	}

	public static int Main ()
	{
		Pair[] elements = new Pair[Slots];
		object[] boxes = new object[8];
		Holder holder = new Holder ();
		long scrubbed = 0;
		int inArray = 0, inField = 0, inBox = 0, inMoved = 0;

		for (int i = 0; i < N; ++i) {
			int at = i % Slots;
			int moved = (at + Slots / 2) % Slots;

			ArrayStep (elements, at, i);
			FieldStep (holder, i);
			BoxStep (boxes, i & 7, i);
			MoveStep (elements, at, moved);

			scrubbed += Scrub (24) + Scrub (24);
			GC.Collect (0);

			if (Holds (elements[at], i))
				inArray++;
			if (Holds (holder.pair, i))
				inField++;
			if (boxes[i & 7] is Pair && Holds ((Pair) boxes[i & 7], i))
				inBox++;
			if (Holds (elements[moved], i))
				inMoved++;
		}

		Check ("an array element keeps its references", inArray == N);
		Check ("a field keeps its references", inField == N);
		Check ("a boxed value type keeps its references", inBox == N);
		Check ("a copy between two array elements keeps its references", inMoved == N);

		/* Reading the sum keeps the scrubbing frames from being erased. */
		Check ("the scrub ran", scrubbed != 0);

		return failures == 0 ? 0 : 1;
	}
}
