using System;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;

//
// Several transparent proxies cast to the same interface through one cast site. A
// successful cast asks the proxy's CanCastTo () and then gives the object a new vtable
// carrying the interface, so a per-site cast cache that remembers the vtable the object
// arrived with would wave the next proxy through unupgraded - and the interface call
// after it would dispatch through an IMT slot nobody filled.
//

interface IFoo { int Hello (); }

class Target : MarshalByRefObject, IFoo {
	public int Hello () { return 42; }
}

class MyProxy : RealProxy, IRemotingTypeInfo {
	object target;
	bool answer;

	public MyProxy (object target, bool answer) : base (typeof (MarshalByRefObject))
	{
		this.target = target;
		this.answer = answer;
	}

	public string TypeName { get { return "MyProxy"; } set { } }

	public bool CanCastTo (Type t, object o) { return answer; }

	public override IMessage Invoke (IMessage msg)
	{
		IMethodCallMessage call = (IMethodCallMessage) msg;
		object result = call.MethodBase.Invoke (target, call.Args);

		return new ReturnMessage (result, null, 0, null, call);
	}
}

class Test {
	static object NewProxy (bool answer)
	{
		return new MyProxy (new Target (), answer).GetTransparentProxy ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallThroughCast (object o)
	{
		return ((IFoo) o).Hello ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool TestIsFoo (object o)
	{
		return o is IFoo;
	}

	static int Main ()
	{
		/* One castclass site, a fresh proxy every time. */
		for (int i = 0; i < 3; ++i) {
			if (CallThroughCast (NewProxy (true)) != 42)
				return 1 + i;
		}

		/* One isinst site, likewise. */
		for (int i = 0; i < 3; ++i) {
			object proxy = NewProxy (true);

			if (!TestIsFoo (proxy))
				return 10 + i;
			if (CallThroughCast (proxy) != 42)
				return 20 + i;
		}

		/* A refusal must not be remembered either - the next proxy answers for itself. */
		if (TestIsFoo (NewProxy (false)))
			return 30;

		object yes = NewProxy (true);

		if (!TestIsFoo (yes))
			return 31;
		if (CallThroughCast (yes) != 42)
			return 32;

		return 0;
	}
}
