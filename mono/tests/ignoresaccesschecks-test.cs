using System;
using System.Runtime.CompilerServices;

// Ahead of every type, because C# requires that of an assembly attribute. The
// type it names is declared below: the attribute is not in corlib, so an
// assembly that wants it declares its own. That is what a /ignoreAccessChecksTo
// build emits, and it is why the runtime matches the attribute by name rather
// than by class.
[assembly: IgnoresAccessChecksTo ("ignoresaccesschecks-granted")]

namespace System.Runtime.CompilerServices
{
	[AttributeUsage (AttributeTargets.Assembly, AllowMultiple = true)]
	internal sealed class IgnoresAccessChecksToAttribute : Attribute
	{
		public IgnoresAccessChecksToAttribute (string assemblyName)
		{
			AssemblyName = assemblyName;
		}

		public string AssemblyName { get; private set; }
	}
}

class Program
{
	// One access per method, and none of them inlined. The interpreter
	// refuses the access while it transforms the method that holds it, so an
	// access sitting in Main would take Main down with it and never reach a
	// catch.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GrantedPrivateField () { return GrantedAssembly.Target.privateField; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GrantedPrivateMethod () { return GrantedAssembly.Target.PrivateStaticMethod (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GrantedInternalMethod () { return GrantedAssembly.Target.InternalStaticMethod (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GrantedInternalClass () { return GrantedAssembly.InternalClass.Value (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UngrantedPrivateField () { return UngrantedAssembly.Target.privateField; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UngrantedPrivateMethod () { return UngrantedAssembly.Target.PrivateStaticMethod (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UngrantedInternalClass () { return UngrantedAssembly.InternalClass.Value (); }

	static int failures;

	static void Granted (string what, Func<int> access, int want)
	{
		try {
			int got = access ();
			if (got != want) {
				Console.WriteLine ("{0}: wrong value {1}, want {2}", what, got, want);
				failures++;
			} else {
				Console.WriteLine ("{0}: OK", what);
			}
		} catch (MemberAccessException e) {
			Console.WriteLine ("{0}: refused - {1}", what, e.GetType ().Name);
			failures++;
		}
	}

	// The negative arm. Without it a runtime that stopped checking access
	// altogether would pass this test.
	static void Refused (string what, Func<int> access)
	{
		try {
			access ();
			Console.WriteLine ("{0}: allowed, want refused", what);
			failures++;
		} catch (MemberAccessException) {
			Console.WriteLine ("{0}: refused, as it should be", what);
		} catch (TypeLoadException) {
			// A type that is not visible fails to load rather than to
			// bind. Both outcomes are a refusal.
			Console.WriteLine ("{0}: refused, as it should be", what);
		}
	}

	static int Main ()
	{
		Granted ("granted private field", GrantedPrivateField, 42);
		Granted ("granted private method", GrantedPrivateMethod, 43);
		Granted ("granted internal method", GrantedInternalMethod, 44);
		Granted ("granted internal class", GrantedInternalClass, 45);

		Refused ("ungranted private field", UngrantedPrivateField);
		Refused ("ungranted private method", UngrantedPrivateMethod);
		Refused ("ungranted internal class", UngrantedInternalClass);

		Console.WriteLine (failures == 0 ? "OK" : failures + " failures");
		return failures;
	}
}
