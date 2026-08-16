// Dispatch: which method a virtual, interface or delegate call site resolves
// to, and which one a delegate binds when it is made.
//
// Every target is NoInlining and returns a value of its own, so the answer says
// which method ran. Where a test makes more than one call, the results are
// folded into one number, so two targets that exchange places give a different
// total.

using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;
using System.Threading;

public class DispatchSuite {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object IdO (object x) { return x; }

	static int dispatch_bumped;

	// -------------------------------------------------------- many interfaces

	// Twenty interfaces on one class. MONO_IMT_SIZE is 19, so two of them must
	// land on one IMT slot, which the interpreter reads as one method table
	// entry holding a list.
	interface IDispatchW01 { int W (); }
	interface IDispatchW02 { int W (); }
	interface IDispatchW03 { int W (); }
	interface IDispatchW04 { int W (); }
	interface IDispatchW05 { int W (); }
	interface IDispatchW06 { int W (); }
	interface IDispatchW07 { int W (); }
	interface IDispatchW08 { int W (); }
	interface IDispatchW09 { int W (); }
	interface IDispatchW10 { int W (); }
	interface IDispatchW11 { int W (); }
	interface IDispatchW12 { int W (); }
	interface IDispatchW13 { int W (); }
	interface IDispatchW14 { int W (); }
	interface IDispatchW15 { int W (); }
	interface IDispatchW16 { int W (); }
	interface IDispatchW17 { int W (); }
	interface IDispatchW18 { int W (); }
	interface IDispatchW19 { int W (); }
	interface IDispatchW20 { int W (); }

