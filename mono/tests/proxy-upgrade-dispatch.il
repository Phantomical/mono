// A proxy upgrade in Upgrade () adds IFoo to the vtable. Dispatch must reload
// the vtable before its subsequent interface call.

.assembly extern mscorlib { }
.assembly 'proxy-upgrade-dispatch' { }

.class interface private auto ansi abstract IFoo
{
	.method public virtual hidebysig newslot abstract instance int32 Hello () cil managed
	{
	}
}

.class private auto ansi beforefieldinit Target
	extends [mscorlib]System.MarshalByRefObject
	implements IFoo
{
	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.MarshalByRefObject::.ctor ()
		ret
	}

	.method public final virtual hidebysig newslot instance int32 Hello () cil managed
	{
		.maxstack 1
		ldc.i4.s 42
		ret
	}
}

.class private auto ansi beforefieldinit MyProxy
	extends [mscorlib]System.Runtime.Remoting.Proxies.RealProxy
	implements [mscorlib]System.Runtime.Remoting.IRemotingTypeInfo
{
	.field private object target

	.method public hidebysig specialname rtspecialname instance void .ctor (object target) cil managed
	{
		.maxstack 2
		ldarg.0
		ldtoken [mscorlib]System.MarshalByRefObject
		call class [mscorlib]System.Type [mscorlib]System.Type::GetTypeFromHandle (valuetype [mscorlib]System.RuntimeTypeHandle)
		call instance void [mscorlib]System.Runtime.Remoting.Proxies.RealProxy::.ctor (class [mscorlib]System.Type)
		ldarg.0
		ldarg.1
		stfld object MyProxy::target
		ret
	}

	.method public final virtual hidebysig newslot specialname instance string get_TypeName () cil managed
	{
		.maxstack 1
		ldstr "MyProxy"
		ret
	}

	.method public final virtual hidebysig newslot specialname instance void set_TypeName (string 'value') cil managed
	{
		.maxstack 0
		ret
	}

	.method public final virtual hidebysig newslot instance bool CanCastTo (class [mscorlib]System.Type t, object o) cil managed
	{
		.maxstack 1
		ldc.i4.1
		ret
	}

	.method public virtual hidebysig instance class [mscorlib]System.Runtime.Remoting.Messaging.IMessage Invoke (class [mscorlib]System.Runtime.Remoting.Messaging.IMessage msg) cil managed
	{
		.maxstack 5
		.locals init (class [mscorlib]System.Runtime.Remoting.Messaging.IMethodCallMessage message)
		ldarg.1
		castclass [mscorlib]System.Runtime.Remoting.Messaging.IMethodCallMessage
		stloc.0
		ldloc.0
		callvirt instance class [mscorlib]System.Reflection.MethodBase [mscorlib]System.Runtime.Remoting.Messaging.IMethodMessage::get_MethodBase ()
		ldarg.0
		ldfld object MyProxy::target
		ldloc.0
		callvirt instance object[] [mscorlib]System.Runtime.Remoting.Messaging.IMethodMessage::get_Args ()
		callvirt instance object [mscorlib]System.Reflection.MethodBase::Invoke (object, object[])
		ldnull
		ldc.i4.0
		ldnull
		ldloc.0
		newobj instance void [mscorlib]System.Runtime.Remoting.Messaging.ReturnMessage::.ctor (object, object[], int32, class [mscorlib]System.Runtime.Remoting.Messaging.LogicalCallContext, class [mscorlib]System.Runtime.Remoting.Messaging.IMethodCallMessage)
		ret
	}

	.property instance string TypeName ()
	{
		.get instance string MyProxy::get_TypeName ()
		.set instance void MyProxy::set_TypeName (string)
	}
}

.class private auto ansi beforefieldinit Test
	extends [mscorlib]System.Object
{
	.method private static hidebysig bool Upgrade (object o) cil managed noinlining
	{
		.maxstack 2
		ldarg.0
		isinst IFoo
		ldnull
		cgt.un
		ret
	}

	.method private static hidebysig int32 Dispatch (object o) cil managed noinlining
	{
		.maxstack 2
		.locals init (int32 length)
		ldarg.0
		callvirt instance string [mscorlib]System.Object::ToString ()
		callvirt instance int32 [mscorlib]System.String::get_Length ()
		stloc.0
		ldarg.0
		call bool Test::Upgrade (object)
		pop
		ldloc.0
		ldarg.0
		callvirt instance int32 IFoo::Hello ()
		add
		ret
	}

	.method private static hidebysig int32 Main () cil managed
	{
		.entrypoint
		.maxstack 3
		.locals init (object proxy, string name)
		newobj instance void Target::.ctor ()
		newobj instance void MyProxy::.ctor (object)
		callvirt instance object [mscorlib]System.Runtime.Remoting.Proxies.RealProxy::GetTransparentProxy ()
		stloc.0
		ldloc.0
		callvirt instance string [mscorlib]System.Object::ToString ()
		stloc.1
		ldloc.0
		call int32 Test::Dispatch (object)
		ldloc.1
		callvirt instance int32 [mscorlib]System.String::get_Length ()
		ldc.i4.s 42
		add
		beq.s ok
		ldc.i4.1
		ret
	ok:
		ldc.i4.0
		ret
	}
}
