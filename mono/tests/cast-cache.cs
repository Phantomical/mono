using System;
using System.Runtime.CompilerServices;

/*
 * The cast cache that generated code reads, against the answers the runtime
 * gives.
 *
 * A cache slot belongs to one site and holds one vtable, so which path a site
 * takes depends on what it saw last. A site entered with one class every time
 * keeps its entry and reads it, which is the fast path. A site entered with two
 * classes overwrites the entry before it can be read, and takes the helper
 * every time. Both need cover, so the sites below are separate methods and each
 * group is entered its own way.
 *
 * isinst caches a refusal as well as a pass, in bit 0 of the same slot. A site
 * that only ever refuses is what reads that bit back.
 */

interface IShape { int Sides (); }

class Base { public virtual int Tag () { return 1; } }
class Left : Base, IShape { public override int Tag () { return 2; } public int Sides () { return 3; } }
class Right : Base { public override int Tag () { return 3; } }
class Other { }

struct Val { public int n; }

class Generic<T> where T : class {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static T AsT (object o) { return o as T; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static T CastT (object o) { return (T) o; }
}

static class Program {
	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/* One site each, and the caller gives each one the same class every time. */

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Left AsLeftPass (object o) { return o as Left; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Left AsLeftRefuse (object o) { return o as Left; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IShape AsShapePass (object o) { return o as IShape; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IShape AsShapeRefuse (object o) { return o as IShape; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsLeftRefuse (object o) { return o is Left; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Base CastBasePass (object o) { return (Base) o; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Base CastBaseThrow (object o) { return (Base) o; }

	/* One site, and the caller alternates, so the slot never answers. */

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Left AsLeftMixed (object o) { return o as Left; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Base CastBaseMixed (object o) { return (Base) o; }

	public static int Main ()
	{
		object left = new Left (), right = new Right (), other = new Other ();
		object boxed = (object) new Val { n = 7 };

		// Enough entries to leave both compiled tiers behind.
		for (int i = 0; i < 60000; ++i) {
			/*
			 * The cached answer is read back. Each of these sites saw the
			 * same class on the entry before, so from the second entry on
			 * the slot is what answers.
			 */
			Check (AsLeftPass (left) != null, "as Left on a Left");
			Check (AsLeftRefuse (right) == null, "as Left on a Right");
			Check (AsShapePass (left) != null, "as IShape on a Left");
			Check (AsShapeRefuse (right) == null, "as IShape on a Right");
			Check (!IsLeftRefuse (other), "is Left on an Other");
			Check (CastBasePass (left) != null, "cast to Base from a Left");

			/* The slot holds another class, so the helper answers. */
			Check (AsLeftMixed (left) != null, "mixed site, as Left on a Left");
			Check (AsLeftMixed (right) == null, "mixed site, as Left on a Right");
			Check (AsLeftMixed (boxed) == null, "mixed site, as Left on a boxed value");
			Check (CastBaseMixed (left) != null, "mixed site, cast to Base from a Left");
			Check (CastBaseMixed (right) != null, "mixed site, cast to Base from a Right");

			/* Null never reads the slot, whatever the slot holds. */
			Check (AsLeftPass (null) == null, "as Left on null");
			Check (AsShapePass (null) == null, "as IShape on null");
			Check (CastBasePass (null) == null, "cast to Base from null");

			/* A shared body reaches its slot through the runtime context. */
			Check (Generic<Left>.AsT (left) != null, "shared as T on a Left");
			Check (Generic<Left>.AsT (right) == null, "shared as T on a Right");
			Check (Generic<Base>.CastT (right) != null, "shared cast to T from a Right");

			/*
			 * A failed castclass is never cached, so this site takes the
			 * helper and throws on every entry.
			 */
			bool threw = false;

			try {
				CastBaseThrow (other);
			} catch (InvalidCastException) {
				threw = true;
			}

			Check (threw, "cast to Base from an Other throws");

			if (fails != 0) {
				Console.WriteLine ("stopped at iteration {0}", i);
				return 1;
			}
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
