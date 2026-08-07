// Lists the test classes in an xunit test assembly, one full name per line.
//
// The console in external/xunit-binaries is a prebuilt 2.4.1 binary with no
// listing mode, so the build cannot ask it what an assembly contains.  It does
// accept -class/-noclass, which match a class's full name exactly, so a list of
// names is enough to cut an assembly into disjoint pieces.
//
// This walks the assembly with reflection rather than driving xunit's own
// discovery.  That is coarser -- it says which classes hold tests, not which
// cases they expand into -- but it is all the -class filter can use anyway, and
// the caller pairs the groups it builds with a -noclass complement, so a class
// this misses still runs.

using System;
using System.Collections.Generic;
using System.Reflection;

static class XunitLister
{
	// [Fact] and everything derived from it, [Theory] included.
	static bool IsTestMethod (MethodInfo m)
	{
		foreach (var attr in CustomAttributeData.GetCustomAttributes (m)) {
			for (var t = attr.Constructor.DeclaringType; t != null; t = t.BaseType)
				if (t.FullName == "Xunit.FactAttribute")
					return true;
		}
		return false;
	}

	static bool HoldsTests (Type t)
	{
		if (!t.IsClass)
			return false;
		const BindingFlags all = BindingFlags.Public | BindingFlags.NonPublic
			| BindingFlags.Instance | BindingFlags.Static | BindingFlags.FlattenHierarchy;
		foreach (var m in t.GetMethods (all))
			if (IsTestMethod (m))
				return true;
		return false;
	}

	public static int Main (string[] args)
	{
		if (args.Length != 1) {
			Console.Error.WriteLine ("usage: xunit-lister.exe <assembly>");
			return 2;
		}

		Type[] types;
		try {
			var asm = Assembly.LoadFrom (args[0]);
			try {
				types = asm.GetTypes ();
			} catch (ReflectionTypeLoadException e) {
				// A suite that references something absent still lists what did load.
				var kept = new List<Type> ();
				foreach (var t in e.Types)
					if (t != null)
						kept.Add (t);
				types = kept.ToArray ();
			}
		} catch (Exception e) {
			Console.Error.WriteLine ("xunit-lister: cannot read {0}: {1}", args[0], e.Message);
			return 1;
		}

		var names = new List<string> ();
		foreach (var t in types) {
			bool holds;
			try {
				holds = HoldsTests (t);
			} catch (Exception e) {
				Console.Error.WriteLine ("xunit-lister: skipping {0}: {1}", t.FullName, e.Message);
				continue;
			}
			if (holds)
				names.Add (t.FullName);
		}

		names.Sort (StringComparer.Ordinal);
		foreach (var n in names)
			Console.WriteLine (n);
		return 0;
	}
}
