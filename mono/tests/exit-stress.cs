//
// https://bugzilla.novell.com/show_bug.cgi?id=379524
//

using System;
using System.Globalization;
using System.Threading;

class Program
{
  // Main returns 1 and one of the threads calls Environment.Exit (0). The
  // threads are foreground, so the runtime is still winding them down when the
  // exit arrives, and 0 has to be the code the process leaves with. Waiting on
  // this puts the exit on the far side of Main's return rather than wherever
  // the scheduler happens to land it.
  static ManualResetEvent mainReturned = new ManualResetEvent (false);

  static int Main (string [] args)
  {
    for (int i = 0; i < 1000; ++i) {
      Thread thread = new Thread (new ParameterizedThreadStart (Test));
      thread.Start (i);
    }
    mainReturned.Set ();
    return 1;
  }

  static void Test (object index)
  {
    if ((int) index == 500) {
      mainReturned.WaitOne ();
      Environment.Exit (0);
    }

    // Thread.CurrentCulture acts on whichever thread sets it, so a culture the
    // threads are each meant to get has to be set from inside them.
    Thread.CurrentThread.CurrentCulture = new CultureInfo ("en-CA");
  }
}
