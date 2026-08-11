using System;

/*
 * Cross-domain calls, which the interpreter reaches through an xdomain-invoke
 * wrapper rather than through the callee's own body.
 *
 * The wrapper serializes the arguments and makes the call itself with a calli
 * to a native helper, and that is the one call the interpreter makes with no
 * InterpMethod behind it -- a pinvoke outside a managed-to-native wrapper,
 * where the transform has nothing to resolve the callee to. A frame set up for
 * that call has no method, so anything reading one off it has to cope.
 *
 * A call whose arguments are all primitives takes a shorter path and does not
 * build that frame, so the shapes below return strings and pass reference types
 * deliberately.
 *
 * This is a plain program rather than a TestDriver corpus: --regression holds
 * the child domain open past the end of the run and hangs on the way out.
 */

public abstract class AbstractRemote : MarshalByRefObject {
	public abstract string PrimitiveParams (int a, uint b, char c, string d);
	public abstract string[] ArrayParams (string[] a);
}

public class Remote : AbstractRemote {
	public override string PrimitiveParams (int a, uint b, char c, string d) {
		return a + "," + b + "," + c + "," + d;
	}

	public override string[] ArrayParams (string[] a) {
		string[] r = new string [a.Length];
		for (int i = 0; i < a.Length; i++)
			r [i] = a [i] + "!";
		return r;
	}
}

public class Server : MarshalByRefObject {
	public AbstractRemote Make () {
		return new Remote ();
	}
}

class Tests {
	static int failed;

	static void check (string what, bool ok) {
		if (!ok) {
			Console.WriteLine ("FAILED: {0}", what);
			failed++;
		}
	}

	public static int Main () {
		AppDomain d = AppDomain.CreateDomain ("xdomain-tests");
		Server s = (Server)d.CreateInstanceAndUnwrap (
			typeof (Server).Assembly.FullName, "Server");
		AbstractRemote r = s.Make ();

		for (int i = 0; i < 100; i++)
			check ("primitive_params", r.PrimitiveParams (1, 2, 'x', "y") == "1,2,x,y");

		string[] arg = new string[] { "a", "b" };
		for (int i = 0; i < 100; i++) {
			string[] got = r.ArrayParams (arg);
			check ("array_params", got.Length == 2 && got [0] == "a!" && got [1] == "b!");
		}

		AppDomain.Unload (d);
		Console.WriteLine (failed == 0 ? "OK" : failed + " failures");
		return failed == 0 ? 0 : 1;
	}
}
