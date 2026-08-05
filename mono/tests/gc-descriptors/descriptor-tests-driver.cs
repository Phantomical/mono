public class DescriptorTest
{
	// One pass allocates every shape the generator emitted, so the coverage
	// worth having is a handful of passes plus the collections they trigger:
	// by thirty every shape has been through several nursery collections and
	// a few major ones, and both the bare and the array-wrapped form of each
	// has been allocated. Past that the loop only repeats itself.
	const int DefaultPasses = 30;

	public static int Main (string[] args)
	{
		var passes = args.Length > 0 ? long.Parse (args [0]) : DefaultPasses;
		var iterations = passes * Bitmaps.NumWhich;

		var r = new Random (31415);
		var objs = new object [9];
		var which = 0;
		var last = new Filler [Bitmaps.NumWhich];
		for (long i = 0; i < iterations; ++i)
		{
			var o = Bitmaps.MakeAndFill (which, objs, r.Next (2) == 0);
			objs [r.Next (objs.Length)] = o;
			last [which] = o;

			if (i % 761 == 0)
			{
				var l = last [r.Next (Bitmaps.NumWhich)];
				if (l != null)
					l.Fill (objs);
			}

			if (i % 5 == 0)
				objs [r.Next (objs.Length)] = null;

			if (++which >= Bitmaps.NumWhich)
				which = 0;
		}

		// `last` holds one live instance of every shape at once, so collecting
		// here traces the whole descriptor set in a single pass. Writing
		// through each survivor afterwards is what turns a field the collector
		// mistraced into a crash rather than into nothing observable.
		GC.Collect ();
		var live = 0;
		foreach (var l in last)
		{
			if (l == null)
				continue;
			l.Fill (objs);
			++live;
		}
		GC.Collect ();

		Console.WriteLine ("{0} iterations over {1} shapes, {2} live",
				   iterations, Bitmaps.NumWhich, live);

		if (live != Bitmaps.NumWhich)
		{
			Console.WriteLine ("FAILED: expected {0} live shapes", Bitmaps.NumWhich);
			return 1;
		}
		return 0;
	}
}
