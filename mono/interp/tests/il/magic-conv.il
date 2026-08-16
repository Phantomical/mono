// The nint, nuint and nfloat "magic" types.
//
// interp_handle_magic_type_intrinsics () replaces a call to one of these types
// with integer or float opcodes instead of calling the managed body.  Which
// opcode it picks depends on the argument width against the pointer width, and
// on whether the source and the destination are both integer types.
//
// A type is magic only when its class is System.nint, System.nuint or
// System.nfloat and the assembly that holds it has one of the names in
// mono_class_is_magic_assembly ().  That name is the Assembly metadata row,
// which csc takes from the output file.  Every `.cs` here compiles into
// interp-tests.dll, so only an IL suite can get a magic name: ilasm takes it
// from the `.assembly` directive below.
//
// The managed bodies are real implementations.  The LLVM backend does not
// replace these calls, so the jit arm runs every body the interpreter skips.  A
// body that disagrees with its opcode shows as a divergence between the arms.

.assembly extern mscorlib { }
.assembly mini_tests { }

.namespace System
{

// ---------------------------------------------------------------------------
// nint
// ---------------------------------------------------------------------------
.class public sequential ansi serializable nint
	extends [mscorlib]System.ValueType
{
	.field assembly int64 v
	.field public static int32 Size

	.method public hidebysig specialname rtspecialname static void .cctor () cil managed
	{
		ldc.i4.8
		stsfld int32 System.nint::Size
		ret
	}

	.method public hidebysig specialname rtspecialname instance void .ctor (int32 x) cil managed
	{
		ldarg.0
		ldarg.1
		conv.i8
		stfld int64 System.nint::v
		ret
	}

	.method public hidebysig specialname rtspecialname instance void .ctor (int64 x) cil managed
	{
		ldarg.0
		ldarg.1
		stfld int64 System.nint::v
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Explicit (int32 x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		conv.i8
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Explicit (int64 x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Explicit (float64 x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		conv.i8
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Explicit (void* x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		conv.i8
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	// A magic float source keeps the managed body as well, through its own arm
	// of the store_value_as_local test.
	.method public hidebysig specialname static valuetype System.nint op_Explicit (valuetype System.nfloat x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		ldfld float64 System.nfloat::v
		conv.i8
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static int32 op_Explicit (valuetype System.nint x) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		conv.i4
		ret
	}

	.method public hidebysig specialname static int64 op_Explicit (valuetype System.nint x) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ret
	}

	// A conversion out of a magic type to a type that is not an integer keeps
	// its managed body.  emit_store_value_as_local () then gives that body the
	// address of the value, not the value.  So these bodies read the parameter
	// with ldarg, which is also correct when a compiled caller passes it by
	// value.  CompareTo, Equals (nint) and the nfloat conversions below take
	// their argument the same way.
	.method public hidebysig specialname static float64 op_Explicit (valuetype System.nint x) cil managed
	{
		ldarg.0
		ldfld int64 System.nint::v
		conv.r8
		ret
	}

	.method public hidebysig specialname static void* op_Explicit (valuetype System.nint x) cil managed
	{
		ldarg.0
		ldfld int64 System.nint::v
		conv.i
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Increment (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldc.i4.1
		conv.i8
		add
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Decrement (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldc.i4.1
		conv.i8
		sub
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_UnaryNegation (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		neg
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_OnesComplement (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		not
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Addition (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		add
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Subtraction (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		sub
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Multiply (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		mul
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Division (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		div
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Modulus (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		rem
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_BitwiseAnd (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		and
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_BitwiseOr (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		or
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_ExclusiveOr (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		xor
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_LeftShift (valuetype System.nint a, int32 amount) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarg.1
		shl
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_RightShift (valuetype System.nint a, int32 amount) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarg.1
		shr
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static bool op_Equality (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_Inequality (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		ceq
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_LessThan (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		clt
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThan (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		cgt
		ret
	}

	.method public hidebysig specialname static bool op_LessThanOrEqual (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		cgt
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThanOrEqual (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		clt
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig instance int32 CompareTo (valuetype System.nint other) cil managed
	{
		ldarg.0
		ldfld int64 System.nint::v
		ldarg.1
		ldfld int64 System.nint::v
		bgt.s ABOVE
		ldarg.0
		ldfld int64 System.nint::v
		ldarg.1
		ldfld int64 System.nint::v
		blt.s BELOW
		ldc.i4.0
		ret
	ABOVE:
		ldc.i4.1
		ret
	BELOW:
		ldc.i4.m1
		ret
	}

	.method public hidebysig instance bool Equals (valuetype System.nint other) cil managed
	{
		ldarg.0
		ldfld int64 System.nint::v
		ldarg.1
		ldfld int64 System.nint::v
		ceq
		ret
	}

	.method public hidebysig virtual instance bool Equals (object o) cil managed
	{
		ldarg.1
		isinst System.nint
		brfalse.s OTHER
		ldarg.0
		ldfld int64 System.nint::v
		ldarg.1
		unbox System.nint
		ldfld int64 System.nint::v
		ceq
		ret
	OTHER:
		ldc.i4.0
		ret
	}

	.method public hidebysig virtual instance int32 GetHashCode () cil managed
	{
		ldarg.0
		ldfld int64 System.nint::v
		conv.i4
		ret
	}

	.method public hidebysig virtual instance string ToString () cil managed
	{
		ldarg.0
		ldflda int64 System.nint::v
		call instance string [mscorlib]System.Int64::ToString()
		ret
	}

	.method public hidebysig static valuetype System.nint Parse (string s) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		call int64 [mscorlib]System.Int64::Parse(string)
		stfld int64 System.nint::v
		ldloc.0
		ret
	}
}

// ---------------------------------------------------------------------------
// nuint
// ---------------------------------------------------------------------------
.class public sequential ansi serializable nuint
	extends [mscorlib]System.ValueType
{
	.field assembly uint64 v

	.method public hidebysig specialname rtspecialname instance void .ctor (uint32 x) cil managed
	{
		ldarg.0
		ldarg.1
		conv.u8
		stfld uint64 System.nuint::v
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_Explicit (uint32 x) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarg.0
		conv.u8
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_Explicit (uint64 x) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarg.0
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_Explicit (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static uint32 op_Explicit (valuetype System.nuint x) cil managed
	{
		ldarga.s 0
		ldfld uint64 System.nuint::v
		conv.u4
		ret
	}

	.method public hidebysig specialname static uint64 op_Explicit (valuetype System.nuint x) cil managed
	{
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_Increment (valuetype System.nuint x) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldc.i4.1
		conv.u8
		add
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_Division (valuetype System.nuint a, valuetype System.nuint b) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarga.s 1
		ldfld uint64 System.nuint::v
		div.un
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_Modulus (valuetype System.nuint a, valuetype System.nuint b) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarga.s 1
		ldfld uint64 System.nuint::v
		rem.un
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nuint op_RightShift (valuetype System.nuint a, int32 amount) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarg.1
		shr.un
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static bool op_Equality (valuetype System.nuint a, valuetype System.nuint b) cil managed
	{
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarga.s 1
		ldfld uint64 System.nuint::v
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThan (valuetype System.nuint a, valuetype System.nuint b) cil managed
	{
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarga.s 1
		ldfld uint64 System.nuint::v
		cgt.un
		ret
	}
}

// ---------------------------------------------------------------------------
// nfloat
// ---------------------------------------------------------------------------
.class public sequential ansi serializable nfloat
	extends [mscorlib]System.ValueType
{
	.field assembly float64 v

	.method public hidebysig specialname rtspecialname instance void .ctor (float32 x) cil managed
	{
		ldarg.0
		ldarg.1
		conv.r8
		stfld float64 System.nfloat::v
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Explicit (float32 x) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarg.0
		conv.r8
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Explicit (float64 x) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarg.0
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Explicit (int32 x) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarg.0
		conv.r8
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Implicit (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarg.0
		ldfld int64 System.nint::v
		conv.r8
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static float32 op_Explicit (valuetype System.nfloat x) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		conv.r4
		ret
	}

	.method public hidebysig specialname static float64 op_Explicit (valuetype System.nfloat x) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ret
	}

	.method public hidebysig specialname static int32 op_Explicit (valuetype System.nfloat x) cil managed
	{
		ldarg.0
		ldfld float64 System.nfloat::v
		conv.i4
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_UnaryPlus (valuetype System.nfloat x) cil managed
	{
		ldarg.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_UnaryNegation (valuetype System.nfloat x) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarga.s 0
		ldfld float64 System.nfloat::v
		neg
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Addition (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		add
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Subtraction (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		sub
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Multiply (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		mul
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Division (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		div
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nfloat op_Modulus (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		.locals init (valuetype System.nfloat r)
		ldloca.s 0
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		rem
		stfld float64 System.nfloat::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static bool op_Equality (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_Inequality (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		ceq
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_LessThan (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		clt
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThan (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		cgt
		ret
	}

	// An unordered pair answers false to both, so the negation of the opposite
	// test is not the same operator.
	.method public hidebysig specialname static bool op_LessThanOrEqual (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		cgt.un
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThanOrEqual (valuetype System.nfloat a, valuetype System.nfloat b) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 1
		ldfld float64 System.nfloat::v
		clt.un
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig static bool IsNaN (valuetype System.nfloat x) cil managed
	{
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ldarga.s 0
		ldfld float64 System.nfloat::v
		ceq
		ldc.i4.0
		ceq
		ret
	}
}

} // .namespace System

// ---------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------
.class public auto ansi MagicConv extends [mscorlib]System.Object
{
	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor()
		ret
	}

	// The transform folds an operation on constants, so every operand goes
	// through one of these first.
	.method public hidebysig static int32 Id4 (int32 x) cil managed noinlining
	{
		ldarg.0
		ret
	}

	.method public hidebysig static int64 Id8 (int64 x) cil managed noinlining
	{
		ldarg.0
		ret
	}

	.method public hidebysig static uint32 IdU4 (uint32 x) cil managed noinlining
	{
		ldarg.0
		ret
	}

	.method public hidebysig static uint64 IdU8 (uint64 x) cil managed noinlining
	{
		ldarg.0
		ret
	}

	.method public hidebysig static float32 IdR4 (float32 x) cil managed noinlining
	{
		ldarg.0
		ret
	}

	.method public hidebysig static float64 IdR8 (float64 x) cil managed noinlining
	{
		ldarg.0
		ret
	}

	// -------------------------------------------------------------------
	// nint: constructors and conversions
	// -------------------------------------------------------------------
	.method public hidebysig static int32 test_7_nint_ctor_from_i4 () cil managed
	{
		.locals init (valuetype System.nint x)
		ldloca.s 0
		ldc.i4.7
		call int32 MagicConv::Id4(int32)
		call instance void System.nint::.ctor(int32)
		ldloc.0
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_1_nint_ctor_from_i4_sign_extends () cil managed
	{
		.locals init (valuetype System.nint x)
		ldloca.s 0
		ldc.i4.m1
		call int32 MagicConv::Id4(int32)
		call instance void System.nint::.ctor(int32)
		ldloc.0
		call int64 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.m1
		conv.i8
		ceq
		ret
	}

	// An int64 argument is already the pointer width, so the .ctor stores it
	// with no conversion.  Reading the value back as an int64 is what shows
	// that all 64 bits landed.
	.method public hidebysig static int32 test_1_nint_ctor_from_i8_keeps_the_high_word () cil managed
	{
		.locals init (valuetype System.nint x)
		ldloca.s 0
		ldc.i8 8589934599
		call int64 MagicConv::Id8(int64)
		call instance void System.nint::.ctor(int64)
		ldloc.0
		call int64 System.nint::op_Explicit(valuetype System.nint)
		ldc.i8 8589934599
		ceq
		ret
	}

	.method public hidebysig static int32 test_9_nint_explicit_from_i4 () cil managed
	{
		ldc.i4.s 9
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	// The widening conversion nint picks is signed; the nuint one below is not.
	.method public hidebysig static int32 test_1_nint_explicit_from_i4_sign_extends () cil managed
	{
		ldc.i4.m1
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call int64 System.nint::op_Explicit(valuetype System.nint)
		ldc.i8 -1
		ceq
		ret
	}

	// The high word is dropped by the narrowing conversion out of nint.
	.method public hidebysig static int32 test_5_nint_explicit_to_i4_truncates () cil managed
	{
		ldc.i8 4294967301
		call int64 MagicConv::Id8(int64)
		call valuetype System.nint System.nint::op_Explicit(int64)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_1_nint_explicit_to_i8 () cil managed
	{
		ldc.i8 8589934595
		call int64 MagicConv::Id8(int64)
		call valuetype System.nint System.nint::op_Explicit(int64)
		call int64 System.nint::op_Explicit(valuetype System.nint)
		ldc.i8 8589934595
		ceq
		ret
	}

	.method public hidebysig static int32 test_1_nint_explicit_to_r8 () cil managed
	{
		ldc.i4.3
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call float64 System.nint::op_Explicit(valuetype System.nint)
		ldc.r8 3.0
		ceq
		ret
	}

	.method public hidebysig static int32 test_3_nint_explicit_from_r8 () cil managed
	{
		ldc.r8 3.9
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nint System.nint::op_Explicit(float64)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_3_nint_explicit_from_nfloat () cil managed
	{
		ldc.r8 3.9
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nint System.nint::op_Explicit(valuetype System.nfloat)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_16_nint_round_trip_through_pointer () cil managed
	{
		ldc.i4.s 16
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call void* System.nint::op_Explicit(valuetype System.nint)
		call valuetype System.nint System.nint::op_Explicit(void*)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	// -------------------------------------------------------------------
	// nint: operators
	// -------------------------------------------------------------------
	.method public hidebysig static int32 test_11_nint_increment () cil managed
	{
		ldc.i4.s 10
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Increment(valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_9_nint_decrement () cil managed
	{
		ldc.i4.s 10
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Decrement(valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_5_nint_unary_negation () cil managed
	{
		ldc.i4.s -5
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_UnaryNegation(valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_7_nint_ones_complement () cil managed
	{
		ldc.i4.s -8
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_OnesComplement(valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_10_nint_addition () cil managed
	{
		ldc.i4.6
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.4
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Addition(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_5_nint_subtraction () cil managed
	{
		ldc.i4.s 9
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.4
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Subtraction(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_12_nint_multiply () cil managed
	{
		ldc.i4.3
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.4
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Multiply(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	// A signed division rounds toward zero, so -20 / 8 is -2 and not -3.
	.method public hidebysig static int32 test_1_nint_division_is_signed () cil managed
	{
		ldc.i4.s -20
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.8
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Division(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.s -2
		ceq
		ret
	}

	.method public hidebysig static int32 test_2_nint_modulus () cil managed
	{
		ldc.i4.s 20
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.6
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nint System.nint::op_Modulus(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_1_nint_bitwise_operators () cil managed
	{
		.locals init (valuetype System.nint a, valuetype System.nint b)
		ldc.i4.s 12
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.0
		ldc.i4.s 10
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.1

		ldloc.0
		ldloc.1
		call valuetype System.nint System.nint::op_BitwiseAnd(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.8
		bne.un.s FAIL

		ldloc.0
		ldloc.1
		call valuetype System.nint System.nint::op_BitwiseOr(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.s 14
		bne.un.s FAIL

		ldloc.0
		ldloc.1
		call valuetype System.nint System.nint::op_ExclusiveOr(valuetype System.nint, valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.6
		bne.un.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// A right shift of a negative value keeps the sign.
	.method public hidebysig static int32 test_1_nint_shift_operators () cil managed
	{
		ldc.i4.1
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.3
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_LeftShift(valuetype System.nint, int32)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.8
		bne.un.s FAIL

		ldc.i4.s -16
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		ldc.i4.2
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_RightShift(valuetype System.nint, int32)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.s -4
		bne.un.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public hidebysig static int32 test_1_nint_equality_operators () cil managed
	{
		.locals init (valuetype System.nint a, valuetype System.nint b)
		ldc.i4.4
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.0
		ldc.i4.5
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.1

		ldloc.0
		ldloc.0
		call bool System.nint::op_Equality(valuetype System.nint, valuetype System.nint)
		brfalse.s FAIL
		ldloc.0
		ldloc.1
		call bool System.nint::op_Equality(valuetype System.nint, valuetype System.nint)
		brtrue.s FAIL
		ldloc.0
		ldloc.1
		call bool System.nint::op_Inequality(valuetype System.nint, valuetype System.nint)
		brfalse.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public hidebysig static int32 test_1_nint_ordering_operators () cil managed
	{
		.locals init (valuetype System.nint a, valuetype System.nint b)
		ldc.i4.s -3
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.0
		ldc.i4.4
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.1

		ldloc.0
		ldloc.1
		call bool System.nint::op_LessThan(valuetype System.nint, valuetype System.nint)
		brfalse.s FAIL
		ldloc.0
		ldloc.1
		call bool System.nint::op_LessThanOrEqual(valuetype System.nint, valuetype System.nint)
		brfalse.s FAIL
		ldloc.1
		ldloc.0
		call bool System.nint::op_GreaterThan(valuetype System.nint, valuetype System.nint)
		brfalse.s FAIL
		ldloc.0
		ldloc.0
		call bool System.nint::op_GreaterThanOrEqual(valuetype System.nint, valuetype System.nint)
		brfalse.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// CompareTo and Equals take the value through a local and its address,
	// which is what the managed body of a magic type expects.
	.method public hidebysig static int32 test_1_nint_compare_to () cil managed
	{
		.locals init (valuetype System.nint a, valuetype System.nint b)
		ldc.i4.5
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.0
		ldc.i4.7
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.1

		ldloca.s 0
		ldloc.1
		call instance int32 System.nint::CompareTo(valuetype System.nint)
		ldc.i4.m1
		bne.un.s FAIL
		ldloca.s 1
		ldloc.0
		call instance int32 System.nint::CompareTo(valuetype System.nint)
		ldc.i4.1
		bne.un.s FAIL
		ldloca.s 0
		ldloc.0
		call instance int32 System.nint::CompareTo(valuetype System.nint)
		brtrue.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public hidebysig static int32 test_1_nint_equals_value_and_object () cil managed
	{
		.locals init (valuetype System.nint a, valuetype System.nint b)
		ldc.i4.5
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.0
		ldc.i4.7
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.1

		ldloca.s 0
		ldloc.0
		call instance bool System.nint::Equals(valuetype System.nint)
		brfalse.s FAIL
		ldloca.s 0
		ldloc.1
		call instance bool System.nint::Equals(valuetype System.nint)
		brtrue.s FAIL
		ldloca.s 0
		ldloc.0
		box System.nint
		call instance bool System.nint::Equals(object)
		brfalse.s FAIL
		ldloca.s 0
		ldloc.1
		box System.nint
		call instance bool System.nint::Equals(object)
		brtrue.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public hidebysig static int32 test_1_nint_string_and_hash () cil managed
	{
		.locals init (valuetype System.nint a)
		ldc.i4.s 42
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		stloc.0

		ldloca.s 0
		call instance string System.nint::ToString()
		ldstr "42"
		call bool [mscorlib]System.String::op_Equality(string, string)
		brfalse.s FAIL
		ldloca.s 0
		call instance int32 System.nint::GetHashCode()
		ldc.i4.s 42
		bne.un.s FAIL
		ldstr "42"
		call valuetype System.nint System.nint::Parse(string)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ldc.i4.s 42
		bne.un.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// C# cannot call a class constructor, and the transform has a branch for it.
	.method public hidebysig static int32 test_8_nint_cctor_called_directly () cil managed
	{
		call void System.nint::.cctor()
		ldsfld int32 System.nint::Size
		ret
	}

	// -------------------------------------------------------------------
	// nuint
	// -------------------------------------------------------------------
	.method public hidebysig static int32 test_1_nuint_ctor_from_u4_zero_extends () cil managed
	{
		.locals init (valuetype System.nuint x)
		ldloca.s 0
		ldc.i4 0xffffffff
		call uint32 MagicConv::IdU4(uint32)
		call instance void System.nuint::.ctor(uint32)
		ldloc.0
		call uint64 System.nuint::op_Explicit(valuetype System.nuint)
		ldc.i8 4294967295
		ceq
		ret
	}

	.method public hidebysig static int32 test_1_nuint_explicit_from_u4_zero_extends () cil managed
	{
		ldc.i4 0xffffffff
		call uint32 MagicConv::IdU4(uint32)
		call valuetype System.nuint System.nuint::op_Explicit(uint32)
		call uint64 System.nuint::op_Explicit(valuetype System.nuint)
		ldc.i8 4294967295
		ceq
		ret
	}

	.method public hidebysig static int32 test_5_nuint_explicit_to_u4_truncates () cil managed
	{
		ldc.i8 4294967301
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call uint32 System.nuint::op_Explicit(valuetype System.nuint)
		ret
	}

	// A signed division would give a negative result for this operand.
	.method public hidebysig static int32 test_1_nuint_division_is_unsigned () cil managed
	{
		ldc.i8 0x8000000000000000
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 2
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call valuetype System.nuint System.nuint::op_Division(valuetype System.nuint, valuetype System.nuint)
		call uint64 System.nuint::op_Explicit(valuetype System.nuint)
		ldc.i8 0x4000000000000000
		ceq
		ret
	}

	.method public hidebysig static int32 test_1_nuint_modulus_is_unsigned () cil managed
	{
		ldc.i8 0x8000000000000001
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 8
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call valuetype System.nuint System.nuint::op_Modulus(valuetype System.nuint, valuetype System.nuint)
		call uint64 System.nuint::op_Explicit(valuetype System.nuint)
		ldc.i8 1
		ceq
		ret
	}

	.method public hidebysig static int32 test_1_nuint_right_shift_is_unsigned () cil managed
	{
		ldc.i8 0x8000000000000000
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i4.4
		call int32 MagicConv::Id4(int32)
		call valuetype System.nuint System.nuint::op_RightShift(valuetype System.nuint, int32)
		call uint64 System.nuint::op_Explicit(valuetype System.nuint)
		ldc.i8 0x0800000000000000
		ceq
		ret
	}

	.method public hidebysig static int32 test_1_nuint_greater_than_is_unsigned () cil managed
	{
		ldc.i8 0x8000000000000000
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 1
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call bool System.nuint::op_GreaterThan(valuetype System.nuint, valuetype System.nuint)
		ret
	}

	.method public hidebysig static int32 test_11_nuint_increment () cil managed
	{
		ldc.i8 10
		call uint64 MagicConv::IdU8(uint64)
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call valuetype System.nuint System.nuint::op_Increment(valuetype System.nuint)
		call uint32 System.nuint::op_Explicit(valuetype System.nuint)
		ret
	}

	.method public hidebysig static int32 test_1_nuint_from_nint () cil managed
	{
		ldc.i4.s -1
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nuint System.nuint::op_Explicit(valuetype System.nint)
		call uint64 System.nuint::op_Explicit(valuetype System.nuint)
		ldc.i8 -1
		ceq
		ret
	}

	// -------------------------------------------------------------------
	// nfloat
	// -------------------------------------------------------------------
	.method public hidebysig static int32 test_7_nfloat_ctor_from_r4 () cil managed
	{
		.locals init (valuetype System.nfloat x)
		ldloca.s 0
		ldc.r4 7.0
		call float32 MagicConv::IdR4(float32)
		call instance void System.nfloat::.ctor(float32)
		ldloc.0
		call int32 System.nfloat::op_Explicit(valuetype System.nfloat)
		ret
	}

	.method public hidebysig static int32 test_1_nfloat_explicit_from_r4 () cil managed
	{
		ldc.r4 0.1
		call float32 MagicConv::IdR4(float32)
		call valuetype System.nfloat System.nfloat::op_Explicit(float32)
		call float64 System.nfloat::op_Explicit(valuetype System.nfloat)
		ldc.r4 0.1
		conv.r8
		ceq
		ret
	}

	// The conversion out of nfloat is a real narrowing, so the value no longer
	// compares equal to the double it came from.
	.method public hidebysig static int32 test_1_nfloat_explicit_to_r4_narrows () cil managed
	{
		ldc.r8 0.1
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call float32 System.nfloat::op_Explicit(valuetype System.nfloat)
		conv.r8
		ldc.r8 0.1
		ceq
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig static int32 test_3_nfloat_explicit_to_i4 () cil managed
	{
		ldc.r8 3.9
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call int32 System.nfloat::op_Explicit(valuetype System.nfloat)
		ret
	}

	.method public hidebysig static int32 test_1_nfloat_implicit_from_nint () cil managed
	{
		ldc.i4.5
		call int32 MagicConv::Id4(int32)
		call valuetype System.nint System.nint::op_Explicit(int32)
		call valuetype System.nfloat System.nfloat::op_Implicit(valuetype System.nint)
		call float64 System.nfloat::op_Explicit(valuetype System.nfloat)
		ldc.r8 5.0
		ceq
		ret
	}

	.method public hidebysig static int32 test_1_nfloat_explicit_from_i4 () cil managed
	{
		ldc.i4.6
		call int32 MagicConv::Id4(int32)
		call valuetype System.nfloat System.nfloat::op_Explicit(int32)
		call float64 System.nfloat::op_Explicit(valuetype System.nfloat)
		ldc.r8 6.0
		ceq
		ret
	}

	// ((10 + 4) * 2 - 3) / 5, then the remainder against 3.
	.method public hidebysig static int32 test_2_nfloat_arithmetic () cil managed
	{
		ldc.r8 10.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		ldc.r8 4.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_Addition(valuetype System.nfloat, valuetype System.nfloat)
		ldc.r8 2.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_Multiply(valuetype System.nfloat, valuetype System.nfloat)
		ldc.r8 3.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_Subtraction(valuetype System.nfloat, valuetype System.nfloat)
		ldc.r8 5.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_Division(valuetype System.nfloat, valuetype System.nfloat)
		ldc.r8 3.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_Modulus(valuetype System.nfloat, valuetype System.nfloat)
		call int32 System.nfloat::op_Explicit(valuetype System.nfloat)
		ret
	}

	.method public hidebysig static int32 test_1_nfloat_comparisons () cil managed
	{
		.locals init (valuetype System.nfloat a, valuetype System.nfloat b)
		ldc.r8 2.5
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		stloc.0
		ldc.r8 3.5
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		stloc.1

		ldloc.0
		ldloc.0
		call bool System.nfloat::op_Equality(valuetype System.nfloat, valuetype System.nfloat)
		brfalse.s FAIL
		ldloc.0
		ldloc.1
		call bool System.nfloat::op_Inequality(valuetype System.nfloat, valuetype System.nfloat)
		brfalse.s FAIL
		ldloc.0
		ldloc.1
		call bool System.nfloat::op_LessThan(valuetype System.nfloat, valuetype System.nfloat)
		brfalse.s FAIL
		ldloc.0
		ldloc.1
		call bool System.nfloat::op_LessThanOrEqual(valuetype System.nfloat, valuetype System.nfloat)
		brfalse.s FAIL
		ldloc.1
		ldloc.0
		call bool System.nfloat::op_GreaterThan(valuetype System.nfloat, valuetype System.nfloat)
		brfalse.s FAIL
		ldloc.0
		ldloc.0
		call bool System.nfloat::op_GreaterThanOrEqual(valuetype System.nfloat, valuetype System.nfloat)
		brfalse.s FAIL

		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public hidebysig static int32 test_1_nfloat_is_nan () cil managed
	{
		.locals init (valuetype System.nfloat a)
		ldc.r8 0.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		stloc.0
		ldloc.0
		ldloc.0
		call valuetype System.nfloat System.nfloat::op_Division(valuetype System.nfloat, valuetype System.nfloat)
		call bool System.nfloat::IsNaN(valuetype System.nfloat)
		brfalse.s FAIL
		ldloc.0
		call bool System.nfloat::IsNaN(valuetype System.nfloat)
		brtrue.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public hidebysig static int32 test_5_nfloat_unary_negation () cil managed
	{
		ldc.r8 -5.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_UnaryNegation(valuetype System.nfloat)
		call int32 System.nfloat::op_Explicit(valuetype System.nfloat)
		ret
	}

	// The unary table gives nfloat a 4-byte move while the value is 8 bytes.
	// The move has one stack slot for both source and destination, and cprop
	// takes it for a whole-value copy, so the lost half never shows.
	.method public hidebysig static int32 test_7_nfloat_unary_plus () cil managed
	{
		ldc.r8 7.0
		call float64 MagicConv::IdR8(float64)
		call valuetype System.nfloat System.nfloat::op_Explicit(float64)
		call valuetype System.nfloat System.nfloat::op_UnaryPlus(valuetype System.nfloat)
		call int32 System.nfloat::op_Explicit(valuetype System.nfloat)
		ret
	}

	// newobj on a magic type, which the transform rewrites the same way it
	// rewrites the call form of the constructor. Every other test here builds
	// its value with `ldloca` and a call, so this is the only site that takes
	// the newobj arm.
	.method public hidebysig static int32 test_9_nint_newobj () cil managed
	{
		ldc.i4.s 9
		call int32 MagicConv::Id4(int32)
		newobj instance void System.nint::.ctor(int32)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public hidebysig static int32 test_5_nuint_newobj () cil managed
	{
		ldc.i4.5
		call uint32 MagicConv::IdU4(uint32)
		newobj instance void System.nuint::.ctor(uint32)
		call uint32 System.nuint::op_Explicit(valuetype System.nuint)
		conv.i4
		ret
	}
}
