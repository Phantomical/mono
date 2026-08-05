using System;
using System.Text;

/*
 * A finally whose every path throws has no endfinally at all: the C# compiler drops
 * the unreachable one. The leave out of the try still chains through the handler, so
 * the handler has to run - once, in the usual order - even though nothing ever
 * resumes at the leave's target.
 */
class FinallyThrowingHandler {
	static StringBuilder log = new StringBuilder ();

	static void L (string s)
	{
		log.Append (s);
		log.Append (';');
	}

	static int ThrowingFinally ()
	{
		try {
			L ("try1");
		} finally {
			L ("fin1");
			throw new InvalidOperationException ("boom");
		}
	}

	/* The outer finally still has to run while the inner one's throw unwinds. */
	static void Nested ()
	{
		try {
			try {
				L ("try2");
			} finally {
				L ("fin2-inner");
				throw new InvalidOperationException ("inner");
			}
		} finally {
			L ("fin2-outer");
		}
	}

	/* Two leaves out of one try, both landing in a handler that throws. */
	static void TwoLeaves (bool early)
	{
		try {
			if (early) {
				L ("early");
				return;
			}
			L ("late");
		} finally {
			L ("fin3");
			throw new InvalidOperationException ("four");
		}
	}

	static int Main ()
	{
		try {
			ThrowingFinally ();
			L ("NOTREACHED1");
		} catch (InvalidOperationException e) {
			L ("caught:" + e.Message);
		}

		try {
			Nested ();
			L ("NOTREACHED2");
		} catch (InvalidOperationException e) {
			L ("caught:" + e.Message);
		}

		foreach (bool early in new bool [] { true, false }) {
			try {
				TwoLeaves (early);
				L ("NOTREACHED3");
			} catch (InvalidOperationException e) {
				L ("caught:" + e.Message);
			}
		}

		string expected = "try1;fin1;caught:boom;"
			+ "try2;fin2-inner;fin2-outer;caught:inner;"
			+ "early;fin3;caught:four;late;fin3;caught:four;";

		if (log.ToString () != expected) {
			Console.WriteLine ("expected: " + expected);
			Console.WriteLine ("     got: " + log.ToString ());
			return 1;
		}

		return 0;
	}
}
