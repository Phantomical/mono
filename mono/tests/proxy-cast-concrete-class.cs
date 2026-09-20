using System;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;

//
// proxy-cast-cache.cs covers interface casts. Widget is a concrete class unrelated to
// MarshalByRefObject, so neither isinst nor castclass may accept its transparent proxy,
// regardless of what CanCastTo () returns. The LLVM path must match the classic JIT and
// preserve the normal answers for non-proxy objects.
//

class Widget { }

class MyProxy : RealProxy, IRemotingTypeInfo {
	bool answer;

	public MyProxy (bool answer) : base (typeof (MarshalByRefObject))
	{
		this.answer = answer;
	}

	public string TypeName { get { return "MyProxy"; } set { } }

	public bool CanCastTo (Type t, object o) { return answer; }

	public override IMessage Invoke (IMessage msg)
	{
		throw new NotImplementedException ();
	}
}

class Test {
	static object NewProxy (bool answer)
	{
		return new MyProxy (answer).GetTransparentProxy ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsWidget (object o)
	{
		return o is Widget;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object AsWidget (object o)
	{
		return (Widget) o;
	}

	static int Main ()
	{
		// CanCastTo () does not make a proxy an instance of an unrelated
		// concrete class.
		if (IsWidget (NewProxy (false)))
			return 1;
		if (IsWidget (NewProxy (true)))
			return 2;

		try {
			AsWidget (NewProxy (true));
			return 3;
		} catch (InvalidCastException) {
			// expected
		}

		// The inline test must still handle ordinary objects correctly.
		if (IsWidget (new object ()))
			return 4;
		if (!IsWidget (new Widget ()))
			return 5;
		if (AsWidget (new Widget ()) == null)
			return 6;

		return 0;
	}
}
