// ldstr and ldtoken: runtime constants that travel as engine-resolved symbols.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'tokens' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'tokens.dll'

.class public auto ansi beforefieldinit Holder extends [mscorlib]System.Object
{
	.field public int32 X
}

.class public abstract auto ansi sealed beforefieldinit Tokens extends [mscorlib]System.Object
{
	.method public static string Hello () cil managed
	{
		.maxstack 1
		ldstr "hello"
		ret
	}

	.method public static string SameLiteralTwice (bool b) cil managed
	{
		.maxstack 1
		ldarg.0
		brtrue take

		ldstr "again"
		ret

	take:
		ldstr "again"
		ret
	}

	.method public static class [mscorlib]System.Type TypeOf () cil managed
	{
		.maxstack 1
		ldtoken Holder
		call class [mscorlib]System.Type [mscorlib]System.Type::GetTypeFromHandle (valuetype [mscorlib]System.RuntimeTypeHandle)
		ret
	}

	.method public static valuetype [mscorlib]System.RuntimeMethodHandle MethodToken () cil managed
	{
		.maxstack 1
		ldtoken method string Tokens::Hello ()
		ret
	}

	.method public static valuetype [mscorlib]System.RuntimeFieldHandle FieldToken () cil managed
	{
		.maxstack 1
		ldtoken field int32 Holder::X
		ret
	}
}
