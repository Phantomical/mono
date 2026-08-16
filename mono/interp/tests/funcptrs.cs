// ldftn and ldvirtftn: the method pointer a delegate is built from.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Operands go through the NoInlining Id helpers, so the transform cannot fold a
// test into its answer.
//
// delegates.cs has the plain shapes. The targets here are an explicit interface
// implementation, a synchronized method, a variant interface, a method of a
// generic class, and a generic virtual method whose override lives in a generic
// class. Delegate.CreateDelegate binds the same shapes in the runtime instead of
// in the bytecode.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class FuncPtrs {

	delegate int FpIntFunc (int x);
	delegate int FpNoArgs ();
	delegate int FpOfNamed (IFpNamed n);
	delegate int FpOfSealed (FpSealed s);
	delegate int FpOfString (string s);
	delegate object FpAnyFunc ();

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdO (object o) { return o; }

	// A number for each type argument, so a result shows which instantiation ran.
	static int Mark<T> ()
	{
		if (typeof (T) == typeof (int))
			return 1;
		if (typeof (T) == typeof (string))
			return 2;
		return 3;
	}

	static int Marked<T> (int x) { return Mark<T> () + x; }

	class FpBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Rank () { return 1; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Plain () { return 20; }

		[MethodImpl (MethodImplOptions.Synchronized)]
		public virtual int Locked () { return 30; }
	}

	class FpMiddle : FpBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Rank () { return 2; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Locked () { return 41; }
	}

	class FpSealed : FpBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public sealed override int Rank () { return 3; }

		[MethodImpl (MethodImplOptions.Synchronized)]
		public sealed override int Locked () { return 31; }
	}

	interface IFpNamed {
		int Size ();
	}

	class FpNamedNine : IFpNamed {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Size () { return 9; }
	}

	class FpNamedExplicit : IFpNamed {
		[MethodImpl (MethodImplOptions.NoInlining)]
		int IFpNamed.Size () { return 11; }
	}

	struct FpCell : IFpNamed {
		public int Value;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Size () { return Value + 1; }
	}

	// The receiver implements IFpSource<string>, so a call through
	// IFpSource<object> finds the slot by variance rather than by an exact
	// interface match.
	interface IFpSource<out T> {
		T Get ();
	}

	class FpStringSource : IFpSource<string> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public string Get () { return "abcd"; }
	}

	class FpBox<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Tag () { return Mark<T> () + 50; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int VTag () { return Mark<T> () + 60; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Pick<U> () { return Mark<T> () * 10 + Mark<U> (); }
	}

	class FpBoxDerived<T> : FpBox<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int VTag () { return Mark<T> () + 70; }
	}

	struct FpPair<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Both () { return Mark<T> () + 200; }
	}

	// The override lives in a generic class, so the method the receiver's vtable
	// holds belongs to a generic instance. generics.cs puts every such override
	// in a non-generic class instead.
	class FpRankBase<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Rank<U> (T t) { return Mark<U> () + 80; }
	}

	class FpRankDerived<T> : FpRankBase<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Rank<U> (T t) { return Mark<U> () + 90; }
	}

	abstract class FpMeterBase<T> {
		public abstract int Kind ();
		public abstract int Meter<U> (T t);
	}

	class FpMeterOf<T> : FpMeterBase<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Kind () { return 55; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Meter<U> (T t) { return Mark<U> () + 100; }
	}

	// ---------------------------------------------------------------------
	// ldftn and ldvirtftn from C#
	// ---------------------------------------------------------------------

	public static int test_11_explicit_interface_target ()
	{
		IFpNamed n = new FpNamedExplicit ();
		FpNoArgs d = n.Size;
		return d ();
	}

	public static int test_5_interface_target_on_a_struct ()
	{
		IFpNamed n = new FpCell { Value = Id (4) };
		FpNoArgs d = n.Size;
		return d ();
	}

	// The transform replaces the method with its synchronized wrapper before it
	// emits the ldvirtftn. A wrapper is neither virtual nor in a vtable slot, so
	// get_virtual_method () hands it straight back and the receiver selects
	// nothing. The base body runs whatever the receiver is. The interpreter
	// answers 30 to all three of these, which is right only for the first. The
	// JIT is right for all three.
	public static int test_30_synchronized_virtual_target ()
	{
		FpBase b = new FpBase ();
		FpNoArgs d = b.Locked;
		return d ();
	}

	public static int test_31_synchronized_sealed_override_target ()
	{
		FpSealed s = new FpSealed ();
		FpNoArgs d = s.Locked;
		return d ();
	}

	// This override is not synchronized and has no wrapper of its own. The
	// delegate gets a wrapper of the base method instead.
	public static int test_41_plain_override_of_a_synchronized_base ()
	{
		FpBase b = new FpMiddle ();
		FpNoArgs d = b.Locked;
		return d ();
	}

	// The call site keeps the method virtual and wraps whatever the vtable
	// answers with, so this one is right on both engines.
	public static int test_31_synchronized_sealed_override_call ()
	{
		FpSealed s = (FpSealed) IdO (new FpSealed ());
		return s.Locked ();
	}

	public static int test_52_instance_method_of_a_generic_class_target ()
	{
		FpBox<string> b = new FpBox<string> ();
		FpNoArgs d = b.Tag;
		return d ();
	}

	public static int test_72_virtual_method_of_a_generic_class_target ()
	{
		FpBox<string> b = new FpBoxDerived<string> ();
		FpNoArgs d = b.VTag;
		return d ();
	}

	public static int test_61_virtual_method_of_a_generic_class_over_a_struct ()
	{
		FpBox<int> b = new FpBox<int> ();
		FpNoArgs d = b.VTag;
		return d ();
	}

	public static int test_21_generic_method_of_a_generic_class_target ()
	{
		FpBox<string> b = new FpBox<string> ();
		FpNoArgs d = b.Pick<int>;
		return d ();
	}

	public static int test_201_method_of_a_generic_struct_target ()
	{
		FpPair<int> p = new FpPair<int> ();
		FpNoArgs d = p.Both;
		return d ();
	}

	public static int test_92_generic_virtual_of_a_generic_class_target ()
	{
		FpRankBase<int> h = new FpRankDerived<int> ();
		FpIntFunc d = h.Rank<string>;
		return d (Id (3));
	}

	public static int test_92_generic_virtual_of_a_generic_class_call ()
	{
		FpRankBase<int> h = new FpRankDerived<int> ();
		return h.Rank<string> (Id (3));
	}

	public static int test_4_variant_interface_target ()
	{
		IFpSource<object> s = new FpStringSource ();
		FpAnyFunc d = s.Get;
		return ((string) d ()).Length;
	}

	// ---------------------------------------------------------------------
	// Delegate.CreateDelegate, which binds the target in the runtime rather
	// than in the bytecode
	// ---------------------------------------------------------------------

	public static int test_9_create_delegate_over_an_interface_method ()
	{
		MethodInfo m = typeof (IFpNamed).GetMethod ("Size");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpNamedNine (), m);
		return d ();
	}

	public static int test_11_create_delegate_over_an_explicit_implementation ()
	{
		MethodInfo m = typeof (IFpNamed).GetMethod ("Size");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpNamedExplicit (), m);
		return d ();
	}

	public static int test_5_create_delegate_over_an_interface_method_on_a_struct ()
	{
		MethodInfo m = typeof (IFpNamed).GetMethod ("Size");
		object target = new FpCell { Value = Id (4) };
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), target, m);
		return d ();
	}

	public static int test_20_create_delegate_over_an_open_interface_method ()
	{
		// No target, so the receiver arrives as the argument and each call
		// resolves Size again.
		MethodInfo m = typeof (IFpNamed).GetMethod ("Size");
		FpOfNamed d = (FpOfNamed) Delegate.CreateDelegate (typeof (FpOfNamed), null, m);
		return d (new FpNamedNine ()) + d (new FpNamedExplicit ());
	}

	// A final method has no vtable lookup to do, so the receiver only decides
	// that the wrapper is needed at all.
	public static int test_31_open_delegate_over_a_synchronized_final_override ()
	{
		MethodInfo m = typeof (FpSealed).GetMethod ("Locked");
		FpOfSealed d = (FpOfSealed) Delegate.CreateDelegate (typeof (FpOfSealed), null, m);
		return d (new FpSealed ());
	}

	public static int test_55_create_delegate_over_an_abstract_method_of_a_generic_class ()
	{
		MethodInfo m = typeof (FpMeterBase<int>).GetMethod ("Kind");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpMeterOf<int> (), m);
		return d ();
	}

	public static int test_102_create_delegate_over_an_abstract_generic_method ()
	{
		MethodInfo m = typeof (FpMeterBase<int>).GetMethod ("Meter").MakeGenericMethod (typeof (string));
		FpIntFunc d = (FpIntFunc) Delegate.CreateDelegate (typeof (FpIntFunc), new FpMeterOf<int> (), m);
		return d (Id (3));
	}

	public static int test_92_create_delegate_over_a_generic_virtual_method ()
	{
		MethodInfo m = typeof (FpRankBase<int>).GetMethod ("Rank").MakeGenericMethod (typeof (string));
		FpIntFunc d = (FpIntFunc) Delegate.CreateDelegate (typeof (FpIntFunc), new FpRankDerived<int> (), m);
		return d (Id (3));
	}

	public static int test_20_create_delegate_over_a_non_virtual_method ()
	{
		MethodInfo m = typeof (FpBase).GetMethod ("Plain");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpMiddle (), m);
		return d ();
	}

	public static int test_3_create_delegate_over_a_sealed_override ()
	{
		MethodInfo m = typeof (FpSealed).GetMethod ("Rank");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpSealed (), m);
		return d ();
	}

	public static int test_2_create_delegate_over_a_virtual_method_binds_the_override ()
	{
		MethodInfo m = typeof (FpBase).GetMethod ("Rank");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpMiddle (), m);
		return d ();
	}

	public static int test_5_create_delegate_over_a_static_generic_method ()
	{
		MethodInfo m = typeof (FuncPtrs).GetMethod ("Marked", BindingFlags.Static | BindingFlags.NonPublic)
			.MakeGenericMethod (typeof (string));
		FpIntFunc d = (FpIntFunc) Delegate.CreateDelegate (typeof (FpIntFunc), null, m);
		return d (Id (3));
	}

	public static int test_51_create_delegate_over_a_method_of_a_generic_class ()
	{
		MethodInfo m = typeof (FpBox<int>).GetMethod ("Tag");
		FpNoArgs d = (FpNoArgs) Delegate.CreateDelegate (typeof (FpNoArgs), new FpBox<int> (), m);
		return d ();
	}

	public static int test_1_create_delegate_with_a_wrong_signature_gives_null ()
	{
		MethodInfo m = typeof (FpBase).GetMethod ("Plain");
		Delegate d = Delegate.CreateDelegate (typeof (FpOfString), null, m, false);
		return d == null ? 1 : 0;
	}

	// ---------------------------------------------------------------------
	// Method pointers as values
	// ---------------------------------------------------------------------

	public static int test_1_method_handle_gives_an_entry_point ()
	{
		MethodInfo m = typeof (FpBase).GetMethod ("Plain");
		return m.MethodHandle.GetFunctionPointer () != IntPtr.Zero ? 1 : 0;
	}

	public static int test_1_one_method_has_one_entry_point ()
	{
		MethodInfo m = typeof (FpBase).GetMethod ("Plain");
		IntPtr first = m.MethodHandle.GetFunctionPointer ();
		IntPtr second = m.MethodHandle.GetFunctionPointer ();
		return first == second ? 1 : 0;
	}

	public static int test_1_a_delegates_entry_point_is_its_targets ()
	{
		FpBase b = new FpBase ();
		FpNoArgs d = b.Plain;
		IntPtr fromDelegate = d.Method.MethodHandle.GetFunctionPointer ();
		IntPtr fromReflection = typeof (FpBase).GetMethod ("Plain").MethodHandle.GetFunctionPointer ();
		return fromDelegate == fromReflection ? 1 : 0;
	}

	public static int test_5_delegate_built_from_an_entry_point ()
	{
		// The delegate gets an address and nothing else. The runtime answers with
		// a native wrapper over it, so the call leaves managed code and comes
		// back.
		MethodInfo m = typeof (FuncPtrs).GetMethod ("Marked", BindingFlags.Static | BindingFlags.NonPublic)
			.MakeGenericMethod (typeof (string));
		IntPtr entry = m.MethodHandle.GetFunctionPointer ();
		FpIntFunc d = (FpIntFunc) Marshal.GetDelegateForFunctionPointer (entry, typeof (FpIntFunc));
		return d (Id (3));
	}
}