	class DispatchTwenty :
		IDispatchW01, IDispatchW02, IDispatchW03, IDispatchW04, IDispatchW05,
		IDispatchW06, IDispatchW07, IDispatchW08, IDispatchW09, IDispatchW10,
		IDispatchW11, IDispatchW12, IDispatchW13, IDispatchW14, IDispatchW15,
		IDispatchW16, IDispatchW17, IDispatchW18, IDispatchW19, IDispatchW20 {
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW01.W () { return 1; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW02.W () { return 2; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW03.W () { return 3; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW04.W () { return 4; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW05.W () { return 5; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW06.W () { return 6; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW07.W () { return 7; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW08.W () { return 8; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW09.W () { return 9; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW10.W () { return 10; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW11.W () { return 11; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW12.W () { return 12; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW13.W () { return 13; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW14.W () { return 14; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW15.W () { return 15; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW16.W () { return 16; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW17.W () { return 17; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW18.W () { return 18; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW19.W () { return 19; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchW20.W () { return 20; }
	}

	public static int test_210_twenty_interfaces_on_one_receiver ()
	{
		object o = IdO (new DispatchTwenty ());
		int sum = 0;

		sum += ((IDispatchW01) o).W ();
		sum += ((IDispatchW02) o).W ();
		sum += ((IDispatchW03) o).W ();
		sum += ((IDispatchW04) o).W ();
		sum += ((IDispatchW05) o).W ();
		sum += ((IDispatchW06) o).W ();
		sum += ((IDispatchW07) o).W ();
		sum += ((IDispatchW08) o).W ();
		sum += ((IDispatchW09) o).W ();
		sum += ((IDispatchW10) o).W ();
		sum += ((IDispatchW11) o).W ();
		sum += ((IDispatchW12) o).W ();
		sum += ((IDispatchW13) o).W ();
		sum += ((IDispatchW14) o).W ();
		sum += ((IDispatchW15) o).W ();
		sum += ((IDispatchW16) o).W ();
		sum += ((IDispatchW17) o).W ();
		sum += ((IDispatchW18) o).W ();
		sum += ((IDispatchW19) o).W ();
		sum += ((IDispatchW20) o).W ();
		return sum;
	}

	// ------------------------------------------------- one interface, many impls

	interface IDispatchWeight {
		int Weight ();
		void Bump ();
	}

	class DispatchAlpha : IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Weight () { return 1; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { dispatch_bumped = dispatch_bumped * 10 + 1; }
	}

	class DispatchBeta : IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Weight () { return 2; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { dispatch_bumped = dispatch_bumped * 10 + 2; }
	}

	class DispatchGamma : IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Weight () { return 3; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { dispatch_bumped = dispatch_bumped * 10 + 3; }
	}

	class DispatchDelta : IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Weight () { return 4; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { dispatch_bumped = dispatch_bumped * 10 + 4; }
	}

	// One site, four unrelated classes, so its IMT slot holds four targets.
	public static int test_1234_one_interface_site_four_implementations ()
	{
		IDispatchWeight[] all = {
			new DispatchAlpha (), new DispatchBeta (),
			new DispatchGamma (), new DispatchDelta ()
		};
		int sum = 0;

		for (int i = 0; i < all.Length; i++)
			sum = sum * 10 + all [i].Weight ();
		return sum;
	}

	class DispatchReimplBase : IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public virtual int Weight () { return 5; }
		[MethodImpl (MethodImplOptions.NoInlining)] public virtual void Bump () { }
	}

	// The interface slot points at the new method, the vtable slot at the old one.
	class DispatchReimplChild : DispatchReimplBase, IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public new int Weight () { return 6; }
		[MethodImpl (MethodImplOptions.NoInlining)] public new void Bump () { }
	}

	public static int test_65_interface_reimplemented_in_a_derived_class ()
	{
		DispatchReimplChild c = new DispatchReimplChild ();
		return ((IDispatchWeight) c).Weight () * 10 + ((DispatchReimplBase) c).Weight ();
	}

	class DispatchExplicit : IDispatchWeight {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Weight () { return 3; }
		[MethodImpl (MethodImplOptions.NoInlining)] int IDispatchWeight.Weight () { return 9; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { }
	}

	public static int test_93_explicit_interface_implementation ()
	{
		DispatchExplicit e = new DispatchExplicit ();
		return ((IDispatchWeight) e).Weight () * 10 + e.Weight ();
	}

	// ---------------------------------------------------- generic interfaces

	interface IDispatchBox<T> { int Tag (T v); }

	class DispatchBoxes : IDispatchBox<int>, IDispatchBox<string> {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Tag (int v) { return 3; }
		[MethodImpl (MethodImplOptions.NoInlining)] public int Tag (string v) { return 7; }
	}

	public static int test_37_two_instantiations_of_a_generic_interface ()
	{
		object o = IdO (new DispatchBoxes ());
		return ((IDispatchBox<int>) o).Tag (Id (0)) * 10 + ((IDispatchBox<string>) o).Tag (null);
	}

	interface IDispatchPick<T> { int Pick<U> (T a, U b); }

	class DispatchPicker : IDispatchPick<int> {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Pick<U> (int a, U b) { return 8; }
	}

	class DispatchPickerG<T> : IDispatchPick<T> {
		[MethodImpl (MethodImplOptions.NoInlining)] public int Pick<U> (T a, U b) { return 9; }
	}

	public static int test_89_generic_virtual_method_through_a_generic_interface ()
	{
		IDispatchPick<int> p = new DispatchPicker ();
		IDispatchPick<int> q = new DispatchPickerG<int> ();
		return p.Pick<string> (Id (0), null) * 10 + q.Pick<string> (Id (0), null);
	}

	// ------------------------------------------------ generic virtual methods

	class DispatchGvBase<T> {
		[MethodImpl (MethodImplOptions.NoInlining)] public virtual int Pick<U> (T a, U b) { return 1; }
	}

	class DispatchGvDerived<T> : DispatchGvBase<T> {
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Pick<U> (T a, U b) { return 2; }
	}

	class DispatchGvPlain : DispatchGvBase<int> { }

	class DispatchGvInt : DispatchGvBase<int> {
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Pick<U> (int a, U b) { return 3; }
	}

	// The vtable entry belongs to a generic class, so the target is inflated
	// with the class instantiation the entry carries.
	public static int test_22_generic_virtual_method_on_a_generic_class ()
	{
		DispatchGvBase<int> b = new DispatchGvDerived<int> ();
		return b.Pick<string> (Id (0), null) * 10 + b.Pick<int> (Id (0), Id (0));
	}

	public static int test_11_generic_virtual_method_inherited_from_a_generic_base ()
	{
		DispatchGvBase<int> b = new DispatchGvPlain ();
		return b.Pick<string> (Id (0), null) * 10 + b.Pick<int> (Id (0), Id (0));
	}

	public static int test_123_three_receivers_at_one_generic_virtual_site ()
	{
		DispatchGvBase<int>[] all = {
			new DispatchGvPlain (), new DispatchGvDerived<int> (), new DispatchGvInt ()
		};
		int sum = 0;

		for (int i = 0; i < all.Length; i++)
			sum = sum * 10 + all [i].Pick<string> (Id (0), null);
		return sum;
	}

	// --------------------------------------------------------- abstract chain

	abstract class DispatchLevel1 {
		[MethodImpl (MethodImplOptions.NoInlining)] public abstract int Depth ();
		[MethodImpl (MethodImplOptions.NoInlining)] public virtual int Extra () { return 1; }
	}

	abstract class DispatchLevel2 : DispatchLevel1 {
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Extra () { return 2; }
	}

	abstract class DispatchLevel3 : DispatchLevel2 {
		[MethodImpl (MethodImplOptions.NoInlining)] public abstract int Other ();
	}

	class DispatchLevel4 : DispatchLevel3 {
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Depth () { return 4; }
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Other () { return 7; }
	}

	class DispatchLevel5 : DispatchLevel3 {
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Depth () { return 5; }
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Other () { return 8; }
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Extra () { return 6; }
	}

	public static int test_427_abstract_chain_four_deep ()
	{
		DispatchLevel1 a = new DispatchLevel4 ();
		return a.Depth () * 100 + a.Extra () * 10 + ((DispatchLevel3) a).Other ();
	}

	public static int test_568_abstract_chain_with_a_second_override ()
	{
		DispatchLevel1 a = new DispatchLevel5 ();
		return a.Depth () * 100 + a.Extra () * 10 + ((DispatchLevel3) a).Other ();
	}

	// ------------------------------------------------------- boxed value types

	struct DispatchTally : IDispatchWeight {
		public int v;
		[MethodImpl (MethodImplOptions.NoInlining)] public int Weight () { return v; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { }
		[MethodImpl (MethodImplOptions.NoInlining)] public override string ToString () { return "t" + v; }
		[MethodImpl (MethodImplOptions.NoInlining)] public override int GetHashCode () { return v * 3; }
	}

	// The box gets the struct's own slot for a method Object declares, so the
	// call must not land on ValueType.
	public static int test_15_gethashcode_override_on_a_boxed_struct ()
	{
		DispatchTally t = new DispatchTally ();
		t.v = Id (5);
		return IdO (t).GetHashCode ();
	}

	public static int test_2_tostring_override_on_a_boxed_struct ()
	{
		DispatchTally t = new DispatchTally ();
		t.v = Id (6);
		return IdO (t).ToString ().Length;
	}

	public static int test_17_a_struct_and_a_class_at_one_interface_site ()
	{
		DispatchTally t = new DispatchTally ();
		t.v = Id (7);
		IDispatchWeight[] all = { new DispatchAlpha (), t };
		int sum = 0;

		for (int i = 0; i < all.Length; i++)
			sum = sum * 10 + all [i].Weight ();
		return sum;
	}

	// ------------------------------------------------------------ synchronized

	// A synchronized method holds the lock on its receiver, so IsEntered tells a
	// site that went through the wrapper apart from one that skipped it. Without
	// that test both give the same answer.

	class DispatchSyncBase {
		[MethodImpl (MethodImplOptions.Synchronized | MethodImplOptions.NoInlining)]
		public virtual int Locked () { return Monitor.IsEntered (this) ? 5 : 0; }
	}

	class DispatchSyncChild : DispatchSyncBase {
		[MethodImpl (MethodImplOptions.Synchronized | MethodImplOptions.NoInlining)]
		public override int Locked () { return Monitor.IsEntered (this) ? 8 : 0; }
	}

	class DispatchSyncImpl : IDispatchWeight {
		[MethodImpl (MethodImplOptions.Synchronized | MethodImplOptions.NoInlining)]
		public int Weight () { return Monitor.IsEntered (this) ? 6 : 0; }
		[MethodImpl (MethodImplOptions.NoInlining)] public void Bump () { }
	}

	public static int test_6_synchronized_interface_method ()
	{
		IDispatchWeight w = new DispatchSyncImpl ();
		return w.Weight ();
	}

	public static int test_58_synchronized_virtual_method ()
	{
		DispatchSyncBase[] all = { new DispatchSyncBase (), new DispatchSyncChild () };
		int sum = 0;

		for (int i = 0; i < all.Length; i++)
			sum = sum * 10 + all [i].Locked ();
		return sum;
	}

	// ------------------------------------------------------ marshal by ref

	// A call on a marshalbyref class becomes MINT_CALLVIRT, which resolves the
	// target when the call runs instead of reading a slot the transform picked.

	class DispatchRemoteBase : MarshalByRefObject {
		[MethodImpl (MethodImplOptions.NoInlining)] public virtual int Tag () { return 2; }
	}

	class DispatchRemoteChild : DispatchRemoteBase {
		[MethodImpl (MethodImplOptions.NoInlining)] public override int Tag () { return 3; }
	}

	class DispatchRemoteSealed : DispatchRemoteBase {
		[MethodImpl (MethodImplOptions.Synchronized | MethodImplOptions.NoInlining)]
		public sealed override int Tag () { return Monitor.IsEntered (this) ? 7 : 0; }
	}

	class DispatchRemoteHider : DispatchRemoteBase {
		[MethodImpl (MethodImplOptions.Synchronized | MethodImplOptions.NoInlining)]
		public new int Tag () { return Monitor.IsEntered (this) ? 9 : 0; }
	}

	public static int test_23_virtual_call_on_a_marshalbyref_class ()
	{
		DispatchRemoteBase b = new DispatchRemoteBase ();
		DispatchRemoteBase c = new DispatchRemoteChild ();
		return b.Tag () * 10 + c.Tag ();
	}

	// The site names the base method, so the override is found in the vtable and
	// its synchronized wrapper is taken there.
	public static int test_7_synchronized_sealed_override_on_a_marshalbyref_class ()
	{
		DispatchRemoteSealed s = new DispatchRemoteSealed ();
		return s.Tag ();
	}

	// Roslyn emits the token of the least overridden method, so a sealed override
	// still reaches the site as the base's virtual method. A new method that
	// hides the base one puts a non-virtual method at a virtual site.
	public static int test_9_synchronized_hiding_method_on_a_marshalbyref_class ()
	{
		DispatchRemoteHider h = new DispatchRemoteHider ();
		return h.Tag ();
	}

	// -------------------------------------------------------- transparent proxy

	class DispatchProxyTarget : MarshalByRefObject {
		[MethodImpl (MethodImplOptions.NoInlining)] public virtual int Tag () { return 12; }
	}

	class DispatchStubProxy : RealProxy {
		public DispatchStubProxy () : base (typeof (DispatchProxyTarget)) { }

		public override IMessage Invoke (IMessage msg)
		{
			IMethodCallMessage call = (IMethodCallMessage) msg;
			return new ReturnMessage (21, null, 0, call.LogicalCallContext, call);
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static DispatchProxyTarget MakeProxy ()
	{
		return (DispatchProxyTarget) new DispatchStubProxy ().GetTransparentProxy ();
	}

	public static int test_21_virtual_call_on_a_transparent_proxy ()
	{
		return MakeProxy ().Tag ();
	}

	public static int test_141_a_proxy_and_a_real_object_at_one_site ()
	{
		DispatchProxyTarget[] all = { new DispatchProxyTarget (), MakeProxy () };
		int sum = 0;

		for (int i = 0; i < all.Length; i++)
			sum = sum * 10 + all [i].Tag ();
		return sum;
	}

	public static int test_21_delegate_over_a_transparent_proxy_method ()
	{
		DispatchProxyTarget t = MakeProxy ();
		Func<int> f = t.Tag;
		return f ();
	}

	// ---------------------------------------------------------------- delegates

	public static int test_6_delegate_over_a_virtual_method_binds_the_override ()
	{
		DispatchLevel1 a = new DispatchLevel5 ();
		Func<int> f = a.Extra;
		return f ();
	}

	// A call site binds the override, so a delegate over the same method must
	// bind it too. The lock the target takes does not change which target it is.
	public static int test_8_delegate_over_a_synchronized_virtual_method ()
	{
		DispatchSyncBase b = new DispatchSyncChild ();
		Func<int> f = b.Locked;
		return f ();
	}

	// The interface declares Weight without Synchronized, so the transform puts
	// no wrapper in front of the ldvirtftn and the vtable read stands.
	public static int test_6_delegate_over_a_synchronized_interface_method ()
	{
		IDispatchWeight w = new DispatchSyncImpl ();
		Func<int> f = w.Weight;
		return f ();
	}

	public static int test_2_delegate_over_a_generic_virtual_method ()
	{
		DispatchGvBase<int> b = new DispatchGvDerived<int> ();
		Func<int, string, int> f = b.Pick<string>;
		return f (Id (0), null);
	}

	// The receiver is boxed once, when the delegate is made.
	public static int test_9_delegate_over_an_interface_method_of_a_struct ()
	{
		DispatchTally t = new DispatchTally ();
		t.v = Id (9);
		IDispatchWeight w = t;
		Func<int> f = w.Weight;
		t.v = Id (1);
		return f ();
	}

	// Three receivers in one invocation list, each with its own interface slot.
	public static int test_132_multicast_over_three_interface_targets ()
	{
		dispatch_bumped = 0;

		IDispatchWeight a = new DispatchAlpha ();
		IDispatchWeight b = new DispatchBeta ();
		IDispatchWeight c = new DispatchGamma ();
		Action all = a.Bump;

		all += c.Bump;
		all += b.Bump;
		all ();
		return dispatch_bumped;
	}

	public static int test_2_createdelegate_over_a_virtual_method_with_a_target ()
	{
		DispatchGvBase<int> b = new DispatchGvDerived<int> ();
		MethodInfo mi = typeof (DispatchGvBase<int>).GetMethod ("Pick").MakeGenericMethod (typeof (string));
		Func<int, string, int> f = (Func<int, string, int>)
			Delegate.CreateDelegate (typeof (Func<int, string, int>), b, mi);
		return f (Id (0), null);
	}

	public static int test_7_delegate_over_a_synchronized_sealed_override ()
	{
		DispatchRemoteSealed s = new DispatchRemoteSealed ();
		Func<int> f = s.Tag;
		return f ();
	}

	// A dynamic method has no image, so the delegate constructor has to
	// transform it before the call can reach it.
	public static int test_17_delegate_over_a_dynamic_method ()
	{
		DynamicMethod dm = new DynamicMethod (
			"DispatchDynamic", typeof (int), new Type [0], typeof (DispatchSuite));
		ILGenerator il = dm.GetILGenerator ();

		il.Emit (OpCodes.Ldc_I4, 17);
		il.Emit (OpCodes.Ret);

		Func<int> f = (Func<int>) dm.CreateDelegate (typeof (Func<int>));
		return f ();
	}
}
