// The leftovers of newobj and of remoted field access: the paths that only a
// class the fast newobj refuses, or a transparent proxy, can reach.
//
// MINT_NEWOBJ is the slow allocation opcode. The transform picks it over
// MINT_NEWOBJ_FAST for a marshal-by-reference class, a class with a finalizer
// and a class with weak fields.
//
// Two of its arms have no other way in. It builds the vtable and runs the class
// initializer at execution time, so an initializer that throws raises from
// inside this opcode rather than from INIT_VTABLE. It also swaps the
// constructor for a remoting invoke wrapper when the allocation gives back a
// transparent proxy. A context-bound class allocates that way.
//
// MINT_LDRMFLD, MINT_LDRMFLD_VT, MINT_STRMFLD and MINT_STRMFLD_VT are the
// remoted field opcodes. The transform picks them from the declaring class, so
// a field on a marshal-by-reference class becomes one of them whether or not
// the reference is a proxy. Each has two arms: a plain offset in this object,
// or a message to the remote object. fields.cs holds the first arm. A
// transparent proxy, which is what RealProxy.GetTransparentProxy returns, is
// what takes the second.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// A test that makes more than one check returns the number of checks that hold,
// so a failure says how many of them were good.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting;
using System.Runtime.Remoting.Contexts;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;

public struct OpcodesTailPair {
	public int X, Y;
}

// A value type with a reference in it. The proxy arm of the store boxes the
// value, so the reference has to survive the box and the unbox.
public struct OpcodesTailNamed {
	public string Name;
	public int Tag;
}

public struct OpcodesTailBox<T> {
	public T Item;
	public int Tag;
}

public enum OpcodesTailColour {
	Red, Green, Blue
}

public interface IOpcodesTailAdder {
	int Add (int a, int b);
}

// The class initializer throws, so the allocation never happens and newobj
// raises TypeInitializationException instead. The finalizer is what keeps the
// class off MINT_NEWOBJ_FAST, whose failure arm is a different piece of code.
class OpcodesTailAngryCctor {
	static OpcodesTailAngryCctor ()
	{
		throw new InvalidOperationException ("no");
	}

	~OpcodesTailAngryCctor () { }
}

// The fields the remoted opcodes are read and written through.
public class OpcodesTailRemote : MarshalByRefObject, IOpcodesTailAdder {
	public bool B;
	public char C;
	public sbyte I1;
	public byte U1;
	public short I2;
	public ushort U2;
	public int I4;
	public uint U4;
	public long I8;
	public ulong U8;
	public float R4;
	public double R8;
	public string S;
	public object O;
	public int[] A;
	public IntPtr N;
	public OpcodesTailPair Pair;
	public OpcodesTailNamed Named;
	public OpcodesTailBox<string> Boxed;
	public OpcodesTailColour E;
	public List<int> L;

	public virtual int Twice (int x) { return 2 * x; }

	public virtual OpcodesTailPair Swap (OpcodesTailPair p)
	{
		OpcodesTailPair r;
		r.X = p.Y;
		r.Y = p.X;
		return r;
	}

	public int Add (int a, int b) { return a + b; }
}

// Hands every message to the real object, so a field read or write through the
// proxy gives the value the object holds.
class OpcodesTailForwarder : RealProxy {
	readonly MarshalByRefObject target;

	public OpcodesTailForwarder (MarshalByRefObject t) : base (t.GetType ())
	{
		target = t;
	}

	public override IMessage Invoke (IMessage request)
	{
		return RemotingServices.ExecuteMessage (target, (IMethodCallMessage) request);
	}
}

// Answers every message with an exception, which is how the remoted field
// opcodes are made to take their error arm.
class OpcodesTailRefuser : RealProxy {
	public OpcodesTailRefuser () : base (typeof (OpcodesTailRemote)) { }

	public override IMessage Invoke (IMessage request)
	{
		return new ReturnMessage (new BadImageFormatException ("refused"),
		                          (IMethodCallMessage) request);
	}
}

// A context-bound class is created inside its own context, so newobj itself
// hands back a transparent proxy rather than the object.
[Synchronization]
public class OpcodesTailBound : ContextBoundObject {
	public int V;

