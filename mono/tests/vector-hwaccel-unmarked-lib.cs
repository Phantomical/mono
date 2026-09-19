/*
 * Negative control: without IntrinsicAttribute, the getter must retain its
 * managed result.
 */

namespace System.Numerics {

	public static class Vector {
		public static bool IsHardwareAccelerated {
			get { return false; }
		}
	}
}
