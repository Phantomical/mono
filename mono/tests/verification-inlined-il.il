// An unverifiable body small enough for the interpreter to inline, wrapped in
// one that reports what happened.
//
// Probe::Small takes the address of an init-only field outside a constructor,
// which ECMA-335 III.4.10 forbids. It runs anyway - the address is read and
// discarded - so the method returns 42.
//
// The body is deliberately kept under INLINE_LENGTH_LIMIT and free of exception
// clauses, which is what makes the interpreter fold it into its caller. An
// inlined body never gets a transform of its own, so it is the one shape that
// can slip past a check placed where a method is transformed.
//
// So the program prints "ran 42" when nothing verifies it, and "rejected
// <exception>" when something does.

.assembly extern mscorlib { }
.assembly 'verification-inlined-il' { }

.class public sealed Box extends [mscorlib]System.Object {
	.field public initonly int32 Value

	.method public specialname rtspecialname instance void .ctor (int32 v) cil managed {
		.maxstack 2
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor()
		ldarg.0
		ldarg.1
		stfld int32 Box::Value
		ret
	}
}

.class public sealed Probe extends [mscorlib]System.Object {

	.method public static int32 Small (class Box b) cil managed {
		.maxstack 2
		ldarg.0
		ldflda int32 Box::Value
		ldind.i4
		ret
	}

	.method public static int32 Main () cil managed {
		.entrypoint
		.maxstack 3
		.locals init (string report)
		.try {
			ldstr "ran "
			ldc.i4 42
			newobj instance void Box::.ctor(int32)
			call int32 Probe::Small (class Box)
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
