using System;
using System.Text;
using System.Globalization;
using System.Collections.Generic;
using System.Reflection;
using System.Threading;
using System.IO;

public class Tests
{
	static int nloops = 1;
	static int nthreads = 10;

	// Where the runtime loaded corlib from, which is the profile directory of
	// whatever build is under test. Asking corlib rather than naming a path
	// keeps this off an installed prefix: with one hardcoded, a machine with no
	// install fails before loading anything, and a machine with one stresses
	// the *installed* assemblies instead of the ones just built.
	static string AssemblyDir ()
	{
		string dir = Path.GetDirectoryName (typeof (object).Assembly.Location);

		return string.IsNullOrEmpty (dir) ? AppDomain.CurrentDomain.BaseDirectory : dir;
	}

	public static void Main (String[] args) {
		if (args.Length > 0)
			nloops = int.Parse (args [0]);
		if (args.Length > 1)
			nthreads = int.Parse (args [1]);

		for (int li = 0; li < nloops; ++li) {
			Thread[] threads = new Thread [nthreads];
			for (int i = 0; i < nthreads; ++i) {
				threads [i] = new Thread (delegate () {
						foreach (string s in Directory.GetFiles (AssemblyDir (), "*.dll")) {
							AssemblyName.GetAssemblyName (s);
						}
					});
			}
			for (int i = 0; i < 10; ++i)
				threads [i].Start ();
		}
	}
}
