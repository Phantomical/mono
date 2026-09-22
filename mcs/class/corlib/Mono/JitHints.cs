namespace Mono {
	internal static class JitHints {
		// Lowered to llvm.assume by the LLVM backend; otherwise this is a no-op.
		internal static void Assume (bool condition)
		{
		}
	}
}
