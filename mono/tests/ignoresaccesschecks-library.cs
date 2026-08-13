using System;

// Built twice: once as the assembly the test names in
// IgnoresAccessChecksTo, once as one it does not name. The namespace follows
// the build so the test can reach both from one compilation.
//
// PERMISSIVE makes every member public. The test is compiled against that
// build, because no C# compiler here will emit a reference to a private member
// of another assembly; the library is then rebuilt without it, which is what
// leaves the run-time check something to reject.
#if GRANTED
namespace GrantedAssembly {
#else
namespace UngrantedAssembly {
#endif

	public class Target
	{
#if PERMISSIVE
		public
#else
		private
#endif
		 static int privateField = 42;

#if PERMISSIVE
		public
#else
		private
#endif
		 static int PrivateStaticMethod () { return 43; }

#if PERMISSIVE
		public
#else
		internal
#endif
		 static int InternalStaticMethod () { return 44; }
	}

#if PERMISSIVE
	public
#else
	internal
#endif
	 class InternalClass
	{
		public static int Value () { return 45; }
	}
}
