using System;
using System.Collections.Generic;

/*
 * An IMT slot is built from the interfaces the class implements, but a call can
 * reach it with a key naming a different interface method: variance and array
 * covariance both match on the generic type definition, and a generic virtual
 * method's instantiations are discovered one at a time. Each case below builds a
 * slot through one key and then dispatches through another, which is the order
 * that goes wrong if a slot is filled with a single target it can no longer
 * revise.
 */

interface IPlain { string Plain (); }
interface IPlain2 { string Plain2 (); }
interface IInv<T> { string Inv (); }

interface ICov<out T> { string Cov (); }
interface ICon<in T> { string Con (); }

interface IGenVirt { string GV<T> (); }
interface IGenVirtG<T> { string GVG<U> (); }

class Impl : IPlain, IPlain2, IInv<int>, ICov<string>, ICon<object>, IGenVirt, IGenVirtG<string> {
	public string Plain () { return "Plain"; }
	public string Plain2 () { return "Plain2"; }
	public string Inv () { return "Inv"; }
	public string Cov () { return "Cov"; }
	public string Con () { return "Con"; }
	public string GV<T> () { return "GV:" + typeof (T).Name; }
	public string GVG<U> () { return "GVG:" + typeof (U).Name; }
}

/* Variance reached before the exact instantiation ever is. */
class CovOnly : ICov<string> {
	public string Cov () { return "CovOnly"; }
}

/* A plain interface sharing a class with a generic virtual one. */
class Mixed : IPlain, IGenVirt {
	public string Plain () { return "MixedPlain"; }
	public string GV<T> () { return "MixedGV:" + typeof (T).Name; }
}

public class Driver {
	static int failures;

	/* Well past THUNK_THRESHOLD, so the rebuild-on-tenth-call path runs too. */
	const int Reps = 25;

	static void Check (string got, string want, string what)
	{
		if (got != want) {
			Console.Error.WriteLine ("{0}: got '{1}', expected '{2}'", what, got, want);
			failures++;
		}
	}

	static void Repeat (Func<string> call, string want, string what)
	{
		for (int i = 0; i < Reps; i++)
			Check (call (), want, what);
	}

	static void PlainInterfaces ()
	{
		Impl o = new Impl ();

		Repeat (() => ((IPlain)o).Plain (), "Plain", "IPlain.Plain");
		Repeat (() => ((IPlain2)o).Plain2 (), "Plain2", "IPlain2.Plain2");
		Repeat (() => ((IInv<int>)o).Inv (), "Inv", "IInv<int>.Inv");
	}

	static void Variance ()
	{
		Impl o = new Impl ();

		Repeat (() => ((ICov<string>)o).Cov (), "Cov", "ICov<string>.Cov");
		Repeat (() => ((ICov<object>)o).Cov (), "Cov", "ICov<object>.Cov");
		Repeat (() => ((ICov<IComparable>)o).Cov (), "Cov", "ICov<IComparable>.Cov");

		Repeat (() => ((ICon<object>)o).Con (), "Con", "ICon<object>.Con");
		Repeat (() => ((ICon<string>)o).Con (), "Con", "ICon<string>.Con");
		Repeat (() => ((ICon<Exception>)o).Con (), "Con", "ICon<Exception>.Con");

		/* The other order: the variant key arrives first. */
		CovOnly c = new CovOnly ();
		Repeat (() => ((ICov<object>)c).Cov (), "CovOnly", "CovOnly as ICov<object>");
		Repeat (() => ((ICov<string>)c).Cov (), "CovOnly", "CovOnly as ICov<string>");
	}

	static void ArrayCovariance ()
	{
		string[] arr = new string[] { "a", "b", "c" };
		object[] widened = arr;

		IList<string> exact = arr;
		Repeat (() => exact [0], "a", "IList<string>.get_Item");

		/* IList`1 is invariant, so this is array covariance rather than variance. */
		IList<object> covariant = widened;
		Repeat (() => (string)covariant [1], "b", "IList<object>.get_Item");

		ICollection<object> coll = widened;
		Repeat (() => coll.Count.ToString (), "3", "ICollection<object>.get_Count");

		IEnumerable<object> seq = widened;
		Repeat (() => {
			int n = 0;
			foreach (object x in seq) n++;
			return n.ToString ();
		}, "3", "IEnumerable<object>.GetEnumerator");
	}

	static void GenericVirtuals ()
	{
		Impl o = new Impl ();
		IGenVirt gv = o;

		Repeat (() => gv.GV<int> (), "GV:Int32", "IGenVirt.GV<int>");
		Repeat (() => gv.GV<string> (), "GV:String", "IGenVirt.GV<string>");
		Repeat (() => gv.GV<long> (), "GV:Int64", "IGenVirt.GV<long>");
		Repeat (() => gv.GV<Exception> (), "GV:Exception", "IGenVirt.GV<Exception>");
		Check (gv.GV<int> (), "GV:Int32", "IGenVirt.GV<int> revisited");

		IGenVirtG<string> gvg = o;
		Repeat (() => gvg.GVG<int> (), "GVG:Int32", "IGenVirtG<string>.GVG<int>");
		Repeat (() => gvg.GVG<object> (), "GVG:Object", "IGenVirtG<string>.GVG<object>");
	}

	static void PlainThenGenericVirtual ()
	{
		Mixed m = new Mixed ();

		Repeat (() => ((IPlain)m).Plain (), "MixedPlain", "Mixed.Plain");
		Repeat (() => ((IGenVirt)m).GV<int> (), "MixedGV:Int32", "Mixed.GV<int>");
		Repeat (() => ((IGenVirt)m).GV<string> (), "MixedGV:String", "Mixed.GV<string>");
		Repeat (() => ((IPlain)m).Plain (), "MixedPlain", "Mixed.Plain revisited");
	}

	public static int Main ()
	{
		PlainInterfaces ();
		Variance ();
		ArrayCovariance ();
		GenericVirtuals ();
		PlainThenGenericVirtual ();

		if (failures != 0) {
			Console.Error.WriteLine ("{0} failures", failures);
			return 1;
		}
		return 0;
	}
}
