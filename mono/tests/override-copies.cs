using System;
using System.IO;
using System.Reflection;

/*
 * Two loaded copies of one target, which is what a process full of Harmony
 * repacks looks like - each mod ships its own repack of the assembly it
 * patches. Matching by name rather than by type identity is what catches all
 * of them, and this is the case that tells the two apart: a table keyed on the
 * first MonoClass it saw replaces one copy and leaves the other running its
 * own body.
 *
 * Both copies are loaded through reflection rather than referenced, since they
 * declare the same types under two assembly names.
 */

namespace Mono.Test {

	public class Test {
		static int failures;

		static void Check (string what, long got, long want)
		{
			if (got == want)
				return;

			Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
			failures++;
		}

		static MethodInfo ValueOf (string name)
		{
			string path = Path.Combine (AppDomain.CurrentDomain.BaseDirectory, name);
			Assembly assembly = Assembly.LoadFrom (path);
			Type type = assembly.GetType ("Mono.Test.Copied");

			if (type == null) {
				Console.WriteLine ("{0} holds no Mono.Test.Copied", name);
				failures++;
				return null;
			}

			return type.GetMethod ("Value", BindingFlags.Public | BindingFlags.Static);
		}

		static void Exercise (string what, MethodInfo value)
		{
			if (value == null)
				return;

			object[] args = new object[1];

			for (int i = 0; i < 200; i++) {
				args[0] = i;
				Check (what, (int) value.Invoke (null, args), i + 100);
			}
		}

		public static int Main ()
		{
			MethodInfo a = ValueOf ("override-copy-a.dll");
			MethodInfo b = ValueOf ("override-copy-b.dll");

			if (a != null && b != null && a == b) {
				Console.WriteLine ("the two copies gave one method, so this "
				                   + "checks a single image");
				failures++;
			}

			Exercise ("copy-a", a);
			Exercise ("copy-b", b);

			if (failures != 0) {
				Console.WriteLine ("{0} failures", failures);
				return 1;
			}

			Console.WriteLine ("OK");
			return 0;
		}
	}
}
