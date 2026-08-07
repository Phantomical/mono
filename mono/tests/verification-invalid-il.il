// A body the IL verifier rejects, wrapped in one that reports what happened.
//
// Probe::Unverifiable enters a protected region with a value still on the
// evaluation stack, which ECMA-335 III.1.7.5 forbids. It is nonetheless
// something both engines run: the value is consumed inside the region and the
// method returns 42.
//
// So the program prints "ran 42" when nothing verifies it, and "rejected
// <exception>" when something does - which is the difference the
// --security= flags are supposed to make.

.assembly extern mscorlib { }
.assembly 'verification-invalid-il' { }

.class public sealed Probe extends [mscorlib]System.Object {

	.method public static int32 Unverifiable () cil managed {
		.maxstack 3
		.locals init (int32 total)
		ldc.i4 40
		.try {
			ldc.i4.2
			add
			stloc.0
			leave.s DONE
		} catch [mscorlib]System.Exception {
			pop
			ldc.i4.m1
			stloc.0
			leave.s DONE
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static int32 Main () cil managed {
		.entrypoint
		.maxstack 3
		.locals init (string report)
		.try {
			ldstr "ran "
			call int32 Probe::Unverifiable ()
			box [mscorlib]System.Int32
			call string [mscorlib]System.String::Concat(object, object)
			stloc.0
			leave.s DONE
		} catch [mscorlib]System.Exception {
			callvirt instance class [mscorlib]System.Type [mscorlib]System.Object::GetType()
			callvirt instance string [mscorlib]System.Type::get_FullName()
			stloc.0
			ldstr "rejected "
			ldloc.0
			call string [mscorlib]System.String::Concat(string, string)
			stloc.0
			leave.s DONE
		}
	DONE:
		ldloc.0
		call void [mscorlib]System.Console::WriteLine(string)
		ldc.i4.0
		ret
	}
}
