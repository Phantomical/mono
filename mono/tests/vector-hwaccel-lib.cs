/*
 * Model an older standalone System.Numerics.Vectors package whose intrinsic
 * getter returns false. The package defines its own IntrinsicAttribute because
 * corlib's type is internal.
 */

namespace System.Runtime.CompilerServices {

	public sealed class IntrinsicAttribute : System.Attribute { }
}

namespace System.Numerics {

	public static class Vector {
		public static bool IsHardwareAccelerated {
			[System.Runtime.CompilerServices.Intrinsic]
			get { return false; }
		}
	}
}
