// Calls and field accesses a C# compiler would refuse to emit.
//
// The transform checks what it is allowed to reach and replaces the site with a
// thrower where it is not, rather than emitting the access.

.assembly extern mscorlib { }

.class public auto ansi AccessTarget extends [mscorlib]System.Object
{
	.field private static int32 hidden_field
	.field public static int32 open_field

	.method private static int32 HiddenMethod () cil managed noinlining
	{
		.maxstack 1
		ldc.i4.s 42
		ret
	}

	.method public static int32 OpenMethod () cil managed noinlining
	{
		.maxstack 1
		ldc.i4.s 43
		ret
	}

	.method private static void .cctor () cil managed
	{
		.maxstack 1
		ldc.i4.s 11
		stsfld int32 AccessTarget::hidden_field
		ldc.i4.s 12
		stsfld int32 AccessTarget::open_field
		ret
	}
}

.class public abstract auto ansi AccessAbstract extends [mscorlib]System.Object
{
	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public abstract virtual instance int32 Missing () cil managed { }
}

.class public auto ansi AccessConcrete extends AccessAbstract
{
	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void AccessAbstract::.ctor ()
		ret
	}

	.method public virtual instance int32 Missing () cil managed
	{
		.maxstack 1
		ldc.i4.s 9
		ret
	}
}

.class public auto ansi Access extends [mscorlib]System.Object
{
	// A non-virtual call naming an abstract method.  There is no body to call,
	// so the transform puts a thrower at the site.
	.method public static int32 test_1_call_of_an_abstract_method () cil managed
	{
		.maxstack 2
		.try {
			newobj instance void AccessConcrete::.ctor ()
			call instance int32 AccessAbstract::Missing ()
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.BadImageFormatException {
			pop
			ldc.i4.1
			ret
		}
	}

	.method public static int32 test_43_public_method_is_reached () cil managed
	{
		.maxstack 1
		call int32 AccessTarget::OpenMethod ()
		ret
	}

	.method public static int32 test_12_public_field_is_reached () cil managed
	{
		.maxstack 1
		ldsfld int32 AccessTarget::open_field
		ret
	}

	// A private method of another class.  The transform replaces the site with a
	// thrower rather than emitting the call, so the answer says which happened.
	.method public static int32 test_1_private_method_of_another_class () cil managed
	{
		.maxstack 2
		.try {
			call int32 AccessTarget::HiddenMethod ()
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.MethodAccessException {
			pop
			ldc.i4.1
			ret
		}
	}

	// The access is in a method of its own: the transform raises the refusal
	// while that method is prepared, which is too early for a try around the site
	// itself.
	.method public static int32 ReadsHiddenField () cil managed noinlining
	{
		.maxstack 1
		ldsfld int32 AccessTarget::hidden_field
		ret
	}

	.method public static int32 test_1_private_field_of_another_class () cil managed
	{
		.maxstack 2
		.try {
			call int32 Access::ReadsHiddenField ()
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.FieldAccessException {
			pop
			ldc.i4.1
			ret
		}
	}
}
