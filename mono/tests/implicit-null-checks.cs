using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A null dereference raises NullReferenceException at the site that made it,
 * at every tier.
 *
 * LLVM's ImplicitNullChecks pass folds the compare and branch the translator
 * emitted into the memory operation behind it, so a fault, and not a branch,
 * is what raises the exception. Mono reads no fault map: the SIGSEGV handler
 * turns a fault near null inside a compiled body into a NullReferenceException
 * from the faulting instruction.
 *
 * The two arms below are the two halves of that.
 *
 * A Bare case holds no clause, so its check folds. The exception has to leave
 * the method and reach the caller's catch, which is what says the unwinder
 * reads the faulting instruction as part of the body.
 *
 * A Guarded case faults inside a try. The gather grows each invoke's range over
 * the code around it whose IL offset lies in the same try region, so the fault
 * is inside the same range the calls around it are. Each Guarded case nests an
 * inner try inside an outer one, so a dispatch that misses the clause shows up
 * as the outer catch running. The finally between them runs either way.
 *
 * Each case runs at tier 0, tier 1 and tier 2 and must answer the same at all
 * three. That is what separates a fold that dispatches wrongly from a
 * translator bug present at every tier.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Holder {
	public int First;
	public long Second;
	public object Reference;

	public virtual int Virtual () { return First; }
}

/// An object whose last field sits more than a page past its start. A fold on
/// that field puts the faulting access outside the page the hardware traps
/// null through, so the pass has to decline it.
class Wide {
	public FarPadding Padding;
	public int Last;
}

struct FarPadding {
	public Padding512 P0, P1, P2, P3, P4, P5, P6, P7, P8;
}

struct Padding512 {
	public Padding64 Q0, Q1, Q2, Q3, Q4, Q5, Q6, Q7;
}

struct Padding64 {
	public long R0, R1, R2, R3, R4, R5, R6, R7;
}

static class Program {
	static int fails;

	/// Where a Guarded case caught its exception, and what ran on the way
	/// out. `Inner` is the answer every one of them wants. `Outer` means the
	/// dispatch missed the clause the faulting site sits in.
	enum Caught { None, Inner, Outer }

	struct Result {
		public Caught Where;
		public bool Finally;
	}

	/*
	 * Every case takes its null through a parameter, so none of them is a
	 * constant dereference the optimizer answers on its own.
	 */

