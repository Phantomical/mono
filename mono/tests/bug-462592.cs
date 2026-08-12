using System;
using System.IO;
using System.Threading;
using System.Reflection;
using System.Reflection.Emit;

public class Test
{
	public virtual void Foo<T> (int i)
	{
	}
}

class Driver
{
		static AssemblyBuilder assembly;
		static ModuleBuilder module;
		static string ASSEMBLY_NAME = "MonoTests.System.Reflection.Emit.TypeBuilderTest";

		static void SetUp ()
		{
			AssemblyName assemblyName = new AssemblyName ();
			assemblyName.Name = ASSEMBLY_NAME;

			// Save into the temp directory rather than the working directory, which every
			// arm of this test shares. The assembly is loaded back below, so the reader has
			// it mapped while another arm's Save is unlinking and rewriting the same path.
			assembly =
				Thread.GetDomain ().DefineDynamicAssembly (
					assemblyName, AssemblyBuilderAccess.RunAndSave, Path.GetTempPath ());

			module = assembly.DefineDynamicModule ("repro", "bug-462592-result.exe");
		}

		static int Main()
		{
			SetUp ();

			MethodInfo foo = typeof (Test).GetMethod ("Foo");
			TypeBuilder type = module.DefineType ("TestType", TypeAttributes.Public, typeof (Test), Type.EmptyTypes);

			MethodBuilder mb = type.DefineMethod ("Foo", MethodAttributes.Public | MethodAttributes.Virtual, typeof (void), new Type[] { typeof (int) });
			mb.DefineGenericParameters ("T");

			ILGenerator il = mb.GetILGenerator ();
			il.Emit (OpCodes.Ldarg_0);
			il.Emit (OpCodes.Ldc_I4, 0);
			il.Emit (OpCodes.Call, foo);
			il.Emit (OpCodes.Ret);

			type.DefineMethodOverride (mb, foo);

			MethodBuilder main = type.DefineMethod ("Main", MethodAttributes.Public | MethodAttributes.Static, typeof (void), Type.EmptyTypes);
			il = main.GetILGenerator ();
			il.Emit (OpCodes.Newobj, type.DefineDefaultConstructor (MethodAttributes.Public));
			il.Emit (OpCodes.Ldc_I4, 0);
			il.Emit (OpCodes.Callvirt, mb.MakeGenericMethod (new Type[] { typeof (string) }));
			il.Emit (OpCodes.Ret);

			type.CreateType ();
			assembly.SetEntryPoint (main);

			assembly.Save ("bug-462592-result.exe");

			Assembly res = Assembly.LoadFrom (Path.Combine (Path.GetTempPath (), "bug-462592-result.exe"));
			res.EntryPoint.Invoke (null, new object[0]);
			return 0;
		}
}

