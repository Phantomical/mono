using System;
using System.Reflection;
using System.Text;

// Mono.Runtime.DumpStateTotal () summarizes every thread into a JSON payload.
// Dumping twice in one process used to emit a leading comma in the threads
// array on the second call, so the payload parsed as JSON only the first time.
class DumpStateJsonValid
{
	static string DumpStateTotal ()
	{
		var monoType = Type.GetType ("Mono.Runtime", false);
		if (monoType == null)
			throw new Exception ("no Mono.Runtime");

		var dump = monoType.GetMethod ("DumpStateTotal",
		                               BindingFlags.NonPublic | BindingFlags.Static);
		if (dump == null)
			throw new Exception ("no Mono.Runtime.DumpStateTotal");

		var output = (Tuple<String, ulong, ulong>) dump.Invoke (null, Array.Empty<object> ());
		return output.Item1;
	}

	public static int Main ()
	{
		// Three, not two: the bug is in a flag that latches, so the first call
		// is always well formed and every later one is not.
		for (int call = 1; call <= 3; call++) {
			string payload = DumpStateTotal ();

			if (String.IsNullOrEmpty (payload)) {
				Console.WriteLine ("dump {0}: empty payload", call);
				return 1;
			}

			try {
				Json.Validate (payload);
			} catch (Exception e) {
				Console.WriteLine ("dump {0}: {1}", call, e.Message);
				Console.WriteLine (payload);
				return 1;
			}

			Console.WriteLine ("dump {0}: {1} bytes, valid", call, payload.Length);
		}

		return 0;
	}
}

// Enough of a JSON reader to say whether the payload is well formed. corlib
// ships no parser this test can reach: the one the old merp test used lives in
// System.Web.Extensions, which this profile does not have.
class Json
{
	readonly string s;
	int i;

	Json (string text)
	{
		s = text;
		i = 0;
	}

	public static void Validate (string text)
	{
		var p = new Json (text);
		p.Space ();
		p.Value ();
		p.Space ();
		if (p.i != p.s.Length)
			throw p.Error ("trailing content");
	}

	Exception Error (string what)
	{
		int from = Math.Max (0, i - 30);
		int len = Math.Min (60, s.Length - from);
		return new Exception (String.Format ("{0} at offset {1}, near: {2}",
		                                     what, i, s.Substring (from, len)));
	}

	void Space ()
	{
		while (i < s.Length && (s [i] == ' ' || s [i] == '\t' || s [i] == '\n' || s [i] == '\r'))
			i++;
	}

	char Peek ()
	{
		if (i >= s.Length)
			throw Error ("unexpected end of input");
		return s [i];
	}

	void Expect (char c)
	{
		if (Peek () != c)
			throw Error (String.Format ("expected '{0}', found '{1}'", c, s [i]));
		i++;
	}

	void Value ()
	{
		switch (Peek ()) {
		case '{':
			Object ();
			return;
		case '[':
			Array ();
			return;
		case '"':
			String_ ();
			return;
		case 't':
			Literal ("true");
			return;
		case 'f':
			Literal ("false");
			return;
		case 'n':
			Literal ("null");
			return;
		default:
			Number ();
			return;
		}
	}

	void Object ()
	{
		Expect ('{');
		Space ();
		if (Peek () == '}') {
			i++;
			return;
		}

		for (;;) {
			Space ();
			String_ ();
			Space ();
			Expect (':');
			Space ();
			Value ();
			Space ();
			if (Peek () == ',') {
				i++;
				continue;
			}
			Expect ('}');
			return;
		}
	}

	void Array ()
	{
		Expect ('[');
		Space ();
		if (Peek () == ']') {
			i++;
			return;
		}

		for (;;) {
			Space ();
			Value ();
			Space ();
			if (Peek () == ',') {
				i++;
				continue;
			}
			Expect (']');
			return;
		}
	}

	void String_ ()
	{
		Expect ('"');
		while (Peek () != '"') {
			if (s [i] == '\\')
				i++;
			i++;
		}
		i++;
	}

	void Literal (string word)
	{
		if (i + word.Length > s.Length || s.Substring (i, word.Length) != word)
			throw Error (String.Format ("expected '{0}'", word));
		i += word.Length;
	}

	void Number ()
	{
		int start = i;
		if (Peek () == '-')
			i++;
		while (i < s.Length && ((s [i] >= '0' && s [i] <= '9') || s [i] == '.' ||
		                        s [i] == 'e' || s [i] == 'E' || s [i] == '+' || s [i] == '-'))
			i++;
		if (i == start)
			throw Error ("expected a value");
	}
}
