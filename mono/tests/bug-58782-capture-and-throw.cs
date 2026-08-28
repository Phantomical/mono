using System;
using System.Runtime.InteropServices;

class Driver
{
	[DllImport ("libtest")]
	static extern void mono_test_native_to_managed_exception_rethrow (Action action);

	[DllImport ("libc", EntryPoint = "_exit")]
	static extern void unix_exit (int exitCode);

	[DllImport ("msvcrt", EntryPoint = "_exit")]
	static extern void win32_exit (int exitCode);

	/* The handler below runs on an exception that is already unhandled, so it
	 * has to leave without unwinding any further. Windows has no libc, and
	 * msvcrt is the CRT that is always present. */
	static void _exit (int exitCode)
	{
		if (Environment.OSVersion.Platform == PlatformID.Unix
		    || Environment.OSVersion.Platform == PlatformID.MacOSX)
			unix_exit (exitCode);
		else
			win32_exit (exitCode);
	}

	static int Main (string[] args)
	{
		AppDomain.CurrentDomain.UnhandledException += (sender, exception_args) =>
		{
			CustomException exc = exception_args.ExceptionObject as CustomException;
			if (exc == null) {
				Console.WriteLine ($"FAILED - Unknown exception: {exception_args.ExceptionObject}");
				_exit (1);
			}

			Console.WriteLine (exc.StackTrace);
			if (string.IsNullOrEmpty (exc.StackTrace)) {
				Console.WriteLine ("FAILED - StackTrace is null for unhandled exception.");
				_exit (2);
			} else {
				Console.WriteLine ("SUCCESS - StackTrace is not null for unhandled exception.");
				_exit (0);
			}
		};

		mono_test_native_to_managed_exception_rethrow (CaptureAndThrow);
		Console.WriteLine ("Should have exited in the UnhandledException event handler.");
		return 2;
	}

	static void CaptureAndThrow ()
	{
		try {
			Throw ();
		} catch (Exception e) {
			System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture (e).Throw ();
		}
	}

	static void Throw ()
	{
		throw new CustomException ("C");
	}

	class CustomException : Exception
	{
		public CustomException(string s) : base(s) {}
	}
}