/*
 * mono-overrides.cs: the assembly the runtime reads out of its own directory.
 *
 * A method carrying [MonoOverride ("namespace.class:method")] replaces the
 * method it names, in whichever assembly that turns out to be and in every
 * loaded copy of it.  An override is always static; where the method it
 * replaces is an instance method, the first parameter receives the receiver.
 *
 * What lives here are compatibility shims for code that patches methods by
 * writing machine code over their entry.  That works for a compiled caller and
 * is invisible to the interpreter, which keeps running the method's own
 * bytecode - so a patch applied that way is applied to half the process.  The
 * shims ask the runtime instead.
 */

using System;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

namespace Mono.Overrides {

/// Names the method that the method carrying this attribute replaces.
///
/// The syntax is mono's own method description: "[namespace.]class:method",
/// with '/' between a nested class and the one holding it, '`n' on a generic
/// class or method, and an optional "(argument, types)" where one name is not
/// enough.  A generic method is named by its definition, and the override is
/// instantiated with each of the target's own type arguments.
[AttributeUsage (AttributeTargets.Method)]
public sealed class MonoOverrideAttribute : Attribute {
	public MonoOverrideAttribute (string target)
	{
		Target = target;
	}

	public string Target { get; private set; }
}

public static class MonoOverride {
	/// Makes replacement run wherever target was called, in both engines.
	///
	/// Each argument is a MonoMethod pointer, which is what HandleOf answers.
	/// A later call on the same target replaces this one. Nothing undoes it.
	[MethodImpl (MethodImplOptions.InternalCall)]
	public static extern void Install (IntPtr target, IntPtr replacement);

	/*
	 * A DynamicMethod has no MethodHandle until its body has been built, and
	 * building it is what mono calls CreateDynMethod. Reached by reflection
	 * because both are private.
	 */
	static readonly MethodInfo create_dyn_method = typeof (DynamicMethod).GetMethod (
		"CreateDynMethod", BindingFlags.NonPublic | BindingFlags.Instance);
	static readonly FieldInfo dyn_method_handle = typeof (DynamicMethod).GetField (
		"mhandle", BindingFlags.NonPublic | BindingFlags.Instance);

	/// The MonoMethod behind a MethodBase, which is what Install takes.
	public static IntPtr HandleOf (MethodBase method)
	{
		DynamicMethod dynamic = method as DynamicMethod;

		if (dynamic != null && create_dyn_method != null && dyn_method_handle != null) {
			create_dyn_method.Invoke (dynamic, new object[0]);
			return ((RuntimeMethodHandle) dyn_method_handle.GetValue (dynamic)).Value;
		}

		return method.MethodHandle.Value;
	}

	/// Install (), taking the two methods rather than their handles.
	public static void Install (MethodBase target, MethodBase replacement)
	{
		Install (HandleOf (target), HandleOf (replacement));
	}
}

/*
 * Harmony patches a method by writing a jump over the bytes at the address
 * RuntimeMethodHandle.GetFunctionPointer () hands back. That address is the
 * method's entry here, so a compiled caller does follow it - but nothing tells
 * the runtime the method was patched, and an interpreted caller goes on
 * running the method's own bytecode. Both shims below hand the same pair of
 * methods to the runtime instead.
 *
 * Neither can be undone, and neither needs to be: HarmonyLib copies the
 * original's IL into each wrapper it builds rather than calling back through a
 * trampoline, and it re-points a method for every patch and unpatch alike.
 */
static class HarmonyShims {
	/// The one place HarmonyLib writes a jump: patch, unpatch, reverse patch
	/// and the struct-return probe all reach it. A null answer means it worked.
	[MonoOverride ("HarmonyLib.Memory:DetourMethod")]
	static string DetourMethod (MethodBase original, MethodBase replacement)
	{
		try {
			MonoOverride.Install (original, replacement);
			return null;
		} catch (Exception e) {
			return e.Message;
		}
	}

	/// MonoMod's own self-test, which runs from a constructor before any of
	/// HarmonyLib has and does not go through Memory.
	///
	/// The probes read what the runtime does with a receiver and a struct
	/// return by detouring a method to one with a deliberately different
	/// signature and looking at what arrives. Letting them run measures this
	/// runtime rather than guessing for it.
	[MonoOverride ("MonoMod.RuntimeDetour.Platforms.DetourRuntimeILPlatform:_HookSelftest")]
	static void HookSelftest (object __instance, MethodInfo from, MethodInfo to)
	{
		MonoOverride.Install (from, to);
	}
}

}
