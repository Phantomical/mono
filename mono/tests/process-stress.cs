using System;
using System.Diagnostics;

public class Tests
{
	static int iterations = 512;

	// Process.Start (string) takes a *filename*, and with UseShellExecute on
	// that goes to the platform handler - xdg-open here, which reports that a
	// file called "echo -n" does not exist. The program and its argument have
	// to be separate, and the shell left out of it.
	static ProcessStartInfo StartInfo ()
	{
		ProcessStartInfo psi = new ProcessStartInfo ("echo", "-n");
		psi.UseShellExecute = false;

		return psi;
	}

	public static void Main(string[] args) {
		if (args.Length > 0)
			iterations = Int32.Parse (args [0]);

		// Spawn threads without waiting for them to exit
		for (int i = 0; i < iterations; i++) {
			Console.Write (".");
			//Console.WriteLine("Starting: " + i.ToString());
			using (var p = System.Diagnostics.Process.Start (StartInfo ())) {
				System.Threading.Thread.Sleep(10);
			}
		}

		// Spawn threads and wait for them to exit
		for (int i = 0; i < iterations; i++) {
			Console.Write (".");
			//Console.WriteLine("Starting: " + i.ToString());
			using (var p = System.Diagnostics.Process.Start (StartInfo ())) {
				p.WaitForExit ();
            }
        }

		Console.WriteLine ();
	}
}