	// The bare arm. No clause, so the check folds.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BareLoadField (Holder holder)
	{
		return holder.First;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void BareStoreField (Holder holder)
	{
		holder.Second = 7;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void BareStoreReference (Holder holder, object value)
	{
		holder.Reference = value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BareArrayLength (int[] array)
	{
		return array.Length;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BareArrayElement (int[] array)
	{
		return array[0];
	}

	/// A virtual call reads the receiver's vtable, which is the load at offset
	/// zero the fold has the least work to reach.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BareVirtualCall (Holder holder)
	{
		return holder.Virtual ();
	}

	/// The field past a page. The pass declines this one and the explicit
	/// branch raises the exception, which must read the same from here.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BareLoadFarField (Wide wide)
	{
		return wide.Last;
	}

	// The guarded arm. A clause protects the site.

	/// Copies a struct out of a null pointer. The backend expands the copy
	/// inline, so the check folds into the first load of that expansion and the
	/// fault lands in the middle of it rather than on a dereference of its own.
	[MethodImpl (MethodImplOptions.NoInlining)]
	unsafe static Result GuardedBlockCopy (Padding64 *source)
	{
		Result result = new Result ();

		try {
			try {
				Padding64 copy = *source;

				GC.KeepAlive (copy);
			} catch (NullReferenceException) {
				result.Where = Caught.Inner;
			} finally {
				result.Finally = true;
			}
		} catch (Exception) {
			result.Where = Caught.Outer;
		}

		return result;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Result GuardedLoadField (Holder holder)
	{
		Result result = new Result ();

		try {
			try {
				GC.KeepAlive (holder.First);
			} catch (NullReferenceException) {
				result.Where = Caught.Inner;
			} finally {
				result.Finally = true;
			}
		} catch (Exception) {
			result.Where = Caught.Outer;
		}

		return result;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Result GuardedStoreField (Holder holder)
	{
		Result result = new Result ();

		try {
			try {
				holder.Second = 7;
			} catch (NullReferenceException) {
				result.Where = Caught.Inner;
			} finally {
				result.Finally = true;
			}
		} catch (Exception) {
			result.Where = Caught.Outer;
		}

		return result;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Result GuardedArrayElement (int[] array)
	{
		Result result = new Result ();

		try {
			try {
				GC.KeepAlive (array[0]);
			} catch (NullReferenceException) {
				result.Where = Caught.Inner;
			} finally {
				result.Finally = true;
			}
		} catch (Exception) {
			result.Where = Caught.Outer;
		}

		return result;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Result GuardedVirtualCall (Holder holder)
	{
		Result result = new Result ();

		try {
			try {
				GC.KeepAlive (holder.Virtual ());
			} catch (NullReferenceException) {
				result.Where = Caught.Inner;
			} finally {
				result.Finally = true;
			}
		} catch (Exception) {
			result.Where = Caught.Outer;
		}

		return result;
	}

	static void Fail (string what, string tier, string how)
	{
		Console.WriteLine ("FAIL: {0} at {1} {2}", what, tier, how);
		++fails;
	}

	/// Runs one bare case and checks that its exception reached here.
	static void ExpectThrow (string what, string tier, Action body)
	{
		try {
			body ();
		} catch (NullReferenceException) {
			return;
		} catch (Exception e) {
			Fail (what, tier, "raised " + e.GetType ().Name);
			return;
		}

		Fail (what, tier, "raised nothing");
	}

	static void ExpectInner (string what, string tier, Result got)
	{
		if (got.Where != Caught.Inner)
			Fail (what, tier, "caught " + got.Where);
		if (!got.Finally)
			Fail (what, tier, "did not run its finally");
	}

	static void RunAll (string tier)
	{
		ExpectThrow ("BareLoadField", tier, () => BareLoadField (null));
		ExpectThrow ("BareStoreField", tier, () => BareStoreField (null));
		ExpectThrow ("BareStoreReference", tier,
			() => BareStoreReference (null, tier));
		ExpectThrow ("BareArrayLength", tier, () => BareArrayLength (null));
		ExpectThrow ("BareArrayElement", tier, () => BareArrayElement (null));
		ExpectThrow ("BareVirtualCall", tier, () => BareVirtualCall (null));
		ExpectThrow ("BareLoadFarField", tier, () => BareLoadFarField (null));

		ExpectInner ("GuardedLoadField", tier, GuardedLoadField (null));
		ExpectInner ("GuardedStoreField", tier, GuardedStoreField (null));
		ExpectInner ("GuardedArrayElement", tier, GuardedArrayElement (null));
		ExpectInner ("GuardedVirtualCall", tier, GuardedVirtualCall (null));
		unsafe { ExpectInner ("GuardedBlockCopy", tier, GuardedBlockCopy (null)); }

		// A case that must not throw, so a tier that raises an exception
		// everywhere fails here.
		Holder present = new Holder ();

		present.First = 11;
		if (BareLoadField (present) != 11)
			Fail ("BareLoadField", tier, "read the wrong field");
	}

	/// Calls each case without checking it, to give the tier-2 compile counts
	/// to lay each body out against.
	static void Warm ()
	{
		try { BareLoadField (null); } catch (NullReferenceException) { }
		try { BareStoreField (null); } catch (NullReferenceException) { }
		try { BareStoreReference (null, null); } catch (NullReferenceException) { }
		try { BareArrayLength (null); } catch (NullReferenceException) { }
		try { BareArrayElement (null); } catch (NullReferenceException) { }
		try { BareVirtualCall (null); } catch (NullReferenceException) { }
		try { BareLoadFarField (null); } catch (NullReferenceException) { }

		GuardedLoadField (null);
		GuardedStoreField (null);
		GuardedArrayElement (null);
		GuardedVirtualCall (null);
		unsafe { GuardedBlockCopy (null); }
	}

	static readonly string[] cases = {
		"BareLoadField", "BareStoreField", "BareStoreReference",
		"BareArrayLength", "BareArrayElement", "BareVirtualCall",
		"BareLoadFarField", "GuardedLoadField", "GuardedStoreField",
		"GuardedArrayElement", "GuardedVirtualCall", "GuardedBlockCopy",
	};

	static bool Promote (int tier, string tier_name)
	{
		foreach (string name in cases) {
			MethodInfo method = typeof (Program).GetMethod (name,
				BindingFlags.Static | BindingFlags.NonPublic);

			if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
				continue;

			Console.WriteLine ("FAIL: {0} would not compile at {1}",
				name, tier_name);
			++fails;
			return false;
		}

		return true;
	}

	public static int Main ()
	{
		RunAll ("tier 0");

		if (!Promote (2, "tier 1"))
			return 1;

		RunAll ("tier 1");

		for (int i = 0; i < 200; i++)
			Warm ();

		if (!Promote (3, "tier 2"))
			return 1;

		RunAll ("tier 2");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