	public OpcodesTailBound (int v) { V = v; }

	public virtual int Get () { return V; }
}

public class OpcodesTail {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int I (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long L (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string Str (string x) { return x; }

	static int Ok (bool held) { return held ? 1 : 0; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static OpcodesTailRemote Forwarded ()
	{
		return (OpcodesTailRemote) new OpcodesTailForwarder (new OpcodesTailRemote ())
			.GetTransparentProxy ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static OpcodesTailRemote Refusing ()
	{
		return (OpcodesTailRemote) new OpcodesTailRefuser ().GetTransparentProxy ();
	}

	//
	// MINT_NEWOBJ, the allocation the fast opcodes do not take.
	//

	// classinit.cs reaches the same failure through a static field, which runs
	// the initializer from INIT_VTABLE. This is the newobj route to it.
	public static int test_2_newobj_class_init_throws ()
	{
		try {
			new OpcodesTailAngryCctor ();
		} catch (TypeInitializationException e) {
			return 1 + Ok (e.InnerException is InvalidOperationException);
		}
		return 0;
	}

	public static int test_1_newobj_contextbound_is_a_proxy ()
	{
		OpcodesTailBound b = new OpcodesTailBound (I (4));
		return Ok (RemotingServices.IsTransparentProxy (b));
	}

	// The constructor call has to go to the remoting invoke wrapper newobj put
	// in place of it, so a field the constructor wrote reads back.
	public static int test_4_newobj_contextbound_ran_its_ctor ()
	{
		OpcodesTailBound b = new OpcodesTailBound (I (4));
		return b.Get ();
	}

	//
	// MINT_LDRMFLD and MINT_STRMFLD through a proxy.
	//

	public static int test_5_proxy_field_i4 ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.I4 = I (5);
		return r.I4;
	}

	public static int test_4_proxy_field_narrow ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.B = true;
		r.C = 'q';
		r.I1 = (sbyte) I (-3);
		r.U1 = (byte) I (200);
		return Ok (r.B) + Ok (r.C == 'q') + Ok (r.I1 == -3) + Ok (r.U1 == 200);
	}

	public static int test_4_proxy_field_wide ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.I2 = (short) I (-300);
		r.U2 = (ushort) I (60000);
		r.U4 = (uint) I (-1);
		r.I8 = L (-1234567890123L);
		return Ok (r.I2 == -300) + Ok (r.U2 == 60000)
		     + Ok (r.U4 == 0xffffffffu) + Ok (r.I8 == -1234567890123L);
	}

	public static int test_2_proxy_field_unsigned_long ()
	{
		OpcodesTailRemote r = Forwarded ();

		r.U8 = (ulong) L (-1);
		int wide = Ok (r.U8 == ulong.MaxValue);
		r.U8 = (ulong) L (7);
		return wide + Ok (r.U8 == 7);
	}

	public static int test_2_proxy_field_float ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.R4 = 1.5f;
		r.R8 = 2.25;
		return Ok (r.R4 == 1.5f) + Ok (r.R8 == 2.25);
	}

