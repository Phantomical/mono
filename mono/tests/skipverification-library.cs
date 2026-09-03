// Built twice, the way ignoresaccesschecks-library.cs is. PERMISSIVE makes every
// member public, because no C# compiler here will emit a reference to a private
// member of another assembly. The library is then rebuilt without it, which
// leaves the run-time check something to reject.
namespace SkipVerificationLibrary
{
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
	}
}
