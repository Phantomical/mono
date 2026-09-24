using System;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;

// A cast to Target replaces the proxy's vtable. The second GetType () must
// therefore reload it.

class Target : MarshalByRefObject {
	public virtual int Hello () { return 42; }
}

class MyProxy : RealProxy, IRemotingTypeInfo {
	object target;

	public MyProxy (object target) : base (typeof (MarshalByRefObject))
	{
		this.target = target;
	}

	public string TypeName { get { return "MyProxy"; } set { } }

	public bool CanCastTo (Type t, object o) { return true; }

	public override IMessage Invoke (IMessage msg)
	{
		IMethodCallMessage call = (IMethodCallMessage) msg;
		object result = call.MethodBase.Invoke (target, call.Args);

		return new ReturnMessage (result, null, 0, null, call);
	}
}

class Test {
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string Types (object o)
	{
		string before = o.GetType ().Name;
		Target t = (Target) o;

		return before + "," + o.GetType ().Name + "," + t.Hello ();
	}

	static int Main ()
	{
		string types = Types (new MyProxy (new Target ()).GetTransparentProxy ());

		if (types != "MarshalByRefObject,Target,42") {
			Console.WriteLine (types);
			return 1;
		}
		return 0;
	}
}
