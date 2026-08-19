//
// Mono.Overrides.MonoOverrideAssemblyAttribute.cs
//

using System;

namespace Mono.Overrides {

	/// <summary>
	///   Says that this assembly holds method overrides, so that the runtime
	///   reads its methods for <see cref="MonoOverrideAttribute" /> as it
	///   loads. An assembly without this attribute is not read.
	/// </summary>
	/// <remarks>
	///   The runtime matches this attribute by name, so an assembly that
	///   cannot reference this one can declare its own
	///   Mono.Overrides.MonoOverrideAssemblyAttribute instead.
	/// </remarks>
	[AttributeUsage (AttributeTargets.Assembly, AllowMultiple = false)]
	public sealed class MonoOverrideAssemblyAttribute : Attribute {
		public MonoOverrideAssemblyAttribute ()
		{
		}
	}
}
