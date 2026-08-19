//
// Mono.Overrides.MonoOverrideAttribute.cs
//

using System;

namespace Mono.Overrides {
	/// <summary>
	/// Indicates that this method is an override for a named <see cref="Target"/> method.
	/// </summary>
	/// 
	/// <remarks>
	/// The method specified by <see cref="Target"/> will be replaced completely,
	/// as-if the patch method was written in its place. This can be used to replace
	/// any method including generics as long as the parameters and generic parameters
	/// match.
	/// </remarks>
	[AttributeUsage (AttributeTargets.Method, AllowMultiple = true)]
	public sealed class MonoOverrideAttribute : Attribute {
		public MonoOverrideAttribute (string target)
		{
			Target = target;
		}

		/// <summary>
		/// The full name of the method to override. "[namespace.]class:method".
		/// </summary>
		public string Target { get; private set; }
	}
}