	public static int test_4_proxy_field_string ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.S = Str ("abcd");
		return r.S.Length;
	}

	public static int test_3_proxy_field_object_and_array ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.O = Str ("xy");
		r.A = new int[] { 1, 2, 3 };
		return Ok (((string) r.O).Length == 2) + Ok (r.A.Length == 3)
		     + Ok (r.A[2] == 3);
	}

	// A null reference has to survive the round trip as a null, not as an
	// exception on the way out.
	public static int test_2_proxy_field_null_reference ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.S = Str ("something");
		r.S = null;
		return Ok (r.S == null) + Ok (r.O == null);
	}

	public static int test_9_proxy_field_native_int ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.N = new IntPtr (I (9));
		return (int) r.N;
	}

	// The field type is an enum, so the read goes round stackval_from_data ()
	// once more on the underlying type.
	public static int test_2_proxy_field_enum ()
	{
		OpcodesTailRemote r = Forwarded ();
		r.E = OpcodesTailColour.Blue;
		return (int) r.E;
	}

	public static int test_2_proxy_field_generic_class ()
	{
		OpcodesTailRemote r = Forwarded ();
		List<int> l = new List<int> ();
		l.Add (I (1));
		l.Add (I (2));
		r.L = l;
		return r.L.Count;
	}

	//
	// MINT_LDRMFLD_VT and MINT_STRMFLD_VT through a proxy.
	//

	public static int test_3_proxy_field_vt ()
	{
		OpcodesTailRemote r = Forwarded ();
		OpcodesTailPair p;
		p.X = I (1);
		p.Y = I (2);
		r.Pair = p;
		return r.Pair.X + r.Pair.Y;
	}

	// The value goes over as a box, so the second store has to replace the
	// first rather than land beside it.
	public static int test_7_proxy_field_vt_second_store_wins ()
	{
		OpcodesTailRemote r = Forwarded ();
		OpcodesTailPair p;
		p.X = I (1);
		p.Y = I (2);
		r.Pair = p;
		p.X = I (3);
		p.Y = I (4);
		r.Pair = p;
		return r.Pair.X + r.Pair.Y;
	}

	public static int test_5_proxy_field_vt_with_a_reference ()
	{
		OpcodesTailRemote r = Forwarded ();
		OpcodesTailNamed n;
		n.Name = Str ("abcd");
		n.Tag = I (1);
		r.Named = n;
		return r.Named.Name.Length + r.Named.Tag;
	}

	public static int test_4_proxy_field_generic_struct ()
	{
		OpcodesTailRemote r = Forwarded ();
		OpcodesTailBox<string> b;
		b.Item = Str ("xyz");
		b.Tag = I (1);
		r.Boxed = b;
		return r.Boxed.Item.Length + r.Boxed.Tag;
	}

	//
	// The error arm of each remoted field opcode. The proxy answers with an
	// exception, and the opcode has to raise it here.
	//

	public static int test_1_proxy_read_that_fails ()
	{
		OpcodesTailRemote r = Refusing ();
		try {
			return r.I4;
		} catch (BadImageFormatException) {
			return 1;
		}
	}

	public static int test_1_proxy_read_vt_that_fails ()
	{
		OpcodesTailRemote r = Refusing ();
		try {
			return r.Pair.X;
		} catch (BadImageFormatException) {
			return 1;
		}
	}

	// The next two are a live divergence, and both fail on the interpreted arm.
	// A read raises out of the opcode's own MonoError, so it lands in the catch
	// here. A store instead reaches the managed TransparentProxy.StoreRemoteField
	// through mono_runtime_invoke_checked (), and that unwind passes the
	// interpreted frame. The catch never runs, and neither does a finally put in
	// its place. Until this is fixed the exception is gone before MINT_STRMFLD
	// and MINT_STRMFLD_VT can see an error, so their THROW_EX stays uncovered.

	public static int test_1_proxy_write_that_fails ()
	{
		OpcodesTailRemote r = Refusing ();
		try {
			r.I4 = I (5);
		} catch (BadImageFormatException) {
			return 1;
		}
		return 0;
	}

	public static int test_1_proxy_write_vt_that_fails ()
	{
		OpcodesTailRemote r = Refusing ();
		OpcodesTailPair p;
		p.X = I (1);
		p.Y = I (2);
		try {
			r.Pair = p;
		} catch (BadImageFormatException) {
			return 1;
		}
		return 0;
	}

	//
	// Calls on a proxy. dispatch.cs holds the plain virtual call, so these are
	// the two shapes it does not carry and the failing one.
	//

	public static int test_3_proxy_interface_call ()
	{
		IOpcodesTailAdder a = Forwarded ();
		return a.Add (I (1), I (2));
	}

	public static int test_3_proxy_call_returning_a_struct ()
	{
		OpcodesTailRemote r = Forwarded ();
		OpcodesTailPair p;
		p.X = I (1);
		p.Y = I (2);
		OpcodesTailPair q = r.Swap (p);
		return Ok (q.X == 2) + Ok (q.Y == 1) + Ok (p.X == 1);
	}

	public static int test_1_proxy_call_that_fails ()
	{
		OpcodesTailRemote r = Refusing ();
		try {
			r.Twice (I (4));
		} catch (BadImageFormatException) {
			return 1;
		}
		return 0;
	}
}
