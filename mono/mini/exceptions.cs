using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Regression tests for the mono JIT.
 *
 * Each test needs to be of the form:
 *
 * public static int test_<result>_<name> ();
 *
 * where <result> is an integer (the value that needs to be returned by
 * the method to make it pass.
 * <name> is a user-displayed name used to identify the test.
 *
 * The tests can be driven in two ways:
 * *) running the program directly: Main() uses reflection to find and invoke
 * 	the test methods (this is useful mostly to check that the tests are correct)
 * *) with the --regression switch of the jit (this is the preferred way since
 * 	all the tests will be run with optimizations on and off)
 *
 * The reflection logic could be moved to a .dll since we need at least another
 * regression test file written in IL code to have better control on how
 * the IL code looks.
 */

#if __MOBILE__
class ExceptionTests
#else
class Tests
#endif
{

#if !__MOBILE__
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (Tests), args);
	}
#endif

	public static int test_0_catch () {
		Exception x = new Exception ();
		
		try {
			throw x;
		} catch (Exception e) {
			if (e == x)
				return 0;
		}
		return 1;
	}

	public static int test_0_finally_without_exc () {
		int x;
		
		try {
			x = 1;
		} catch (Exception e) {
			x = 2;
		} finally {
			x = 0;
		}
		
		return x;
	}

	public static int test_0_finally () {
		int x = 1;
		
		try {
			throw new Exception ();
		} catch (Exception e) {
			x = 2;
		} finally {
			x = 0;
		}
		return x;
	}

	public static int test_0_nested_finally () {
		int a;

		try {
			a = 1;
		} finally {
			try {
				a = 2;
			} finally {
				a = 0;
			}
		}
		return a;
	}

	/*
	 * EH F2 functional coverage: STANDALONE try/finally through the LLVM tier.
	 *
	 * The finally-bearing helpers are deliberately kept in their own [NoInlining]
	 * methods so each holds a single, non-nested finally clause - the minimal shape
	 * the F2 slice first exercised. The exception that runs the finally on the
	 * exceptional path propagates OUT of the standalone-finally method into a
	 * SEPARATE standalone try/catch here, keeping both methods a single clause each.
	 * (Depth-2 nesting is itself admitted since EH N3; these stay flat only to keep
	 * the F2 coverage focused.)
	 */

	static int llvm_f2_finally_ex_log;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void llvm_f2_finally_thrower () {
		try {
			llvm_f2_finally_ex_log = 1;
			throw new Exception ("f2");
		} finally {
			llvm_f2_finally_ex_log = llvm_f2_finally_ex_log + 10;
		}
	}

	/* finally runs on EXCEPTIONAL unwind, then the exception resumes to an outer catch. */
	public static int test_0_llvm_finally_exceptional () {
		llvm_f2_finally_ex_log = 0;
		try {
			llvm_f2_finally_thrower ();
		} catch (Exception) {
			/* the finally must have run (1 + 10) before the throw reached here */
			return llvm_f2_finally_ex_log == 11 ? 0 : 1;
		}
		return 2;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f2_finally_leave (int[] log) {
		try {
			log [0] = 1;
			return 5;		/* leave out of the try: runs the finally, then returns */
		} finally {
			log [1] = 1;
		}
	}

	/* finally runs on NORMAL exit via leave. */
	public static int test_0_llvm_finally_normal_leave () {
		int[] log = new int [2];
		int r = llvm_f2_finally_leave (log);
		return (r == 5 && log [0] == 1 && log [1] == 1) ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f2_finally_multi (int sel, int[] log) {
		try {
			if (sel == 0)
				goto L0;
			if (sel == 1)
				goto L1;
			goto L2;
		} finally {
			log [0] = log [0] + 1;
		}
	L0: return 100;
	L1: return 200;
	L2: return 300;
	}

	/* leave to MULTIPLE distinct targets: each runs the finally then resumes to the right continuation. */
	public static int test_0_llvm_finally_multi_target () {
		int[] log = new int [1];
		int r0 = llvm_f2_finally_multi (0, log);
		int r1 = llvm_f2_finally_multi (1, log);
		int r2 = llvm_f2_finally_multi (2, log);
		return (r0 == 100 && r1 == 200 && r2 == 300 && log [0] == 3) ? 0 : 1;
	}

	/*
	 * EH F3 functional coverage: LEAVE-to-multiple-targets breadth + nested-leave
	 * continuations over the SAME standalone-finally indicator/switch machinery F2
	 * activated. Every helper holds a single, non-nested finally clause (so the F2
	 * nesting gate still admits it and each compiles through the LLVM tier), is
	 * [NoInlining] so the clause structure survives, and records the finally-run
	 * count in log [0]; each driver resets the log per call and asserts the finally
	 * ran EXACTLY once per exit AND that the exact per-path continuation ran. These
	 * exercise the OP_CALL_HANDLER distinct-indicator / OP_ENDFINALLY switch scheme
	 * on harder leave shapes than F2's three targets (doc 16 3.4).
	 */

	/*
	 * Shape 1: MANY distinct leave targets (six) from one try/finally. The if/goto
	 * ladder inside the try leaves to six different continuations L0..L5, each of
	 * which is its own bblock -> six distinct OP_CALL_HANDLER indicator values and
	 * six OP_ENDFINALLY switch cases, all over the single finally.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f3_multi_leave (int sel, int[] log) {
		int r;
		try {
			switch (sel) {
			case 0: goto L0;
			case 1: goto L1;
			case 2: goto L2;
			case 3: goto L3;
			case 4: goto L4;
			default: goto L5;
			}
		} finally {
			log [0] = log [0] + 1;
		}
	L0: r = 10; goto End;
	L1: r = 21; goto End;
	L2: r = 32; goto End;
	L3: r = 43; goto End;
	L4: r = 54; goto End;
	L5: r = 65; goto End;
	End: return r;
	}

	public static int test_0_llvm_finally_multi_leave_many () {
		int[] expect = { 10, 21, 32, 43, 54, 65 };
		int[] log = new int [1];
		for (int sel = 0; sel < 6; sel++) {
			log [0] = 0;
			int r = llvm_f3_multi_leave (sel, log);
			if (r != expect [sel])
				return 1 + sel;			/* wrong continuation for sel */
			if (log [0] != 1)
				return 10 + sel;		/* finally skipped or double-run */
		}
		return 0;
	}

	/*
	 * Shape 2: leave that RETURNS A VALUE. Each continuation computes a distinct
	 * value from a local established before the leave, proving the indicator switch
	 * routes to the right VALUE-PRODUCING continuation, not merely the right block.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f3_value_leave (int sel, int[] log) {
		int basev = sel * 100;
		try {
			if (sel == 0) goto A;
			if (sel == 1) goto B;
			goto C;
		} finally {
			log [0] = log [0] + 1;
		}
	A: return basev + 1;
	B: return basev + 2;
	C: return basev + 3;
	}

	public static int test_0_llvm_finally_value_leave () {
		int[] log = new int [1];
		int[] expect = { 1, 102, 203 };
		for (int sel = 0; sel < 3; sel++) {
			log [0] = 0;
			int r = llvm_f3_value_leave (sel, log);
			if (r != expect [sel])
				return 1 + sel;
			if (log [0] != 1)
				return 10 + sel;
		}
		return 0;
	}

	/*
	 * Shape 3: leave OUT OF NESTED NON-FINALLY SCOPES. The goto Done crosses a
	 * for-loop and two nested ordinary blocks on its way out of the try; the
	 * fall-through path also leaves the try. Both exits route through the single
	 * finally to the same continuation.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f3_nested_scopes (int n, int[] log) {
		int acc = 0;
		try {
			for (int i = 0; i < n; i++) {
				acc += i;
				if (acc > 5) {
					{
						{
							goto Done;	/* leave crossing loop + nested blocks */
						}
					}
				}
			}
			acc += 1000;			/* fall-through path also leaves the try */
		} finally {
			log [0] = log [0] + 1;
		}
	Done: return acc;
	}

	public static int test_0_llvm_finally_nested_scopes () {
		int[] log = new int [1];
		/* n=5: 0+1+2+3 = 6 > 5 at i=3 -> goto Done, acc == 6 */
		log [0] = 0;
		int r0 = llvm_f3_nested_scopes (5, log);
		if (r0 != 6 || log [0] != 1)
			return 1;
		/* n=2: acc never exceeds 5, fall through -> acc == 0+1+1000 == 1001 */
		log [0] = 0;
		int r1 = llvm_f3_nested_scopes (2, log);
		if (r1 != 1001 || log [0] != 1)
			return 2;
		return 0;
	}

	/*
	 * Shape 4: NESTED-LEAVE continuations. The leave routes to A, B or C; each
	 * continuation chains into the next (A->B->C), so the finally's switch feeds a
	 * chain of continuations rather than independent returns, all through the one
	 * finally.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f3_chained (int sel, int[] log) {
		int acc = 0;
		try {
			if (sel == 0) goto A;
			if (sel == 1) goto B;
			goto C;
		} finally {
			log [0] = log [0] + 1;
		}
	A: acc += 1;   goto B;
	B: acc += 10;  goto C;
	C: acc += 100; return acc;
	}

	public static int test_0_llvm_finally_chained_leave () {
		int[] log = new int [1];
		int[] expect = { 111, 110, 100 };
		for (int sel = 0; sel < 3; sel++) {
			log [0] = 0;
			int r = llvm_f3_chained (sel, log);
			if (r != expect [sel])
				return 1 + sel;
			if (log [0] != 1)
				return 10 + sel;
		}
		return 0;
	}

	/*
	 * Shape 5: normal FALL-THROUGH and explicit LEAVE both exercised in one method;
	 * the finally must run exactly once on either exit.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int llvm_f3_fallthrough_vs_leave (int early, int[] log) {
		int x = 0;
		try {
			if (early != 0)
				return 7;		/* explicit early leave out of the try */
			x = 2;				/* fall-through path */
		} finally {
			log [0] = log [0] + 1;
		}
		return 8 + x;			/* natural fall-through continuation */
	}

	public static int test_0_llvm_finally_fallthrough_and_leave () {
		int[] log = new int [1];
		log [0] = 0;
		int r0 = llvm_f3_fallthrough_vs_leave (1, log);	/* early leave */
		if (r0 != 7 || log [0] != 1)
			return 1;
		log [0] = 0;
		int r1 = llvm_f3_fallthrough_vs_leave (0, log);	/* fall through */
		if (r1 != 10 || log [0] != 1)
			return 2;
		return 0;
	}

	/*
	 * EH F5 functional coverage: CROSS-TIER finally-resume. The from_llvm finally
	 * resume protocol (a tier-1/LLVM finally suspends via jit_tls->resume_state and
	 * re-enters mono_handle_exception_internal(resume=TRUE), then the walk continues
	 * into the NEXT frame) runs for the first time in genuinely MIXED tier-0/tier-1
	 * stacks (doc 16 5, the [UNVERIFIED] resume-tail-into-a-classic-next-frame item in
	 * 8). Every finally-bearing helper holds a single, non-nested finally clause (so the
	 * F2 nesting gate still admits it), is [NoInlining] so its frame really exists, and
	 * records the finally-run order in log (log [0] = count, log [1..] = the sequence of
	 * finally tags in the order they ran).
	 *
	 * These tests assert CORRECTNESS on every configuration (classic, and
	 * MONO_TIERED=1 at any threshold): the exact finally order AND the frame that
	 * catches. The per-frame TIER is forced deterministically OUT OF BAND with
	 * MONO_LLVM_METHOD (an allowlist: only the named methods reach the LLVM tier, every
	 * other stays tier-0 classic) + MONO_TIERED=1 + a low MONO_TIERED_CALL_THRESHOLD; the
	 * "_t1" helpers are the ones named there, the "_t0" helpers are left classic. Each
	 * test warms its intended-tier-1 helpers past a low threshold first so the MEASURED
	 * run executes the promoted tier-1 body (the warm-up is inert at the default
	 * threshold 1000, so these stay pure correctness tests in the regression suite).
	 */

	/* Scenario 1: throw inside a tier-1 (LLVM) try/finally; the finally suspends via
	   resume_state and the walk resumes into a tier-0 (classic) caller that catches.
	   Force: MONO_LLVM_METHOD='f5_s1_finally_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f5_s1_finally_t1 (bool doThrow, int[] log) {
		try {
			if (doThrow)
				throw new Exception ("f5_s1");
		} finally {
			log [++log [0]] = 1;
		}
	}

	public static int test_0_f5_cross_tier_s1 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; f5_s1_finally_t1 (false, log); }
		log [0] = 0;
		try {
			f5_s1_finally_t1 (true, log);
		} catch (Exception) {
			return (log [0] == 1 && log [1] == 1) ? 0 : 1;
		}
		return 2;
	}

	/* Scenario 2 (mirror): throw inside a tier-0 (classic) try/finally; the classic
	   finally RETURNS synchronously and the resume-driven walk continues into a tier-1
	   (LLVM) caller that catches. Force: MONO_LLVM_METHOD='f5_s2_catcher_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f5_s2_finally_t0 (bool doThrow, int[] log) {
		try {
			if (doThrow)
				throw new Exception ("f5_s2");
		} finally {
			log [++log [0]] = 1;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s2_catcher_t1 (bool doThrow, int[] log) {
		try {
			f5_s2_finally_t0 (doThrow, log);
		} catch (Exception) {
			return 7;
		}
		return 0;
	}

	public static int test_0_f5_cross_tier_s2 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; f5_s2_catcher_t1 (false, log); }
		log [0] = 0;
		int r = f5_s2_catcher_t1 (true, log);
		return (r == 7 && log [0] == 1 && log [1] == 1) ? 0 : 1;
	}

	/* Scenario 3: a throw crossing SEVERAL mixed-tier finally frames. The chain is
	   test(classic catch) -> fin1_t1(LLVM) -> fin2_t0(classic) -> fin3_t1(LLVM, throws),
	   so the pass-2 walk alternates resume / classic-return / resume before the catch.
	   All three finallys must run innermost-first (tags 3, 2, 1) and the catch must fire
	   in the outermost frame. Force: MONO_LLVM_METHOD='f5_s3_fin1_t1;f5_s3_fin3_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f5_s3_fin3_t1 (bool doThrow, int[] log) {
		try {
			if (doThrow)
				throw new Exception ("f5_s3");
		} finally {
			log [++log [0]] = 3;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f5_s3_fin2_t0 (bool doThrow, int[] log) {
		try {
			f5_s3_fin3_t1 (doThrow, log);
		} finally {
			log [++log [0]] = 2;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f5_s3_fin1_t1 (bool doThrow, int[] log) {
		try {
			f5_s3_fin2_t0 (doThrow, log);
		} finally {
			log [++log [0]] = 1;
		}
	}

	public static int test_0_f5_cross_tier_s3 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; f5_s3_fin1_t1 (false, log); }
		log [0] = 0;
		try {
			f5_s3_fin1_t1 (true, log);
		} catch (Exception) {
			return (log [0] == 3 && log [1] == 3 && log [2] == 2 && log [3] == 1) ? 0 : 1;
		}
		return 2;
	}

	/* Scenario 4: `leave` normal-exit across a tier boundary, both directions. A tier-1
	   finally whose continuation returns into a tier-0 caller, and a tier-0 finally
	   returning into a tier-1 caller - the non-exceptional finally path must be
	   tier-agnostic too. Force: MONO_LLVM_METHOD='f5_s4_finally_t1;f5_s4_caller_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s4_finally_t1 (int[] log) {
		try {
			return 42;
		} finally {
			log [++log [0]] = 4;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s4_caller_t0 (int[] log) {
		return f5_s4_finally_t1 (log) + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s4_finally_t0 (int[] log) {
		try {
			return 50;
		} finally {
			log [++log [0]] = 5;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s4_caller_t1 (int[] log) {
		return f5_s4_finally_t0 (log) + 1;
	}

	public static int test_0_f5_cross_tier_s4 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; f5_s4_caller_t0 (log); f5_s4_caller_t1 (log); }
		log [0] = 0;
		int a = f5_s4_caller_t0 (log);		/* tier-0 caller, tier-1 finally */
		int b = f5_s4_caller_t1 (log);		/* tier-1 caller, tier-0 finally */
		return (a == 43 && b == 51 && log [0] == 2 && log [1] == 4 && log [2] == 5) ? 0 : 1;
	}

	/* Scenario 5: value-returning method + finally across a tier boundary, both
	   directions - confirm the return value survives the mixed walk. Force:
	   MONO_LLVM_METHOD='f5_s5_value_t1;f5_s5_caller_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s5_value_t1 (int x, int[] log) {
		int v;
		try {
			v = x * 3 + 1;
		} finally {
			log [++log [0]] = 6;
		}
		return v;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s5_value_t0 (int x, int[] log) {
		int v;
		try {
			v = x * 5 + 2;
		} finally {
			log [++log [0]] = 7;
		}
		return v;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s5_caller_t0 (int x, int[] log) {
		return f5_s5_value_t1 (x, log);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int f5_s5_caller_t1 (int x, int[] log) {
		return f5_s5_value_t0 (x, log);
	}

	public static int test_0_f5_cross_tier_s5 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; f5_s5_caller_t0 (2, log); f5_s5_caller_t1 (2, log); }
		log [0] = 0;
		int a = f5_s5_caller_t0 (10, log);	/* tier-0 caller -> tier-1 value method: 10*3+1 */
		int b = f5_s5_caller_t1 (10, log);	/* tier-1 caller -> tier-0 value method: 10*5+2 */
		return (a == 31 && b == 52 && log [0] == 2 && log [1] == 6 && log [2] == 7) ? 0 : 1;
	}

	/*
	 * EH F6 functional coverage: nested-exception / finally edge hardening (the last
	 * finally/fault slice). These pin the documented edges of finally-on-LLVM (doc 16
	 * 7 F6; doc 11 9.3 single-slot resume_state; 8.3 free_stack). Every finally-bearing
	 * helper below holds a single, non-nested finally clause (so the F2 nesting gate
	 * still admits it and each compiles through the LLVM tier), is [NoInlining] so the
	 * clause structure survives, and records its run order in a log array. The tests
	 * are pure correctness tests in the regression suite (the warm-up loops are inert at
	 * the default promotion threshold); the tier-1 proof is a separate driven run under
	 * MONO_LLVM_METHOD (see .claude/scratch/eh-f6/progress.md).
	 *
	 * HEADLINE (doc 11 9.3 / doc 16 8 [UNVERIFIED]): a throw from INSIDE a from_llvm
	 * (tier-1) finally, before it reaches its resume trampoline. This abandons the outer
	 * unwind's single-slot jit_tls->resume_state. Per ECMA-335 12.4.2.5 that is CORRECT:
	 * an exception raised in a finally REPLACES the one being propagated; the original is
	 * lost. These tests assert exactly that - the finally's exception propagates and is
	 * caught at the right frame, the original is not resurrected, no intervening finally
	 * is skipped, and there is no crash or mis-unwind from the reused resume_state slot.
	 */

	/* Shape 1: try throws A, finally throws B; B replaces A. Assert the catcher sees B
	   (the finally's exception), the try was entered, and the finally body ran. Force:
	   MONO_LLVM_METHOD='f6_throw_in_finally_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f6_throw_in_finally_t1 (int[] log) {
		try {
			log [++log [0]] = 1;			/* try entered */
			throw new Exception ("A");
		} finally {
			log [++log [0]] = 2;			/* finally body ran */
			throw new Exception ("B");		/* replaces A */
		}
	}

	public static int test_0_f6_throw_in_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { f6_throw_in_finally_t1 (log); } catch { } }
		log [0] = 0;
		string caught = null;
		try {
			f6_throw_in_finally_t1 (log);
		} catch (Exception e) {
			caught = e.Message;
		}
		/* B must reach the catcher; the original A is discarded; both bodies ran in order. */
		return (caught == "B" && log [0] == 2 && log [1] == 1 && log [2] == 2) ? 0 : 1;
	}

	/* Shape 2: the throwing finally is one frame down; an INTERVENING tier-1 finally
	   between it and the catcher must still run for the REPLACEMENT exception B - proving
	   the new exception's fresh unwind runs intervening finallys even though the outer
	   A-unwind (and its saved resume_state) was abandoned. Force:
	   MONO_LLVM_METHOD='f6_inner_throws_t1;f6_intervening_finally_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f6_inner_throws_t1 (int[] log) {
		try {
			throw new Exception ("A");
		} finally {
			log [++log [0]] = 10;			/* inner finally ran */
			throw new Exception ("B");		/* replaces A */
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f6_intervening_finally_t1 (int[] log) {
		try {
			f6_inner_throws_t1 (log);
		} finally {
			log [++log [0]] = 20;			/* must run for B on the way out */
		}
	}

	public static int test_0_f6_throw_in_finally_intervening () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { f6_intervening_finally_t1 (log); } catch { } }
		log [0] = 0;
		string caught = null;
		try {
			f6_intervening_finally_t1 (log);
		} catch (Exception e) {
			caught = e.Message;
		}
		/* order: inner finally (10) throws B, intervening finally (20) runs for B, catch sees B. */
		return (caught == "B" && log [0] == 2 && log [1] == 10 && log [2] == 20) ? 0 : 1;
	}

	/* Shape 3: TYPE discrimination. try throws an ArgumentException; the finally throws
	   an InvalidOperationException that replaces it; the outer catch matches ONLY
	   InvalidOperationException. If the single-slot resume_state ever leaked the original
	   ArgumentException, an IOE-only catch would NOT match it and the exception would
	   escape - so a pass proves the REPLACEMENT exception's own type drives the match.
	   Force: MONO_LLVM_METHOD='f6_typed_throw_in_finally_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f6_typed_throw_in_finally_t1 (int[] log) {
		try {
			log [++log [0]] = 1;
			throw new ArgumentException ("orig");
		} finally {
			log [++log [0]] = 2;
			throw new InvalidOperationException ("repl");
		}
	}

	public static int test_0_f6_typed_throw_in_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { f6_typed_throw_in_finally_t1 (log); } catch { } }
		log [0] = 0;
		int where = 0;
		try {
			f6_typed_throw_in_finally_t1 (log);
		} catch (InvalidOperationException) {
			where = 1;			/* the replacement's type: correct */
		} catch (ArgumentException) {
			where = 2;			/* the original leaked: WRONG */
		}
		return (where == 1 && log [0] == 2 && log [1] == 1 && log [2] == 2) ? 0 : 1;
	}

	/* Shape 4: an EMPTY tier-1 finally on the EXCEPTIONAL path. The exception must still
	   propagate THROUGH the empty finally to an outer catch - proving the resume-trampoline
	   tail is emitted and taken even when the finally body is trivial (no observable side
	   effect to hide a dropped unwind). Force: MONO_LLVM_METHOD='f6_empty_finally_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void f6_empty_finally_t1 (int[] log) {
		try {
			log [++log [0]] = 1;
			throw new Exception ("thru");
		} finally {
			/* deliberately empty: the finally must still be traversed on unwind */
		}
	}

	public static int test_0_f6_empty_finally_exceptional () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { f6_empty_finally_t1 (log); } catch { } }
		log [0] = 0;
		string caught = null;
		try {
			f6_empty_finally_t1 (log);
		} catch (Exception e) {
			caught = e.Message;
		}
		return (caught == "thru" && log [0] == 1 && log [1] == 1) ? 0 : 1;
	}

	/*
	 * ---------------------------------------------------------------- EH N3 (doc 21)
	 * The nesting gate (translator.cpp) now ADMITS depth-2 clause containment through
	 * the LLVM tier: the .mono_lsda synthesis (build_ex_info, EH N1) appends one
	 * enclosing entry per base entry, and the innermost landing pad's selector switch
	 * (emit_handler_start, EH N2) re-dispatches RDX == the enclosing clause_index to
	 * that handler's body. These tests exercise the two sharpest depth-2 shapes end to
	 * end (doc 21 6.1 and 6.2). Each nesting helper carries a _t1 suffix so a driven
	 * run under MONO_LLVM_METHOD forces it onto the tier-1 LLVM path (these previously
	 * DECLINED); in the regression suite the warm-up loops are inert at the default
	 * promotion threshold, so each is a pure correctness test with the classic JIT as
	 * the differential oracle. C7ExA / C7ExB (defined below) are two unrelated custom
	 * exception types: C7ExA is the caught type, C7ExB the uncaught one.
	 *
	 * §6.1 - C# try/catch/finally: an inner CATCH nested in an outer FINALLY. mode 0 =
	 * no throw (finally on the normal `leave`); 1 = throw the caught type (inner catch
	 * runs, then finally on the `leave` out of the catch); 2 = throw an UNcaught type
	 * (inner catch skipped, the outer finally runs as unwinding cleanup, exception
	 * propagates to the caller). The finally is NOT intervening on the caught path -
	 * the catch body sits inside the finally's try, so unwinding to it never crosses
	 * the finally boundary.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n3_try_catch_finally_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer FINALLY */
			try {					/* inner CATCH (C7ExA) */
				log [++log [0]] = 1;
				if (mode == 1)
					throw new C7ExA ();
				if (mode == 2)
					throw new C7ExB ();
				r = 10;
			} catch (C7ExA) {
				log [++log [0]] = 2;
				r = 20;
			}
		} finally {
			log [++log [0]] = 3;
		}
		return r;
	}

	public static int test_0_n3_try_catch_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n3_try_catch_finally_t1 (0, log);
			log [0] = 0; n3_try_catch_finally_t1 (1, log);
			log [0] = 0; try { n3_try_catch_finally_t1 (2, log); } catch (C7ExB) { }
		}
		/* normal path: try body runs, finally on leave -> r=10, order 1,3 */
		log [0] = 0;
		int rn = n3_try_catch_finally_t1 (0, log);
		if (!(rn == 10 && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 1;
		/* caught: inner catch handles, finally on the leave out of the catch -> r=20, order 1,2,3 */
		log [0] = 0;
		int rc = n3_try_catch_finally_t1 (1, log);
		if (!(rc == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 2;
		/* uncaught: inner catch skipped, finally runs as cleanup, exception propagates -> order 1,3 */
		log [0] = 0;
		bool propagated = false;
		try {
			n3_try_catch_finally_t1 (2, log);
		} catch (C7ExB) {
			propagated = true;
		}
		if (!(propagated && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 3;
		return 0;
	}

	/*
	 * §6.2 - C# try/finally inside try/catch: an inner FINALLY nested in an outer
	 * CATCH (the sharpest case). The inner finally OWNS the landing pad the fault
	 * unwinds to, so that finally pad must (N2 §5.2) carry a selector switch routing
	 * RDX == the outer catch's clause_index to the catch body, AND store the exception
	 * object for the catch route. mode 0 = no throw (finally on the normal `leave`);
	 * 1 = throw the caught type (the intervening inner finally runs FIRST, THEN the
	 * outer catch handles it, seeing the right exception object); 2 = throw an uncaught
	 * type (inner finally runs as cleanup, exception propagates).
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n3_finally_in_catch_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer CATCH (C7ExA) */
			try {					/* inner FINALLY */
				log [++log [0]] = 1;
				if (mode == 1)
					throw new C7ExA ();
				if (mode == 2)
					throw new C7ExB ();
				r = 10;
			} finally {
				log [++log [0]] = 2;
			}
		} catch (C7ExA e) {
			/* record whether the routed exception object is the right one */
			log [++log [0]] = (e is C7ExA) ? 3 : 99;
			r = 20;
		}
		return r;
	}

	public static int test_0_n3_finally_in_catch () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n3_finally_in_catch_t1 (0, log);
			log [0] = 0; n3_finally_in_catch_t1 (1, log);
			log [0] = 0; try { n3_finally_in_catch_t1 (2, log); } catch (C7ExB) { }
		}
		/* normal path: try body, finally on leave -> r=10, order 1,2 */
		log [0] = 0;
		int rn = n3_finally_in_catch_t1 (0, log);
		if (!(rn == 10 && log [0] == 2 && log [1] == 1 && log [2] == 2))
			return 1;
		/* caught: inner finally FIRST, THEN outer catch (sees the exc) -> r=20, order 1,2,3 */
		log [0] = 0;
		int rc = n3_finally_in_catch_t1 (1, log);
		if (!(rc == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 2;
		/* uncaught: inner finally as cleanup, exception propagates -> order 1,2 */
		log [0] = 0;
		bool propagated = false;
		try {
			n3_finally_in_catch_t1 (2, log);
		} catch (C7ExB) {
			propagated = true;
		}
		if (!(propagated && log [0] == 2 && log [1] == 1 && log [2] == 2))
			return 3;
		return 0;
	}

	/*
	 * DEPTH-3 (doc 21 8.1 / EH N6): a nest where the innermost CATCH(C7ExA) clause has
	 * TWO enclosers - the middle and outer finallys. Until N6 this DECLINED (the gate's
	 * depth-2 `enclosers > 1` cap); N6 removes the cap so it now compiles through the
	 * LLVM tier, and this is its live guard. The inner catch owns the landing pad; its
	 * base entry synthesises TWO enclosing finally entries (middle then outer, ascending
	 * clause_index = innermost-encloser first), so an uncaught throw runs the middle
	 * finally THEN the outer finally inner-to-outer, byte-for-slot identical to the
	 * classic inner-first clause array. Named _t1; forced tier-1 via
	 * MONO_LLVM_METHOD='n3_depth3_nested_t1', asserted tier-1 == classic.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n3_depth3_nested_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer FINALLY */
			try {					/* middle FINALLY */
				try {				/* inner CATCH (C7ExA) - two enclosers */
					log [++log [0]] = 1;
					if (mode == 1)
						C7Throw (1);		/* C7ExA -> inner catch */
					if (mode == 2)
						C7Throw (2);		/* C7ExB -> uncaught, both finallys as cleanup */
					r = 10;
				} catch (C7ExA) {
					log [++log [0]] = 2;
					r = 20;
				}
			} finally {
				log [++log [0]] = 3;
			}
		} finally {
			log [++log [0]] = 4;
		}
		return r;
	}

	public static int test_0_n3_depth3_nested () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n3_depth3_nested_t1 (0, log);
			log [0] = 0; n3_depth3_nested_t1 (1, log);
			log [0] = 0; try { n3_depth3_nested_t1 (2, log); } catch (C7ExB) { }
		}
		/* normal path: try body, both finallys inner-to-outer -> r=10, order 1,3,4 */
		log [0] = 0;
		int rn = n3_depth3_nested_t1 (0, log);
		if (!(rn == 10 && log [0] == 3 && log [1] == 1 && log [2] == 3 && log [3] == 4))
			return 1;
		/* caught: inner catch, then both finallys on the leave -> r=20, order 1,2,3,4 */
		log [0] = 0;
		int re = n3_depth3_nested_t1 (1, log);
		if (!(re == 20 && log [0] == 4 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 4))
			return 2;
		/* uncaught: inner catch skipped, middle THEN outer finally as cleanup, propagate -> order 1,3,4 */
		log [0] = 0;
		bool prop = false;
		try { n3_depth3_nested_t1 (2, log); } catch (C7ExB) { prop = true; }
		if (!(prop && log [0] == 3 && log [1] == 1 && log [2] == 3 && log [3] == 4))
			return 3;
		return 0;
	}

	/*
	 * ---------------------------------------------------------------- EH N4 (doc 21)
	 * Depth-2 nested-clause BREADTH, promoted to permanent regression tests. N1/N2/N3
	 * turned the LLVM tier's nesting gate on for one level of clause containment; N3
	 * pinned the two sharpest shapes (doc 21 6.1 try/catch/finally, 6.2 finally-in-
	 * catch). These lock in the rest of the depth-2 surface the N3 adversarial review
	 * ran throwaway and found tier-1-identical-to-classic: nested try/catch with
	 * base/derived precedence across the boundary (6.3), nested try/finally cleanup
	 * order (6.4), a base-type catch enclosed by a finally, value-returning
	 * finally-in-catch breadth, and an inner try with several invoke ranges under an
	 * outer finally.
	 *
	 * A sixth shape - a SIBLING catch group enclosed by an outer finally - once
	 * uncovered a real tier-1 miscompile in the N1 enclosing-entry synthesis, where
	 * the outer finally ran ONCE PER SIBLING when an exception propagated past every
	 * sibling (the same-range sibling base entries each re-synthesised the same
	 * enclosing finally ei). That is now FIXED - build_ex_info de-duplicates
	 * synthesised enclosing entries by (base range, enclosing clause) - and the shape
	 * is a permanent guard here: test_0_n4_sibling_catch_in_finally (two siblings) plus
	 * a three-sibling variant, each asserting the finally runs exactly once regardless
	 * of sibling count (seq 1,4 / 1,5 on the propagate path, not 1,4,4 / 1,5,5,5).
	 *
	 * Each nesting helper carries a _t1 suffix; a driven run under
	 *   MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=1 \
	 *   MONO_LLVM_METHOD='n4_nested_catch_t1;n4_nested_finally_t1;\
	 *     n4_base_catch_in_finally_t1;n4_finally_in_catch_multi_t1;\
	 *     n4_multi_invoke_t1;n4_sibling_catch_in_finally_t1;\
	 *     n4_sibling3_catch_in_finally_t1' ./mono -v exceptions.exe
	 * forces each onto the tier-1 LLVM path (verbose "LLVM Method ... emitted"). In the
	 * regression suite the warm-up loops are inert at the default promotion threshold,
	 * so each is a pure correctness test with the classic JIT as the differential
	 * oracle - the assertions (which clause runs, in what order, caught vs propagated,
	 * exception identity, finally side-effects) must hold identically on both tiers.
	 * C7ExA / C7ExB / C7ExC (: C7ExB) / C7ExD (defined below) are the exception types;
	 * C7Throw maps 1->A 2->B 3->C 4->D 0->none. Every shape is depth-2 (each inner
	 * clause has exactly ONE encloser), so the depth-2 gate admits it.
	 */

	/*
	 * §6.3 - nested try/catch: inner CATCH(C7ExC, derived) in an outer CATCH(C7ExB,
	 * base). Base/derived precedence crosses the nesting boundary: the derived type
	 * hits the inner catch; the base type skips the inner (isinst fails) and the outer
	 * base catch takes it; an unrelated type propagates. mode 0 no throw, 1 C7ExC,
	 * 2 C7ExB, 3 C7ExA.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n4_nested_catch_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer CATCH (C7ExB, base) */
			try {					/* inner CATCH (C7ExC : C7ExB, derived) */
				log [++log [0]] = 1;
				if (mode == 1)
					throw new C7ExC ();
				if (mode == 2)
					throw new C7ExB ();
				if (mode == 3)
					throw new C7ExA ();
				r = 10;
			} catch (C7ExC) {
				log [++log [0]] = 2;
				r = 20;
			}
		} catch (C7ExB) {
			log [++log [0]] = 3;
			r = 30;
		}
		return r;
	}

	public static int test_0_n4_nested_catch () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_nested_catch_t1 (0, log);
			log [0] = 0; n4_nested_catch_t1 (1, log);
			log [0] = 0; n4_nested_catch_t1 (2, log);
			log [0] = 0; try { n4_nested_catch_t1 (3, log); } catch (C7ExA) { }
		}
		/* no throw: inner try body, no handler -> r=10, order 1 */
		log [0] = 0;
		if (!(n4_nested_catch_t1 (0, log) == 10 && log [0] == 1 && log [1] == 1))
			return 1;
		/* derived: inner catch wins across the boundary -> r=20, order 1,2 */
		log [0] = 0;
		if (!(n4_nested_catch_t1 (1, log) == 20 && log [0] == 2 && log [1] == 1 && log [2] == 2))
			return 2;
		/* base: inner isinst fails, outer base catch takes it -> r=30, order 1,3 */
		log [0] = 0;
		if (!(n4_nested_catch_t1 (2, log) == 30 && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 3;
		/* unrelated: neither catch matches, propagates -> order 1 */
		log [0] = 0;
		bool prop = false;
		try { n4_nested_catch_t1 (3, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 1 && log [1] == 1))
			return 4;
		return 0;
	}

	/*
	 * §6.4 - nested try/finally (both FINALLY). An uncaught throw runs the inner
	 * finally THEN the outer finally (inner-to-outer cleanup by array order) and then
	 * propagates; the normal-leave path runs them in the same order. mode 0 no throw,
	 * 1 throw (uncaught, propagates).
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n4_nested_finally_t1 (int mode, int[] log) {
		try {						/* outer FINALLY */
			try {					/* inner FINALLY */
				log [++log [0]] = 1;
				if (mode == 1)
					throw new C7ExA ();
			} finally {
				log [++log [0]] = 2;		/* inner finally */
			}
		} finally {
			log [++log [0]] = 3;		/* outer finally */
		}
	}

	public static int test_0_n4_nested_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_nested_finally_t1 (0, log);
			log [0] = 0; try { n4_nested_finally_t1 (1, log); } catch (C7ExA) { }
		}
		/* normal leave: inner finally then outer finally -> order 1,2,3 */
		log [0] = 0;
		n4_nested_finally_t1 (0, log);
		if (!(log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 1;
		/* uncaught: both finallys inner-to-outer as cleanup, then propagates -> order 1,2,3 */
		log [0] = 0;
		bool prop = false;
		try { n4_nested_finally_t1 (1, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 2;
		return 0;
	}

	/*
	 * A base-type inner CATCH enclosed by an outer FINALLY: try { try {throw Derived}
	 * catch (Base) {} } finally {}. The base catch matches the derived throw; the
	 * finally runs on every path. mode 0 no throw, 1 throw C7ExC (derived, caught by
	 * the C7ExB base catch), 2 throw C7ExA (unrelated, propagates after the finally).
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n4_base_catch_in_finally_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer FINALLY */
			try {					/* inner CATCH (C7ExB, base) */
				log [++log [0]] = 1;
				if (mode == 1)
					throw new C7ExC ();		/* derived -> base catch matches */
				if (mode == 2)
					throw new C7ExA ();		/* unrelated -> propagate */
				r = 10;
			} catch (C7ExB) {
				log [++log [0]] = 2;
				r = 20;
			}
		} finally {
			log [++log [0]] = 3;
		}
		return r;
	}

	public static int test_0_n4_base_catch_in_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_base_catch_in_finally_t1 (0, log);
			log [0] = 0; n4_base_catch_in_finally_t1 (1, log);
			log [0] = 0; try { n4_base_catch_in_finally_t1 (2, log); } catch (C7ExA) { }
		}
		/* no throw: catch skipped, finally on leave -> r=10, order 1,3 */
		log [0] = 0;
		if (!(n4_base_catch_in_finally_t1 (0, log) == 10 && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 1;
		/* derived caught by base: catch, then finally on the leave -> r=20, order 1,2,3 */
		log [0] = 0;
		if (!(n4_base_catch_in_finally_t1 (1, log) == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 2;
		/* unrelated: catch skipped, finally as cleanup, propagates -> order 1,3 */
		log [0] = 0;
		bool prop = false;
		try { n4_base_catch_in_finally_t1 (2, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 3;
		return 0;
	}

	/*
	 * §6.2 breadth - value-returning try/finally inside try/catch with base/derived
	 * matching. The inner FINALLY owns the landing pad the fault unwinds to; the outer
	 * base CATCH(C7ExB) routes through it. mode 0 no throw; 1 throw C7ExC (derived,
	 * matched by the base catch, so the inner finally runs FIRST then the catch sees
	 * the right object); 2 throw C7ExA (unrelated, inner finally as cleanup then
	 * propagate). The method returns a value on every path.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n4_finally_in_catch_multi_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer CATCH (C7ExB, base) */
			try {					/* inner FINALLY */
				log [++log [0]] = 1;
				if (mode == 1)
					throw new C7ExC ();		/* derived -> matched by base catch */
				if (mode == 2)
					throw new C7ExA ();		/* unrelated -> cleanup + propagate */
				r = 10;
			} finally {
				log [++log [0]] = 2;
			}
		} catch (C7ExB e) {
			/* record whether the routed exception object is the derived one */
			log [++log [0]] = (e is C7ExC) ? 3 : 99;
			r = 20;
		}
		return r;
	}

	public static int test_0_n4_finally_in_catch_multi () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_finally_in_catch_multi_t1 (0, log);
			log [0] = 0; n4_finally_in_catch_multi_t1 (1, log);
			log [0] = 0; try { n4_finally_in_catch_multi_t1 (2, log); } catch (C7ExA) { }
		}
		/* no throw: finally on leave -> r=10, order 1,2 */
		log [0] = 0;
		if (!(n4_finally_in_catch_multi_t1 (0, log) == 10 && log [0] == 2 && log [1] == 1 && log [2] == 2))
			return 1;
		/* derived caught by base: inner finally FIRST, then outer catch (right obj) -> r=20, order 1,2,3 */
		log [0] = 0;
		if (!(n4_finally_in_catch_multi_t1 (1, log) == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 2;
		/* unrelated: inner finally as cleanup, propagates -> order 1,2 */
		log [0] = 0;
		bool prop = false;
		try { n4_finally_in_catch_multi_t1 (2, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 2 && log [1] == 1 && log [2] == 2))
			return 3;
		return 0;
	}

	/*
	 * An inner try with MULTIPLE invoke ranges under an outer FINALLY: three separate
	 * calls in the inner protected region, each a distinct invoke range, each routed
	 * to the inner CATCH(C7ExB) when it faults. slot 1/2/3 = the i-th call throws
	 * C7ExB (caught by the inner catch, proving every range is covered); slot 4 = the
	 * middle call throws C7ExA (unrelated, propagates through the outer finally);
	 * slot 0 = no throw.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n4_multi_invoke_t1 (int slot, int[] log) {
		int r = 0;
		try {						/* outer FINALLY */
			try {					/* inner CATCH (C7ExB) */
				log [++log [0]] = 1;
				C7Throw (slot == 1 ? 2 : 0);			/* call 1 */
				log [++log [0]] = 2;
				C7Throw (slot == 2 ? 2 : (slot == 4 ? 1 : 0));	/* call 2: slot4 -> C7ExA (uncaught) */
				log [++log [0]] = 3;
				C7Throw (slot == 3 ? 2 : 0);			/* call 3 */
				log [++log [0]] = 4;
				r = 10;
			} catch (C7ExB) {
				log [++log [0]] = 5;
				r = 20;
			}
		} finally {
			log [++log [0]] = 6;
		}
		return r;
	}

	public static int test_0_n4_multi_invoke () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_multi_invoke_t1 (0, log);
			log [0] = 0; n4_multi_invoke_t1 (1, log);
			log [0] = 0; n4_multi_invoke_t1 (2, log);
			log [0] = 0; n4_multi_invoke_t1 (3, log);
			log [0] = 0; try { n4_multi_invoke_t1 (4, log); } catch (C7ExA) { }
		}
		/* no throw: all three calls inert, finally on leave -> r=10, order 1,2,3,4,6 */
		log [0] = 0;
		if (!(n4_multi_invoke_t1 (0, log) == 10 && log [0] == 5 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 4 && log [5] == 6))
			return 1;
		/* call 1 faults -> inner catch, finally -> r=20, order 1,5,6 */
		log [0] = 0;
		if (!(n4_multi_invoke_t1 (1, log) == 20 && log [0] == 3 && log [1] == 1 && log [2] == 5 && log [3] == 6))
			return 2;
		/* call 2 faults -> inner catch, finally -> r=20, order 1,2,5,6 */
		log [0] = 0;
		if (!(n4_multi_invoke_t1 (2, log) == 20 && log [0] == 4 && log [1] == 1 && log [2] == 2 && log [3] == 5 && log [4] == 6))
			return 3;
		/* call 3 faults -> inner catch, finally -> r=20, order 1,2,3,5,6 */
		log [0] = 0;
		if (!(n4_multi_invoke_t1 (3, log) == 20 && log [0] == 5 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 5 && log [5] == 6))
			return 4;
		/* middle call throws unrelated: inner catch skipped, finally cleanup, propagates -> order 1,2,6 */
		log [0] = 0;
		bool prop = false;
		try { n4_multi_invoke_t1 (4, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 6))
			return 5;
		return 0;
	}

	/*
	 * A SIBLING catch group (two catches over one protected region) enclosed by an
	 * outer FINALLY. This uncovered a tier-1 miscompile in the N1 enclosing-entry
	 * synthesis: the outer finally ran ONCE PER SIBLING when an exception propagated
	 * past every sibling (an ECMA-335 §12.4.2 violation - a double-run finally can
	 * dispose/unlock twice), because each same-range sibling base entry independently
	 * re-synthesised the SAME enclosing finally ei, leaving two identical finally
	 * entries over one range that pass-2's first-match-and-continue scan both ran. The
	 * fix de-duplicates synthesised enclosing entries by (base range, enclosing
	 * clause), so the finally is synthesised once regardless of sibling count. mode 4
	 * (C7ExD) matches NEITHER sibling and propagates past both -> the finally must run
	 * EXACTLY ONCE (seq 1,4 - the bug gave 1,4,4). Modes 1/2/3 exercise the caught
	 * paths (each catch then the finally on the leave), 0 the no-throw leave. Force
	 * tier-1: MONO_LLVM_METHOD='n4_sibling_catch_in_finally_t1'.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n4_sibling_catch_in_finally_t1 (int which, int[] log) {
		int r = 0;
		try {						/* outer FINALLY */
			try {					/* inner: two sibling catches, one protected region */
				log [++log [0]] = 1;
				C7Throw (which);
				r = 10;
			} catch (C7ExA) {
				log [++log [0]] = 2;
				r = 20;
			} catch (C7ExB) {
				log [++log [0]] = 3;
				r = 30;
			}
		} finally {
			log [++log [0]] = 4;
		}
		return r;
	}

	public static int test_0_n4_sibling_catch_in_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_sibling_catch_in_finally_t1 (0, log);
			log [0] = 0; n4_sibling_catch_in_finally_t1 (1, log);
			log [0] = 0; n4_sibling_catch_in_finally_t1 (2, log);
			log [0] = 0; n4_sibling_catch_in_finally_t1 (3, log);
			log [0] = 0; try { n4_sibling_catch_in_finally_t1 (4, log); } catch (C7ExD) { }
		}
		/* no throw: finally on the leave -> r=10, seq 1,4 */
		log [0] = 0;
		if (!(n4_sibling_catch_in_finally_t1 (0, log) == 10 && log [0] == 2 && log [1] == 1 && log [2] == 4))
			return 1;
		/* C7ExA -> first sibling, then finally -> r=20, seq 1,2,4 */
		log [0] = 0;
		if (!(n4_sibling_catch_in_finally_t1 (1, log) == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 4))
			return 2;
		/* C7ExB -> second sibling, then finally -> r=30, seq 1,3,4 */
		log [0] = 0;
		if (!(n4_sibling_catch_in_finally_t1 (2, log) == 30 && log [0] == 3 && log [1] == 1 && log [2] == 3 && log [3] == 4))
			return 3;
		/* C7ExC : C7ExB -> second sibling (base match), then finally -> r=30, seq 1,3,4 */
		log [0] = 0;
		if (!(n4_sibling_catch_in_finally_t1 (3, log) == 30 && log [0] == 3 && log [1] == 1 && log [2] == 3 && log [3] == 4))
			return 4;
		/* C7ExD matches NEITHER sibling: propagates past both, the outer finally runs
		 * EXACTLY ONCE as cleanup (the bug ran it twice -> seq 1,4,4). seq 1,4. */
		log [0] = 0;
		bool prop = false;
		try { n4_sibling_catch_in_finally_t1 (4, log); } catch (C7ExD) { prop = true; }
		if (!(prop && log [0] == 2 && log [1] == 1 && log [2] == 4))
			return 5;
		return 0;
	}

	/*
	 * THREE-sibling variant of the above, pinning "the finally runs exactly ONCE
	 * regardless of sibling count": a three-catch sibling group enclosed by an outer
	 * finally. Before the (range, enclosing clause) de-dup the finally would have been
	 * synthesised three times, running THREE times on the propagate path (seq 1,5,5,5).
	 * mode 5 (C7ExE) matches none of catch(A)/catch(B)/catch(D) and propagates past all
	 * three -> the finally must run EXACTLY ONCE (seq 1,5). Force tier-1:
	 * MONO_LLVM_METHOD='n4_sibling3_catch_in_finally_t1'.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n4_sibling3_catch_in_finally_t1 (int which, int[] log) {
		int r = 0;
		try {						/* outer FINALLY */
			try {					/* inner: THREE sibling catches, one protected region */
				log [++log [0]] = 1;
				C7Throw (which);
				r = 10;
			} catch (C7ExA) {
				log [++log [0]] = 2;
				r = 20;
			} catch (C7ExB) {
				log [++log [0]] = 3;
				r = 30;
			} catch (C7ExD) {
				log [++log [0]] = 4;
				r = 40;
			}
		} finally {
			log [++log [0]] = 5;
		}
		return r;
	}

	public static int test_0_n4_sibling3_catch_in_finally () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n4_sibling3_catch_in_finally_t1 (0, log);
			log [0] = 0; n4_sibling3_catch_in_finally_t1 (1, log);
			log [0] = 0; n4_sibling3_catch_in_finally_t1 (2, log);
			log [0] = 0; n4_sibling3_catch_in_finally_t1 (4, log);
			log [0] = 0; try { n4_sibling3_catch_in_finally_t1 (5, log); } catch (C7ExE) { }
		}
		/* no throw: finally on the leave -> r=10, seq 1,5 */
		log [0] = 0;
		if (!(n4_sibling3_catch_in_finally_t1 (0, log) == 10 && log [0] == 2 && log [1] == 1 && log [2] == 5))
			return 1;
		/* C7ExA -> first sibling, then finally -> r=20, seq 1,2,5 */
		log [0] = 0;
		if (!(n4_sibling3_catch_in_finally_t1 (1, log) == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 5))
			return 2;
		/* C7ExB -> second sibling, then finally -> r=30, seq 1,3,5 */
		log [0] = 0;
		if (!(n4_sibling3_catch_in_finally_t1 (2, log) == 30 && log [0] == 3 && log [1] == 1 && log [2] == 3 && log [3] == 5))
			return 3;
		/* C7ExD -> third sibling, then finally -> r=40, seq 1,4,5 */
		log [0] = 0;
		if (!(n4_sibling3_catch_in_finally_t1 (4, log) == 40 && log [0] == 3 && log [1] == 1 && log [2] == 4 && log [3] == 5))
			return 4;
		/* C7ExE matches NONE of the three siblings: propagates, the outer finally runs
		 * EXACTLY ONCE (the bug ran it 3x -> seq 1,5,5,5). seq 1,5. */
		log [0] = 0;
		bool prop = false;
		try { n4_sibling3_catch_in_finally_t1 (5, log); } catch (C7ExE) { prop = true; }
		if (!(prop && log [0] == 2 && log [1] == 1 && log [2] == 5))
			return 5;
		return 0;
	}

	/*
	 * ---------------------------------------------------------------- EH N5 (doc 21 §8.2)
	 * Cross-tier composition of DEPTH-2 nested EH. F5 proved the from_llvm finally-resume
	 * protocol across mixed tier-0/tier-1 stacks for SINGLE-level finally clauses; N1-N4
	 * turned the LLVM tier's nesting gate on for one level of clause containment and pinned
	 * the depth-2 surface (incl. the sibling-catch-in-finally double-run fix). N5 is their
	 * intersection: a throw that unwinds through frames each carrying a depth-2 NESTED EH
	 * structure, where the frames alternate tiers, must run every intervening finally
	 * inner-to-outer in the right order across each tier boundary, enter the right catch in
	 * the right frame, and preserve exception identity - regardless of whether a given
	 * nested frame is tier-1 (from_llvm resume) or tier-0 (classic call_filter).
	 *
	 * The per-frame TIER is forced deterministically OUT OF BAND (exactly the F5/N4
	 * mechanism): MONO_LLVM_METHOD is an ALLOWLIST - only the named methods reach the LLVM
	 * tier, every other stays tier-0 classic - and under MONO_TIERED=1 with a low
	 * MONO_TIERED_CALL_THRESHOLD a filtered-out method's promotion returns NULL (compiled
	 * NO_LLVM_FALLBACK) so it stays tier-0. Helpers ending "_t1" are the ones named there
	 * (forced tier-1); helpers ending "_t0" are left classic. Each test warms its intended
	 * tier-1 helpers past a low threshold first so the MEASURED run executes the promoted
	 * tier-1 body; the warm-up is inert at the default promotion threshold, so these stay
	 * pure correctness tests in the regression suite with the classic JIT as the oracle.
	 * Driven tier-1 proof:
	 *   MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=1 \
	 *   MONO_LLVM_METHOD='n5_s1_nested_t1;n5_s2_caller_t1;n5_s3_f1_t1;n5_s3_f3_t1;\
	 *     n5_s4_sibling_t1' ./mono -v exceptions.exe
	 * Each nesting helper is depth-2 (every inner clause has exactly one encloser), [NoInlining]
	 * so its frame really exists, and records finally/catch order in log (log[0]=count,
	 * log[1..]=the run sequence). C7ExA/C7ExB/C7ExC(:C7ExB)/C7ExD are defined below.
	 */

	/* Scenario 1: a tier-1 nested frame (inner FINALLY inside outer FINALLY) whose uncaught
	   throw runs BOTH finallys via resume (inner-then-outer) and propagates to a tier-0
	   classic caller that catches. Force: MONO_LLVM_METHOD='n5_s1_nested_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n5_s1_nested_t1 (int[] log) {
		try {						/* outer FINALLY (tier-1) */
			try {					/* inner FINALLY (tier-1) */
				C7Throw (1);			/* throw C7ExA via a call (invoke edge) */
			} finally {
				log [++log [0]] = 2;		/* inner finally */
			}
		} finally {
			log [++log [0]] = 3;			/* outer finally */
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n5_s1_caller_t0 (int[] log) {
		try {
			n5_s1_nested_t1 (log);
		} catch (C7ExA) {
			return 7;
		}
		return 0;
	}

	public static int test_0_n5_cross_tier_s1 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { n5_s1_nested_t1 (log); } catch (C7ExA) { } }
		log [0] = 0;
		int r = n5_s1_caller_t0 (log);
		/* tier-1 inner then outer finally via resume, caught in the tier-0 frame */
		return (r == 7 && log [0] == 2 && log [1] == 2 && log [2] == 3) ? 0 : 1;
	}

	/* Scenario 2 (mirror): a tier-0 nested frame (inner FINALLY inside outer FINALLY) whose
	   classic finallys return synchronously into a resume-driven tier-1 caller that is
	   ITSELF depth-2 nested (inner CATCH inside outer FINALLY). The tier-0 finallys run
	   2,3; the tier-1 inner catch takes it (4); the tier-1 outer finally runs on the leave
	   (5). Force: MONO_LLVM_METHOD='n5_s2_caller_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n5_s2_nested_t0 (int[] log) {
		try {						/* outer FINALLY (tier-0) */
			try {					/* inner FINALLY (tier-0) */
				C7Throw (1);			/* throw C7ExA via a call (invoke edge) */
			} finally {
				log [++log [0]] = 2;
			}
		} finally {
			log [++log [0]] = 3;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n5_s2_caller_t1 (int[] log) {
		int r = 0;
		try {						/* outer FINALLY (tier-1) */
			try {					/* inner CATCH (tier-1) */
				n5_s2_nested_t0 (log);
			} catch (C7ExA) {
				log [++log [0]] = 4;
				r = 7;
			}
		} finally {
			log [++log [0]] = 5;
		}
		return r;
	}

	public static int test_0_n5_cross_tier_s2 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; n5_s2_caller_t1 (log); }
		log [0] = 0;
		int r = n5_s2_caller_t1 (log);
		return (r == 7 && log [0] == 4 && log [1] == 2 && log [2] == 3 && log [3] == 4 && log [4] == 5) ? 0 : 1;
	}

	/* Scenario 3: a throw crossing SEVERAL mixed-tier NESTED frames. The chain is
	   test(classic catch) -> f1_t1(LLVM, nested finally) -> f2_t0(classic, nested finally)
	   -> f3_t1(LLVM, nested finally, throws). Each frame holds an inner FINALLY inside an
	   outer FINALLY, so the pass-2 walk alternates resume / classic-return / resume and
	   must run all SIX finallys innermost-first (6,5,4,3,2,1) before the outermost tier-0
	   catch. Force: MONO_LLVM_METHOD='n5_s3_f1_t1;n5_s3_f3_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n5_s3_f3_t1 (int[] log) {
		try {						/* f3 outer FINALLY (tier-1) */
			try {					/* f3 inner FINALLY (tier-1) */
				C7Throw (1);			/* throw C7ExA via a call (invoke edge) */
			} finally {
				log [++log [0]] = 6;
			}
		} finally {
			log [++log [0]] = 5;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n5_s3_f2_t0 (int[] log) {
		try {						/* f2 outer FINALLY (tier-0) */
			try {					/* f2 inner FINALLY (tier-0) */
				n5_s3_f3_t1 (log);
			} finally {
				log [++log [0]] = 4;
			}
		} finally {
			log [++log [0]] = 3;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n5_s3_f1_t1 (int[] log) {
		try {						/* f1 outer FINALLY (tier-1) */
			try {					/* f1 inner FINALLY (tier-1) */
				n5_s3_f2_t0 (log);
			} finally {
				log [++log [0]] = 2;
			}
		} finally {
			log [++log [0]] = 1;
		}
	}

	public static int test_0_n5_cross_tier_s3 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { n5_s3_f1_t1 (log); } catch (C7ExA) { } }
		log [0] = 0;
		try {
			n5_s3_f1_t1 (log);
		} catch (C7ExA) {
			return (log [0] == 6 && log [1] == 6 && log [2] == 5 && log [3] == 4 &&
				log [4] == 3 && log [5] == 2 && log [6] == 1) ? 0 : 1;
		}
		return 2;
	}

	/* Scenario 4: a nested SIBLING-catch-in-finally frame (the shape the double-run fix
	   addressed) in a MIXED stack. A tier-1 frame has two sibling catches (C7ExA/C7ExB)
	   over one inner region enclosed by an outer FINALLY; a C7ExD matches NEITHER sibling
	   and propagates past both, so the tier-1 outer finally must run EXACTLY ONCE (seq
	   1,4 - the pre-fix bug ran it twice: 1,4,4) as unwinding continues into a tier-0 next
	   frame that catches C7ExD. Force: MONO_LLVM_METHOD='n5_s4_sibling_t1'. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n5_s4_sibling_t1 (int[] log) {
		try {						/* outer FINALLY (tier-1) */
			try {					/* inner: two sibling catches, one region */
				log [++log [0]] = 1;
				C7Throw (4);			/* C7ExD: matches NEITHER sibling */
			} catch (C7ExA) {
				log [++log [0]] = 2;
			} catch (C7ExB) {
				log [++log [0]] = 3;
			}
		} finally {
			log [++log [0]] = 4;			/* must run EXACTLY once */
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n5_s4_caller_t0 (int[] log) {
		try {
			n5_s4_sibling_t1 (log);
		} catch (C7ExD) {
			log [++log [0]] = 5;
			return 9;
		}
		return 0;
	}

	public static int test_0_n5_cross_tier_s4 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) { log [0] = 0; try { n5_s4_sibling_t1 (log); } catch (C7ExD) { } }
		log [0] = 0;
		int r = n5_s4_caller_t0 (log);
		/* tier-1 finally runs exactly once (1,4) then the tier-0 next frame catches (5) */
		return (r == 9 && log [0] == 3 && log [1] == 1 && log [2] == 4 && log [3] == 5) ? 0 : 1;
	}

	/*
	 * ---------------------------------------------------------------- EH N6 (doc 21 §8.1/§4.1)
	 * N6 removes the depth-2 nesting cap: any NONE/FINALLY/FAULT try-nesting of ARBITRARY
	 * depth now compiles through the LLVM tier. That exercises, for the first time live, a
	 * clause with MULTIPLE enclosers - and with it the enclosing-entry ORDER that doc 21 §4.1
	 * left open. build_ex_info appends a base's enclosing entries in ASCENDING clause_index;
	 * ECMA-335 §12.4.2.5 numbers a more-deeply-nested try clause BEFORE its enclosers, so
	 * ascending clause_index == innermost-encloser first. Because pass-2 resumes at the running
	 * clause's ARRAY slot + 1, that order makes intervening finallys run inner-to-outer and
	 * enclosing catches be reached in precedence order - byte-for-slot identical to the classic
	 * JIT, whose jinfo->clauses is the same inner-first IL clause array. (The legacy
	 * mini-llvm.c:3821 prepend built the DESCENDING order, which would run a depth-3 finally
	 * chain C, A, B - only ever safe live at depth-2, where the single encloser makes order
	 * moot.) These tests pin the order at depth 3 and 4 against the classic oracle.
	 *
	 * Each helper is _t1; a driven run forces it onto tier-1:
	 *   MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=1 \
	 *   MONO_LLVM_METHOD='n3_depth3_nested_t1;n6_finally3_t1;n6_catch3_t1;\
	 *     n6_catch_finally_mix_t1;n6_finally4_t1' ./mono -v exceptions.exe
	 * In the regression suite the warm-up loops are inert at the default promotion threshold,
	 * so these stay pure correctness tests with the classic JIT as the differential oracle. The
	 * assertions (finally order across >= 2 enclosers, catch selection/precedence across two
	 * boundaries, exception identity, each finally exactly once) must hold identically on both
	 * tiers. C7ExA/C7ExB/C7ExC(:C7ExB)/C7ExD map via C7Throw 1->A 2->B 3->C 4->D.
	 */

	/*
	 * DEPTH-3 try/finally x3 (doc 21 §4.1, the sharpest ordering test). An uncaught throw in
	 * the innermost try must run the three finallys INNERMOST-FIRST: inner (2), middle (3),
	 * outer (4). The inner try owns the sole landing pad; its base entry synthesises TWO
	 * enclosing finally entries (middle @ clause1, outer @ clause2) appended ascending, so
	 * pass-2 runs 2,3,4 - the descending-order bug would run 2,4,3. mode 0 = no throw (finallys
	 * on the normal leave), 1 = uncaught throw (finallys as cleanup, then propagate).
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n6_finally3_t1 (int mode, int[] log) {
		try {						/* outer FINALLY (clause2) */
			try {					/* middle FINALLY (clause1) */
				try {				/* inner FINALLY (clause0) */
					log [++log [0]] = 1;
					if (mode == 1)
						C7Throw (1);		/* throw C7ExA via a call (invoke edge), uncaught */
				} finally {
					log [++log [0]] = 2;	/* inner */
				}
			} finally {
				log [++log [0]] = 3;		/* middle */
			}
		} finally {
			log [++log [0]] = 4;			/* outer */
		}
	}

	public static int test_0_n6_finally3 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n6_finally3_t1 (0, log);
			log [0] = 0; try { n6_finally3_t1 (1, log); } catch (C7ExA) { }
		}
		/* normal leave: inner, middle, outer -> order 1,2,3,4 */
		log [0] = 0;
		n6_finally3_t1 (0, log);
		if (!(log [0] == 4 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 4))
			return 1;
		/* uncaught: three finallys inner-to-outer as cleanup, then propagate -> order 1,2,3,4 */
		log [0] = 0;
		bool prop = false;
		try { n6_finally3_t1 (1, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 4 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 4))
			return 2;
		return 0;
	}

	/*
	 * DEPTH-3 try/catch precedence across TWO nesting boundaries: inner CATCH(C7ExC, derived)
	 * in middle CATCH(C7ExB, base of C7ExC) in outer CATCH(C7ExA, unrelated). The inner catch
	 * has two enclosers (middle, outer). A throw is routed to the innermost catch whose type
	 * matches, scanning inner -> middle -> outer in array order: C7ExC hits the inner, C7ExB
	 * (base) skips the derived inner and hits the middle, C7ExA skips both and hits the outer,
	 * C7ExD matches none and propagates. mode 1=C7ExC 2=C7ExB 3=C7ExA 4=C7ExD.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n6_catch3_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer CATCH (C7ExA) */
			try {					/* middle CATCH (C7ExB) */
				try {				/* inner CATCH (C7ExC : C7ExB) */
					log [++log [0]] = 1;
					if (mode == 1)
						C7Throw (3);		/* C7ExC -> inner */
					if (mode == 2)
						C7Throw (2);		/* C7ExB -> middle */
					if (mode == 3)
						C7Throw (1);		/* C7ExA -> outer */
					if (mode == 4)
						C7Throw (4);		/* C7ExD -> propagate */
					r = 10;
				} catch (C7ExC) {
					log [++log [0]] = 2;
					r = 20;
				}
			} catch (C7ExB) {
				log [++log [0]] = 3;
				r = 30;
			}
		} catch (C7ExA) {
			log [++log [0]] = 4;
			r = 40;
		}
		return r;
	}

	public static int test_0_n6_catch3 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n6_catch3_t1 (0, log);
			log [0] = 0; n6_catch3_t1 (1, log);
			log [0] = 0; n6_catch3_t1 (2, log);
			log [0] = 0; n6_catch3_t1 (3, log);
			log [0] = 0; try { n6_catch3_t1 (4, log); } catch (C7ExD) { }
		}
		/* no throw -> r=10, order 1 */
		log [0] = 0;
		if (!(n6_catch3_t1 (0, log) == 10 && log [0] == 1 && log [1] == 1))
			return 1;
		/* C7ExC -> inner catch (most derived) -> r=20, order 1,2 */
		log [0] = 0;
		if (!(n6_catch3_t1 (1, log) == 20 && log [0] == 2 && log [1] == 1 && log [2] == 2))
			return 2;
		/* C7ExB -> inner isinst fails, middle base catch across one boundary -> r=30, order 1,3 */
		log [0] = 0;
		if (!(n6_catch3_t1 (2, log) == 30 && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 3;
		/* C7ExA -> inner+middle fail, outer catch across two boundaries -> r=40, order 1,4 */
		log [0] = 0;
		if (!(n6_catch3_t1 (3, log) == 40 && log [0] == 2 && log [1] == 1 && log [2] == 4))
			return 4;
		/* C7ExD -> none match, propagates -> order 1 */
		log [0] = 0;
		bool prop = false;
		try { n6_catch3_t1 (4, log); } catch (C7ExD) { prop = true; }
		if (!(prop && log [0] == 1 && log [1] == 1))
			return 5;
		return 0;
	}

	/*
	 * DEPTH-3 catch/finally MIX: inner CATCH(C7ExB) in middle FINALLY in outer CATCH(C7ExA).
	 * The inner catch owns the landing pad; its enclosers are the middle finally (clause1) and
	 * the outer catch (clause2), appended ascending. The load-bearing case is an uncaught-by-
	 * inner throw (C7ExA): pass-2 must run the intervening middle FINALLY (slot 1) BEFORE it
	 * enters the outer CATCH (slot 2). The descending-order bug would enter the outer catch
	 * without first running the middle finally. mode 1=C7ExB (inner catch) 2=C7ExA (middle
	 * finally then outer catch).
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int n6_catch_finally_mix_t1 (int mode, int[] log) {
		int r = 0;
		try {						/* outer CATCH (C7ExA) */
			try {					/* middle FINALLY */
				try {				/* inner CATCH (C7ExB) */
					log [++log [0]] = 1;
					if (mode == 1)
						C7Throw (2);		/* C7ExB -> inner catch */
					if (mode == 2)
						C7Throw (1);		/* C7ExA -> middle finally then outer catch */
					r = 10;
				} catch (C7ExB) {
					log [++log [0]] = 2;
					r = 20;
				}
			} finally {
				log [++log [0]] = 3;		/* middle finally */
			}
		} catch (C7ExA e) {
			log [++log [0]] = (e is C7ExA) ? 4 : 99;	/* identity check on the routed object */
			r = 40;
		}
		return r;
	}

	public static int test_0_n6_catch_finally_mix () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n6_catch_finally_mix_t1 (0, log);
			log [0] = 0; n6_catch_finally_mix_t1 (1, log);
			log [0] = 0; n6_catch_finally_mix_t1 (2, log);
		}
		/* no throw: inner body, middle finally on leave -> r=10, order 1,3 */
		log [0] = 0;
		if (!(n6_catch_finally_mix_t1 (0, log) == 10 && log [0] == 2 && log [1] == 1 && log [2] == 3))
			return 1;
		/* C7ExB: inner catch, then middle finally on the leave out -> r=20, order 1,2,3 */
		log [0] = 0;
		if (!(n6_catch_finally_mix_t1 (1, log) == 20 && log [0] == 3 && log [1] == 1 && log [2] == 2 && log [3] == 3))
			return 2;
		/* C7ExA: inner skipped, middle finally FIRST, then outer catch (right obj) -> r=40, order 1,3,4 */
		log [0] = 0;
		if (!(n6_catch_finally_mix_t1 (2, log) == 40 && log [0] == 3 && log [1] == 1 && log [2] == 3 && log [3] == 4))
			return 3;
		return 0;
	}

	/*
	 * DEPTH-4 try/finally x4 - stresses the ordering across THREE enclosers of one base. An
	 * uncaught throw runs the four finallys innermost-first: 2,3,4,5. The inner base entry
	 * synthesises three enclosing finally entries (clause1,2,3) ascending, so pass-2 walks them
	 * inner-to-outer. mode 0 = no throw, 1 = uncaught throw.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void n6_finally4_t1 (int mode, int[] log) {
		try {						/* clause3 (outer) */
			try {					/* clause2 */
				try {				/* clause1 */
					try {			/* clause0 (inner) */
						log [++log [0]] = 1;
						if (mode == 1)
							C7Throw (1);	/* C7ExA, uncaught */
					} finally {
						log [++log [0]] = 2;
					}
				} finally {
					log [++log [0]] = 3;
				}
			} finally {
				log [++log [0]] = 4;
			}
		} finally {
			log [++log [0]] = 5;
		}
	}

	public static int test_0_n6_finally4 () {
		int[] log = new int [8];
		for (int i = 0; i < 8; i++) {
			log [0] = 0; n6_finally4_t1 (0, log);
			log [0] = 0; try { n6_finally4_t1 (1, log); } catch (C7ExA) { }
		}
		/* normal leave: 1,2,3,4,5 */
		log [0] = 0;
		n6_finally4_t1 (0, log);
		if (!(log [0] == 5 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 4 && log [5] == 5))
			return 1;
		/* uncaught: four finallys inner-to-outer, then propagate -> 1,2,3,4,5 */
		log [0] = 0;
		bool prop = false;
		try { n6_finally4_t1 (1, log); } catch (C7ExA) { prop = true; }
		if (!(prop && log [0] == 5 && log [1] == 1 && log [2] == 2 && log [3] == 3 && log [4] == 4 && log [5] == 5))
			return 2;
		return 0;
	}

	public static int test_0_byte_cast () {
		int a;
		long l;
		ulong ul;
		byte b = 0;
		bool failed;

		try {
			a = 255;
			failed = false;
			checked {
				b = (byte)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 1;
		if (b != 255)
			return -1;

		try {
			a = 0;
			failed = false;
			checked {
				b = (byte)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;
		if (b != 0)
			return -2;


		try {
			a = 256;
			failed = true;
			checked {
				b = (byte)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 3;
		if (b != 0)
			return -3;

		try {
			a = -1;
			failed = true;
			checked {
				b = (byte)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;
		if (b != 0)
			return -4;

		try {
			double d = 0;
			failed = false;
			checked {
				b = (byte)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 5;
		if (b != 0)
			return -5;
		
		try {
			double d = -1;
			failed = true;
			checked {
				b = (byte)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 6;
		if (b != 0)
			return -6;

		try {
			double d = 255;
			failed = false;
			checked {
				b = (byte)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 7;
		if (b != 255)
			return -7;

		try {
			double d = 256;
			failed = true;
			checked {
				b = (byte)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;
		if (b != 255)
			return -8;

		try {
			l = 255;
			failed = false;
			checked {
				b = (byte)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 9;
		if (b != 255)
			return -9;

		try {
			l = 0;
			failed = false;
			checked {
				b = (byte)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 10;
		if (b != 0)
			return -10;

		try {
			l = 256;
			failed = true;
			checked {
				b = (byte)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 11;
		if (b != 0)
			return -11;

		try {
			l = -1;
			failed = true;
			checked {
				b = (byte)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 12;
		if (b != 0)
			return -12;

		try {
			ul = 256;
			failed = true;
			checked {
				b = (byte)ul;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 13;
		if (b != 0)
			return -13;

		return 0;
	}
	
	public static int test_0_sbyte_cast () {
		int a;
		long l;
		sbyte b = 0;
		bool failed;

		try {
			a = 255;
			failed = true;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 1;
		if (b != 0)
			return -1;

		try {
			a = 0;
			failed = false;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;
		if (b != 0)
			return -2;

		try {
			a = 256;
			failed = true;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 3;
		if (b != 0)
			return -3;

		try {
			a = -129;
			failed = true;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;
		if (b != 0)
			return -4;

		try {
			a = -1;
			failed = false;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 5;
		if (b != -1)
			return -5;

		try {
			a = -128;
			failed = false;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 6;
		if (b != -128)
			return -6;

		try {
			a = 127;
			failed = false;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 7;
		if (b != 127)
			return -7;

		try {
			a = 128;
			failed = true;
			checked {
				b = (sbyte)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;
		if (b != 127)
			return -8;

		try {
			double d = 127;
			failed = false;
			checked {
				b = (sbyte)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 9;
		if (b != 127)
			return -9;

		try {
			double d = -128;
			failed = false;
			checked {
				b = (sbyte)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 10;
		if (b != -128)
			return -10;

		try {
			double d = 128;
			failed = true;
			checked {
				b = (sbyte)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 11;
		if (b != -128)
			return -11;

		try {
			double d = -129;
			failed = true;
			checked {
				b = (sbyte)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 12;
		if (b != -128)
			return -12;

		try {
			l = 255;
			failed = true;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 13;
		if (b != -128)
			return -13;

		try {
			l = 0;
			failed = false;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 14;
		if (b != 0)
			return -14;

		try {
			l = 256;
			failed = true;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 15;
		if (b != 0)
			return -15;

		try {
			l = -129;
			failed = true;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 16;
		if (b != 0)
			return -16;

		try {
			l = -1;
			failed = false;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 17;
		if (b != -1)
			return -17;

		try {
			l = -128;
			failed = false;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 18;
		if (b != -128)
			return -18;

		try {
			l = 127;
			failed = false;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 19;
		if (b != 127)
			return -19;

		try {
			l = 128;
			failed = true;
			checked {
				b = (sbyte)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 20;
		if (b != 127)
			return -20;

		try {
			ulong ul = 128;
			failed = true;
			checked {
				b = (sbyte)ul;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 21;
		if (b != 127)
			return -21;

		return 0;
	}

	public static int test_0_ushort_cast () {
		int a;
		long l;
		ulong ul;
		ushort b;
		bool failed;

		try {
			a = System.UInt16.MaxValue;
			failed = false;
			checked {
				b = (ushort)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 1;

		try {
			a = 0;
			failed = false;
			checked {
				b = (ushort)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;

		try {
			a = System.UInt16.MaxValue + 1;
			failed = true;
			checked {
				b = (ushort)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 3;

		try {
			a = -1;
			failed = true;
			checked {
				b = (ushort)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;

		try {
			double d = 0;
			failed = false;
			checked {
				b = (ushort)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 5;

		try {
			double d = System.UInt16.MaxValue;
			failed = false;
			checked {
				b = (ushort)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 6;

		try {
			double d = -1;
			failed = true;
			checked {
				b = (ushort)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 7;

		try {
			double d = System.UInt16.MaxValue + 1.0;
			failed = true;
			checked {
				b = (ushort)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;

		try {
			l = System.UInt16.MaxValue;
			failed = false;
			checked {
				b = (ushort)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 9;

		try {
			l = 0;
			failed = false;
			checked {
				b = (ushort)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 10;

		try {
			l = System.UInt16.MaxValue + 1;
			failed = true;
			checked {
				b = (ushort)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 11;

		try {
			l = -1;
			failed = true;
			checked {
				b = (ushort)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 12;

		try {
			ul = 0xfffff;
			failed = true;
			checked {
				b = (ushort)ul;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 13;

		return 0;
	}
	
	public static int test_0_short_cast () {
		int a;
		long l;
		short b;
		bool failed;

		try {
			a = System.UInt16.MaxValue;
			failed = true;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			a = 0;
			failed = false;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;

		try {
			a = System.Int16.MaxValue + 1;
			failed = true;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 3;

		try {
			a = System.Int16.MinValue - 1;
			failed = true;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;

		try {
			a = -1;
			failed = false;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 5;

		try {
			a = System.Int16.MinValue;
			failed = false;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 6;

		try {
			a = System.Int16.MaxValue;
			failed = false;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 7;

		try {
			a = System.Int16.MaxValue + 1;
			failed = true;
			checked {
				b = (short)a;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;

		try {
			double d = System.Int16.MaxValue;
			failed = false;
			checked {
				b = (short)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 9;
		
		try {
			double d = System.Int16.MinValue;
			failed = false;
			checked {
				b = (short)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 10;
		
		try {
			double d = System.Int16.MaxValue + 1.0;
			failed = true;
			checked {
				b = (short)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 11;

		try {
			double d = System.Int16.MinValue - 1.0;
			failed = true;
			checked {
				b = (short)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 12;

		try {
			l = System.Int16.MaxValue + 1;
			failed = true;
			checked {
				b = (short)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 13;

		try {
			l = System.Int16.MaxValue;
			failed = false;
			checked {
				b = (short)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 14;

		try {
			l = System.Int16.MinValue - 1;
			failed = true;
			checked {
				b = (short)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 15;

		
		try {
			l = System.Int16.MinValue;
			failed = false;
			checked {
				b = (short)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 16;

		try {
			l = 0x00000000ffffffff;
			failed = true;
			checked {
				b = (short)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 17;

		try {
			ulong ul = 32768;
			failed = true;
			checked {
				b = (short)ul;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 18;

		return 0;
	}
	
	public static int test_0_int_cast () {
		int a;
		long l;
		bool failed;

		try {
			double d = System.Int32.MaxValue + 1.0;
			failed = true;
			checked {
				a = (int)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			double d = System.Int32.MaxValue;
			failed = false;
			checked {
				a = (int)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;
		

		try {
			double d = System.Int32.MinValue;
			failed = false;			
			checked {
				a = (int)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 3;


		try {
			double d =  System.Int32.MinValue - 1.0;
			failed = true;
			checked {
				a = (int)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;

		try {
			l = System.Int32.MaxValue + (long)1;
			failed = true;
			checked {
				a = (int)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 5;

		try {
			l = System.Int32.MaxValue;
			failed = false;
			checked {
				a = (int)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 6;
		

		try {
			l = System.Int32.MinValue;
			failed = false;			
			checked {
				a = (int)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 7;


		try {
			l =  System.Int32.MinValue - (long)1;
			failed = true;
			checked {
				a = (int)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;

		try {
			uint ui = System.UInt32.MaxValue;
			failed = true;
			checked {
				a = (int)ui;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 9;

		try {
			ulong ul = (long)(System.Int32.MaxValue) + 1;
			failed = true;
			checked {
				a = (int)ul;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 10;

		try {
			ulong ul = UInt64.MaxValue;
			failed = true;
			checked {
				a = (int)ul;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 11;

		{
			int i; 
			float f = 1.1f;
			checked {
				i = (int) f;
			}
		}

		return 0;
	}

	public static int test_0_uint_cast () {
		uint a;
		long l;
		bool failed;

		try {
			double d =  System.UInt32.MaxValue;
			failed = false;
			checked {
				a = (uint)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 1;

		try {
			double d = System.UInt32.MaxValue + 1.0;
			failed = true;
			checked {
				a = (uint)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 2;

		try {
			double d = System.UInt32.MinValue;
			failed = false;
			checked {
				a = (uint)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 3;

		try {
			double d = System.UInt32.MinValue - 1.0;
			failed = true;
			checked {
				a = (uint)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;
		
		try {
			l =  System.UInt32.MaxValue;
			failed = false;
			checked {
				a = (uint)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 5;

		try {
			l = System.UInt32.MaxValue + (long)1;
			failed = true;
			checked {
				a = (uint)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 6;

		try {
			l = System.UInt32.MinValue;
			failed = false;
			checked {
				a = (uint)l;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 7;

		try {
			l = System.UInt32.MinValue - (long)1;
			failed = true;
			checked {
				a = (uint)l;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;

		try {
			int i = -1;
			failed = true;
			checked {
				a = (uint)i;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 9;

		{
			uint i; 
			float f = 1.1f;
			checked {
				i = (uint) f;
			}
		}
		
		return 0;
	}
	
	public static int test_0_long_cast () {

		/*
		 * These tests depend on properties of x86 fp arithmetic so they won't work
		 * on other platforms.
		 */
		/*
		long a;
		bool failed;

		try {
			double d = System.Int64.MaxValue - 512.0;
			failed = true;
			checked {
				a = (long)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			double d = System.Int64.MaxValue - 513.0;
			failed = false;
			checked {
				a = (long)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;
		
		try {
			double d = System.Int64.MinValue - 1024.0;
			failed = false;			
			checked {
				a = (long)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 3;

		try {
			double d = System.Int64.MinValue - 1025.0;
			failed = true;
			checked {
				a = (long)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;
		*/

		{
			long i; 
			float f = 1.1f;
			checked {
				i = (long) f;
			}
		}

		return 0;
	}

	/* Github issue 13284 */
	public static int test_0_ulong_ovf_spilling () {
		checked {
			ulong x = 2UL;
			ulong y = 1UL;
			ulong z = 3UL;
			ulong t = x - y;

			try {
				var a = x - y >= z;
				if (a)
					return 1;
				// Console.WriteLine ($"u64 ({x} - {y} >= {z}) => {a} [{(a == false ? "OK" : "NG")}]");
			} catch (OverflowException) {
				return 2;
				// Console.WriteLine ($"u64 ({x} - {y} >= {z}) => overflow [NG]");
			}

			try {
				var a = t >= z;
				if (a)
					return 3;
				// Console.WriteLine ($"u64 ({t} >= {z}) => {a} [{(a == false ? "OK" : "NG")}]");
			} catch (OverflowException) {
				return 4;
				// Console.WriteLine ($"u64 ({t} >= {z}) => overflow [NG]");
			}

			try {
				var a = x - y - z >= 0;
				if (a)
					return 5;
				else
					return 6;
				// Console.WriteLine ($"u64 ({x} - {y} - {z} >= 0) => {a} [NG]");
			} catch (OverflowException) {
				return 0;
				// Console.WriteLine ($"u64 ({x} - {y} - {z} >= 0) => overflow [OK]");
			}
		}
	}

	public static int test_0_ulong_cast () {
		ulong a;
		bool failed;

		/*
		 * These tests depend on properties of x86 fp arithmetic so they won't work
		 * on other platforms.
		 */

		/*
		try {
			double d = System.UInt64.MaxValue - 1024.0;
			failed = true;
			checked {
				a = (ulong)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			double d = System.UInt64.MaxValue - 1025.0;
			failed = false;
			checked {
				a = (ulong)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;
		*/	

		try {
			double d = 0;
			failed = false;			
			checked {
				a = (ulong)d;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 3;

		try {
			double d = -1;
			failed = true;
			checked {
				a = (ulong)d;
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;

		{
			ulong i; 
			float f = 1.1f;
			checked {
				i = (ulong) f;
			}
		}

		try {
			int i = -1;
			failed = true;
			checked {
				a = (ulong)i;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 5;

		try {
			int i = Int32.MinValue;
			failed = true;
			checked {
				a = (ulong)i;
			}
		}
		catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 6;

		return 0;
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static double opaque_double (double d) {
		return d;
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static float opaque_float (float f) {
		return f;
	}

	/*
	 * conv.ovf at the edges of each target's range, on operands the compiler cannot see
	 * through. What makes these worth stating is that the interesting values are the ones
	 * a float cannot hold exactly: the last double below 2^63 is 1024 short of it, and
	 * truncation means a fraction past either end still fits.
	 */
	public static int test_0_checked_float_to_int_boundaries () {
		bool failed;

		try {
			failed = false;
			checked {
				if ((long) opaque_double (9223372036854774784.0) != 9223372036854774784)
					return 1;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 2;

		try {
			failed = false;
			checked {
				if ((long) opaque_double (-9223372036854775808.0) != Int64.MinValue)
					return 3;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 4;

		/* 2^63 itself, and the first double past Int64.MinValue, are both out. */
		try {
			failed = true;
			checked {
				long l = (long) opaque_double (9223372036854775808.0);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 5;

		try {
			failed = true;
			checked {
				long l = (long) opaque_double (-9223372036854777856.0);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 6;

		try {
			failed = true;
			checked {
				long l = (long) opaque_double (Double.NaN);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 7;

		try {
			failed = true;
			checked {
				long l = (long) opaque_double (Double.PositiveInfinity);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 8;

		try {
			failed = false;
			checked {
				if ((ulong) opaque_double (18446744073709549568.0) != 18446744073709549568)
					return 9;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 10;

		try {
			failed = true;
			checked {
				ulong u = (ulong) opaque_double (18446744073709551616.0);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 11;

		/* A negative fraction truncates to zero, which is in range; -1.0 is not. */
		try {
			failed = false;
			checked {
				if ((ulong) opaque_double (-0.5) != 0)
					return 12;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 13;

		try {
			failed = true;
			checked {
				ulong u = (ulong) opaque_double (-1.0);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 14;

		try {
			failed = false;
			checked {
				if ((int) opaque_double (-2147483648.5) != Int32.MinValue)
					return 15;
				if ((int) opaque_double (2147483647.5) != Int32.MaxValue)
					return 16;
				if ((uint) opaque_double (4294967295.5) != UInt32.MaxValue)
					return 17;
				if ((sbyte) opaque_double (-128.9) != -128)
					return 18;
				if ((sbyte) opaque_double (127.9) != 127)
					return 19;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 20;

		try {
			failed = true;
			checked {
				int i = (int) opaque_double (2147483648.0);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 21;

		/*
		 * float32 sources, where the spacing is coarse enough that the last value in
		 * range is Int32.MinValue itself and the next one down is 256 away.
		 */
		try {
			failed = false;
			checked {
				if ((long) opaque_float (-9223372036854775808.0f) != Int64.MinValue)
					return 22;
				if ((int) opaque_float (-2147483648.0f) != Int32.MinValue)
					return 23;
			}
		} catch (OverflowException) {
			failed = true;
		}
		if (failed)
			return 24;

		try {
			failed = true;
			checked {
				int i = (int) opaque_float (-2147483904.0f);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 25;

		try {
			failed = true;
			checked {
				ulong u = (ulong) opaque_float (18446744073709551616.0f);
			}
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 26;

		return 0;
	}

	public static int test_0_simple_double_casts () {

		double d = 0xffffffff;

		if ((uint)d != 4294967295)
			return 1;

		/*
		 * These tests depend on properties of x86 fp arithmetic so they won't work
		 * on other platforms.
		 */
		/*
		d = 0xffffffffffffffff;

		if ((ulong)d != 0)
			return 2;

		if ((ushort)d != 0)
			return 3;
			
		if ((byte)d != 0)
			return 4;
		*/
			
		d = 0xffff;

		if ((ushort)d != 0xffff)
			return 5;
		
		if ((byte)d != 0xff)
			return 6;
			
		return 0;
	}
	
	public static int test_0_div_zero () {
		int d = 1;
		int q = 0;
		int val;
		bool failed;

		try {
			failed = true;
			val = d / q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			failed = true;
			val = d % q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 2;

		try {
			failed = true;
			q = -1;
			d = Int32.MinValue;
			val = d / q;
		} catch (DivideByZeroException) {
			/* wrong exception */
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 3;

		try {
			failed = true;
			q = -1;
			d = Int32.MinValue;
			val = d % q;
		} catch (DivideByZeroException) {
			/* wrong exception */
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;

		return 0;
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static void dummy () {
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static int div_zero_llvm_inner (int i) {
		try {
			// This call make use avoid the 'handler without invoke' restriction in the llvm backend
			dummy ();
			return 5 / i;
		} catch (Exception ex) {
			return 0;
		}
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static long div_zero_llvm_inner_long (long l) {
		try {
			dummy ();
			return (long)5 / l;
		} catch (Exception ex) {
			return 0;
		}
	}

	public static int test_0_div_zero_llvm () {
	    long r = div_zero_llvm_inner (0);
		if (r != 0)
			return 1;
	    r = div_zero_llvm_inner_long (0);
		if (r != 0)
			return 2;
		return 0;
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static int div_overflow_llvm_inner (int i) {
		try {
			dummy ();
			return Int32.MinValue / i;
		} catch (Exception ex) {
			return 0;
		}
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	static long div_overflow_llvm_inner_long (long l) {
		try {
			dummy ();
			return Int64.MinValue / l;
		} catch (Exception ex) {
			return 0;
		}
	}

	public static int test_0_div_overflow_llvm () {
		long r = div_overflow_llvm_inner (-1);
		if (r != 0)
			return 1;
		r = div_overflow_llvm_inner_long ((long)-1);
		if (r != 0)
			return 2;
		return 0;
	}

	public static int return_55 () {
		return 55;
	}

	public static int test_0_cfold_div_zero () {
		// Test that constant folding doesn't cause division by zero exceptions
		if (return_55 () != return_55 ()) {
			int d = 1;
			int q = 0;
			int val;			

			val = d / q;
			val = d % q;

			q = -1;
			d = Int32.MinValue;
			val = d / q;

			q = -1;
			val = d % q;
		}

		return 0;
	}

	public static int test_0_udiv_zero () {
		uint d = 1;
		uint q = 0;
		uint val;
		bool failed;

		try {
			failed = true;
			val = d / q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			failed = true;
			val = d % q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 2;

		return 0;
	}

	public static int test_0_long_div_zero () {
		long d = 1;
		long q = 0;
		long val;
		bool failed;

		try {
			failed = true;
			val = d / q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			failed = true;
			val = d % q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 2;

		try {
			failed = true;
			q = -1;
			d = Int64.MinValue;
			val = d / q;
		} catch (DivideByZeroException) {
			/* wrong exception */
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 3;

		try {
			failed = true;
			q = -1;
			d = Int64.MinValue;
			val = d % q;
		} catch (DivideByZeroException) {
			/* wrong exception */
		} catch (OverflowException) {
			failed = false;
		}
		if (failed)
			return 4;

		return 0;
	}

	public static int test_0_ulong_div_zero () {
		ulong d = 1;
		ulong q = 0;
		ulong val;
		bool failed;

		try {
			failed = true;
			val = d / q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 1;

		try {
			failed = true;
			val = d % q;
		} catch (DivideByZeroException) {
			failed = false;
		}
		if (failed)
			return 2;

		return 0;
	}

	public static int test_0_float_div_zero () {
		double d = 1;
		double q = 0;
		double val;
		bool failed;

		try {
			failed = false;
			val = d / q;
		} catch (DivideByZeroException) {
			failed = true;
		}
		if (failed)
			return 1;

		try {
			failed = false;
			val = d % q;
		} catch (DivideByZeroException) {
			failed = true;
		}
		if (failed)
			return 2;

		return 0;
	}

	public static int test_0_invalid_unbox () {

		int i = 123;
		object o = "Some string";
		int res = 1;
		
		try {
			// Illegal conversion; o contains a string not an int
			i = (int) o;   
		} catch (Exception e) {
			if (i ==123)
				res = 0;
		}

		return res;
	}

	// Test that double[] can't be cast to double (bug #46027)
	public static int test_0_invalid_unbox_arrays () {
		double[] d1 = { 1.0 };
		double[][] d2 = { d1 };
		Array a = d2;

		try {
			foreach (double d in a) {
			}
			return 1;
		}
		catch (InvalidCastException e) {
			return 0;
		}
	}

	/* bug# 42190, at least mcs generates a leave for the return that
	 * jumps out of multiple exception clauses: we used to execute just 
	 * one enclosing finally block.
	 */
	public static int finally_level;
	static void do_something () {
		int a = 0;
		try {
			try {
				return;
			} finally {
				a = 1;
			}
		} finally {
			finally_level++;
		}
	}

	public static int test_2_multiple_finally_clauses () {
		finally_level = 0;
		do_something ();
		if (finally_level == 1)
			return 2;
		return 0;
	}

	public static int test_3_checked_cast_un () {
                ulong i = 0x8000000034000000;
                long j;

		try {
	                checked { j = (long)i; }
		} catch (OverflowException) {
			j = 2;
		}

		if (j != 2)
			return 0;
		return 3;
	}
	
	public static int test_4_checked_cast () {
                long i;
                ulong j;

		unchecked { i = (long)0x8000000034000000;};
		try {
                	checked { j = (ulong)i; }
		} catch (OverflowException) {
			j = 3;
		}

		if (j != 3)
			return 0;
		return 4;
	}

	static readonly int[] mul_dim_results = new int[] {
		0, 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8,
		1, 0, 1, 1, 1, 2, 1, 3, 1, 4, 1, 5, 1, 6, 1, 7, 1, 8,
		2, 0, 2, 1, 2, 8, 
		3, 0, 3, 1, 3, 8, 
		4, 0, 4, 1, 4, 8, 
		5, 0, 5, 1, 5, 2, 5, 3, 5, 4, 5, 5, 5, 6, 5, 7, 5, 8,
		6, 0, 6, 1, 6, 2, 6, 3, 6, 4, 6, 5, 6, 6, 6, 7, 6, 8,
		7, 0, 7, 1, 7, 2, 7, 3, 7, 4, 7, 5, 7, 6, 7, 7, 7, 8,
	};

	public static int test_0_multi_dim_array_access () {
		int [,] a = System.Array.CreateInstance (typeof (int),
			new int [] {3,6}, new int [] {2,2 }) as int[,];
                int x, y;
		int result_idx = 0;
		for (x = 0; x < 8; ++x) {
			for (y = 0; y < 9; ++y) {
				bool got_ex = false;
				try {
					a [x, y] = 1;
				} catch {
					got_ex = true;
				}
				if (got_ex) {
					if (result_idx >= mul_dim_results.Length)
						return -1;
					if (mul_dim_results [result_idx] != x || mul_dim_results [result_idx + 1] != y) {
						return result_idx + 1;
					}
					result_idx += 2;
				}
			}
		}
		if (result_idx == mul_dim_results.Length)
			return 0;
		return 200;
	}

	static void helper_out_obj (out object o) {
		o = (object)"buddy";
	}

	static void helper_out_string (out string o) {
		o = "buddy";
	}

	public static int test_2_array_mismatch () {
		string[] a = { "hello", "world" };
		object[] b = a;
		bool passed = false;

		try {
			helper_out_obj (out b [1]);
		} catch (ArrayTypeMismatchException) {
			passed = true;
		}
		if (!passed)
			return 0;
		helper_out_string (out a [1]);
		if (a [1] != "buddy")
			return 1;
		return 2;
	}

	public static int test_0_ovf1 () {
		int exception = 0;
		
		checked {
			try {
				ulong a =  UInt64.MaxValue - 1;
				ulong t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf2 () {
		int exception = 0;

		checked {
			try {
				ulong a =  UInt64.MaxValue;
				ulong t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf3 () {
		int exception = 0;

		long a = Int64.MaxValue - 1;
		checked {
			try {
				long t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf4 () {
		int exception = 0;

		long a = Int64.MaxValue;
		checked {
			try {
				long t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf5 () {
		int exception = 0;

		ulong a = UInt64.MaxValue - 1;
		checked {
			try {
				ulong t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf6 () {
		int exception = 0;

		ulong a = UInt64.MaxValue;
		checked {
			try {
				ulong t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf7 () {
		int exception = 0;

		long a = Int64.MinValue + 1;
		checked {
			try {
				long t = a--;
			} catch {
				exception = 1;
			}
		}
		return 0;
	}

	public static int test_1_ovf8 () {
		int exception = 0;

		long a = Int64.MinValue;
		checked {
			try {
				long t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf9 () {
		int exception = 0;

		ulong a = UInt64.MinValue + 1;
		checked {
			try {
				ulong t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf10 () {
		int exception = 0;

		ulong a = UInt64.MinValue;
		checked {
			try {
				ulong t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf11 () {
		int exception = 0;

		int a = Int32.MinValue + 1;
		checked {
			try {
				int t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf12 () {
		int exception = 0;

		int a = Int32.MinValue;
		checked {
			try {
				int t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf13 () {
		int exception = 0;

		uint a = 1;
		checked {
			try {
				uint t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf14 () {
		int exception = 0;

		uint a = 0;
		checked {
			try {
				uint t = a--;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf15 () {
		int exception = 0;

		sbyte a = 126;
		checked {
			try {
				sbyte t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf16 () {
		int exception = 0;

		sbyte a = 127;
		checked {
			try {
				sbyte t = a++;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf17 () {
		int exception = 0;

		checked {
			try {
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf18 () {
		int exception = 0;

		int a = 1 << 29;
		checked {
			try {
				int t = a*2;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf19 () {
		int exception = 0;

		int a = 1 << 30;
		checked {
			try {
				int t = a*2;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_0_ovf20 () {
		int exception = 0;

		checked {
			try {
				ulong a = 0xffffffffff;
				ulong t = a*0x0ffffff;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf21 () {
		int exception = 0;

		ulong a = 0xffffffffff;
		checked {
			try {
				ulong t = a*0x0fffffff;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf22 () {
		int exception = 0;

		long a = Int64.MinValue;
		long b = 10;
		checked {
			try {
				long v = a * b;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	public static int test_1_ovf23 () {
		int exception = 0;

		long a = 10;
		long b = Int64.MinValue;
		checked {
			try {
				long v = a * b;
			} catch {
				exception = 1;
			}
		}
		return exception;
	}

	class Broken {
		public static int i;

		static Broken () {
			throw new Exception ("Ugh!");
		}
	
		public static int DoSomething () {
			return i;
		}
	}

	public static int test_0_exception_in_cctor () {
		try {
			Broken.DoSomething ();
		}
		catch (TypeInitializationException) {
			// This will only happen once even if --regression is used
		}
		return 0;
	}

	public static int test_5_regalloc () {
		int i = 0;

		try {
			for (i = 0; i < 10; ++i) {
				if (i == 5)
					throw new Exception ();
			}
		}
		catch (Exception) {
			if (i != 5)
				return i;
		}

		// Check that variables written in catch clauses are volatile
		int j = 0;
		try {
			throw new Exception ();
		}
		catch (Exception) {
			j = 5;
		}
		if (j != 5)
			return 6;

		int k = 0;
		try {
			try {
				throw new Exception ();
			}
			finally {
				k = 5;
			}
		}
		catch (Exception) {
		}
		if (k != 5)
			return 7;

		return i;
	}

	public static void rethrow () {
		try {
			throw new ApplicationException();
		} catch (ApplicationException) {
			try {
				throw new OverflowException();
			} catch (Exception) {
				throw;
			}
		}
	}

	// Test that a rethrow rethrows the correct exception
	public static int test_0_rethrow_nested () {
		try {
			rethrow ();
		} catch (OverflowException) {
			return 0;
		} catch (Exception) {
			return 1;
		}
		return 2;
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	public static void rethrow1 () {
		throw new Exception ();
	}

	[MethodImplAttribute (MethodImplOptions.NoInlining)]
	public static void rethrow2 () {
		rethrow1 ();
		/* This disables tailcall opts */
		Console.WriteLine ();
	}

	[Category ("!BITCODE")]
	public static int test_0_rethrow_stacktrace () {
		// Check that rethrowing an exception preserves the original stack trace
		try {
			try {
				rethrow2 ();
			}
			catch (Exception ex) {
				// Check that each catch clause has its own exception variable
				// If not, the throw below will overwrite the exception used
				// by the rethrow
				try {
					throw new DivideByZeroException ();
				}
				catch (Exception foo) {
				}

				throw;
			}
		}
		catch (Exception ex) {
			if (ex.StackTrace.IndexOf ("rethrow2") != -1)
				return 0;
		}

		return 1;
	}
	
	interface IFace {}
	class Face : IFace {}
		
	public static int test_1_array_mismatch_2 () {
		try {
			object [] o = new Face [1];
			o [0] = 1;
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}
	
	public static int test_1_array_mismatch_3 () {
		try {
			object [] o = new IFace [1];
			o [0] = 1;
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}
	
	public static int test_1_array_mismatch_4 () {
		try {
			object [][] o = new Face [5] [];
			o [0] = new object [5];
			
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	public static int test_0_array_size () {
		bool failed;

		try {
			failed = true;
			int[,] mem2 = new int [Int32.MaxValue, Int32.MaxValue];
		}
		catch (OutOfMemoryException e) {
			failed = false;
		}
		if (failed)
			return 2;

		return 0;
	}

	struct S {
		int i, j, k, l, m, n;
	}

	static IntPtr[] addr;

	static unsafe void throw_func (int i, S s) {
		addr [i] = new IntPtr (&i);
		throw new Exception ();
	}

	/* Test that arguments are correctly popped off the stack during unwinding */
	/* FIXME: Fails on x86 when llvm is enabled (#5432) */
	/*
	public static int test_0_stack_unwind () {
		addr = new IntPtr [1000];
		S s = new S ();
		for (int j = 0; j < 1000; j++) {
			try {
				throw_func (j, s);
			}
			catch (Exception) {
			}
		}
		return (addr [0].ToInt64 () - addr [100].ToInt64 () < 100) ? 0 : 1;
	}
	*/

	static unsafe void get_sp (int i) {
		addr [i] = new IntPtr (&i);
	}

	/* Test that the arguments to the throw trampoline are correctly popped off the stack */
	public static int test_0_throw_unwind () {
		addr = new IntPtr [1000];
		S s = new S ();
		for (int j = 0; j < 1000; j++) {
			try {
				get_sp (j);
				throw new Exception ();
			}
			catch (Exception) {
			}
		}
		return (addr [0].ToInt64 () - addr [100].ToInt64 () < 100) ? 0 : 1;
	}

	public static int test_0_regress_73242 () {
		int [] arr = new int [10];
		for (int i = 0; i < 10; ++i)
			arr [i] = 0;
		try {
			throw new Exception ();
		}
		catch {
		}
		return 0;
    }

	public static int test_0_nullref () {
		try {
			Array foo = null;
			foo.Clone();
		} catch (NullReferenceException e) {
			return 0;
		}
		return 1;
	}

	public int amethod () {
		return 1;
	}

	public static int test_0_nonvirt_nullref_at_clause_start () {
		ExceptionTests t = null;
		try {
			t.amethod ();
		} catch (NullReferenceException) {
			return 0;
		}

		return 1;
	}

	/*
	 * Null-deref shapes that the LLVM ImplicitNullChecks pass folds into faulting
	 * loads in the tier-1 JIT (design doc 25). Each must still raise a catchable
	 * NullReferenceException, identically to the classic explicit-branch path.
	 */
	class NullFoldNode {
		public int val;
		public NullFoldNode next;
	}

	interface INullFold { int Do (); }
	class NullFoldImpl : INullFold { public int x; public virtual int Do () { return x; } }

	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_load (NullFoldNode n) { return n.val; }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static void nullfold_store (NullFoldNode n) { n.val = 5; }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_aload (int[] a) { return a [3]; }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static void nullfold_astore (int[] a) { a [3] = 9; }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_vcall (NullFoldImpl o) { return o.Do (); }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_icall (INullFold o) { return o.Do (); }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_chain (NullFoldNode a) { return a.next.val; }
	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_loop (NullFoldNode n, int iters) {
		int s = 0;
		for (int i = 0; i < iters; i++)
			s += n.val;
		return s;
	}
	[MethodImpl(MethodImplOptions.NoInlining)]
	static int nullfold_deref_after_try (NullFoldNode n) {
		int r = 0;
		try { r = 1; } finally { r = 2; }
		/* deref is OUTSIDE the try: it folds, and the fault must unwind to the caller */
		return r + n.val;
	}

	public static int test_0_nullfold_field_load () {
		try { nullfold_load (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_field_store () {
		try { nullfold_store (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_array_load () {
		try { nullfold_aload (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_array_store () {
		try { nullfold_astore (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_virtual_call () {
		try { nullfold_vcall (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_iface_call () {
		try { nullfold_icall (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_chain () {
		/* a is non-null but a.next is null: the second deref folds */
		try { nullfold_chain (new NullFoldNode ()); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_hot_loop () {
		try { nullfold_loop (null, 1000); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_unwind_to_caller () {
		/* folded fault inside the callee (deref after its try) must unwind out and be caught here */
		try { nullfold_deref_after_try (null); } catch (NullReferenceException) { return 0; }
		return 1;
	}

	public static int test_0_nullfold_message () {
		try {
			nullfold_load (null);
		} catch (NullReferenceException e) {
			/* The hardware/folded null path sets the message without a trailing
			 * period; the explicit-branch path adds one. Normalize before compare. */
			if (e.Message.TrimEnd ('.') == "Object reference not set to an instance of an object")
				return 0;
		}
		return 1;
	}

	public static int throw_only () {
		throw new Exception ();
	}

	[MethodImpl(MethodImplOptions.NoInlining)] 
	public static int throw_only2 () {
		return throw_only ();
	}

	public static int test_0_inline_throw_only () {
		try {
			return throw_only2 ();
		}
		catch (Exception ex) {
			return 0;
		}
	}

	public static string GetText (string s) {
		return s;
	}

	public static int throw_only_gettext () {
		throw new Exception (GetText ("FOO"));
	}

	public static int test_0_inline_throw_only_gettext () {
		object o = null;
		try {
			o = throw_only_gettext ();
		}
		catch (Exception ex) {
			return 0;
		}

		return o != null ? 0 : 1;
	}

	// bug #78633
	public static int test_0_throw_to_branch_opt_outer_clause () {
		int i = 0;

		try {
			try {
				string [] files = new string[1];

				string s = files[2];
			} finally {
				i ++;
			}
		} catch {
		}

		return (i == 1) ? 0 : 1;
	}		

	// bug #485721
	public static int test_0_try_inside_finally_cmov_opt () {
		bool Reconect = false;

		object o = new object ();

		try {
		}
		catch (Exception ExCon) {
			if (o != null)
				Reconect = true;

			try {
			}
			catch (Exception Last) {
			}
		}
		finally {
			if (Reconect == true) {
				try {
				}
				catch (Exception ex) {
				}
			}
		}

		return 0;
	}

	public static int test_0_inline_throw () {
		try {
			inline_throw1 (5);
			return 1;
		} catch {
			return 0;
		}
	}

	// for llvm, the end bblock is unreachable
	public static int inline_throw1 (int i) {
		if (i == 0)
			throw new Exception ();
		else
			return inline_throw2 (i);
	}

	public static int inline_throw2 (int i) {
		throw new Exception ();
	}

	// bug #539550
	public static int test_0_lmf_filter () {
		try {
			// The invoke calls a runtime-invoke wrapper which has a filter clause
#if __MOBILE__
			typeof (ExceptionTests).GetMethod ("lmf_filter").Invoke (null, new object [] { });
#else
			typeof (Tests).GetMethod ("lmf_filter").Invoke (null, new object [] { });
#endif
		} catch (TargetInvocationException) {
		}
		return 0;
	}

    public static void lmf_filter () {
        try {
            Connect ();
        }
        catch {
            throw new NotImplementedException ();
        }
    }

    public static void Connect () {
        Stop ();
        throw new Exception();
    }

    public static void Stop () {
        try {
            lock (null) {}
        }
        catch {
        }
    }

	private static void do_raise () {
		throw new System.Exception ();
	}

	private static int int_func (int i) {
		return i;
	}

	// #559876
	public static int test_8_local_deadce_causes () {
      int myb = 4;
  
      try {
        myb = int_func (8);
        do_raise();
        myb = int_func (2);
      } catch (System.Exception) {
		  return myb;
	  }
	  return 0;
	}

	public static int test_0_except_opt_two_clauses () {
		int size;
		size = -1;
		uint ui = (uint)size;
		try {
			checked {
				uint v = ui * (uint)4;
			}
		} catch (OverflowException e) {
			return 0;
		} catch (Exception) {
			return 1;
		}

		return 2;
	}

    class Child
    {
        public virtual long Method()
        {
            throw new Exception();
        }
    }

	/* #612206 */
	public static int test_100_long_vars_in_clauses_initlocals_opt () {
		Child c = new Child();
		long value = 100; 
		try {
			value = c.Method();
		}
		catch {}
		return (int)value;
	}

	class A {
		public object AnObj;
	}

	public static void DoSomething (ref object o) {
	}

	public static int test_0_ldflda_null () {
		A a = null;

		try {
			DoSomething (ref a.AnObj);
		} catch (NullReferenceException) {
			return 0;
		}

		return 1;
	}

	unsafe struct Foo
	{
		public int i;

		public static Foo* pFoo;
	}

	/* MS.NET doesn't seem to throw in this case */
	public unsafe static int test_0_ldflda_null_pointer () {
		int* pi = &Foo.pFoo->i;

		return 0;
	}

	static int test_0_try_clause_in_finally_clause_regalloc () {
		// Fill up registers with values
		object a = new object ();
		object[] arr1 = new object [1];
		object[] arr2 = new object [1];
		object[] arr3 = new object [1];
		object[] arr4 = new object [1];
		object[] arr5 = new object [1];

		for (int i = 0; i < 10; ++i)
			arr1 [0] = a;
		for (int i = 0; i < 10; ++i)
			arr2 [0] = a;
		for (int i = 0; i < 10; ++i)
			arr3 [0] = a;
		for (int i = 0; i < 10; ++i)
			arr4 [0] = a;
		for (int i = 0; i < 10; ++i)
			arr5 [0] = a;

		int res = 1;
		try {
			try_clause_in_finally_clause_regalloc_inner (out res);
		} catch (Exception) {
		}
		return res;		
	}

	public static object Throw () {
		for (int i = 0; i < 10; ++i)
			;
		throw new Exception ();
	}

	static void try_clause_in_finally_clause_regalloc_inner (out int res) {
		object o = null;

		res = 1;
		try {
			o = Throw ();
		} catch (Exception) {
			/* Make sure this doesn't branch to the finally */
			throw new DivideByZeroException ();
		} finally {
			try {
				/* Make sure o is register allocated */
				if (o == null)
					res = 0;
				else
					res = 1;
				if (o == null)
					res = 0;
				else
					res = 1;
				if (o == null)
					res = 0;
				else
					res = 1;
			} catch (DivideByZeroException) {
			}
		}
	}

    public static bool t_1835_inner () {
        bool a = true;
        if (a) throw new Exception();
        return true;
    }

	[MethodImpl(MethodImplOptions.NoInlining)] 
    public static bool t_1835_inner_2 () {
		bool b = t_1835_inner ();
		return b;
	}

	public static int test_0_inline_retval_throw_in_branch_1835 () {
		try {
			t_1835_inner_2 ();
		} catch {
			return 0;
		}
		return 1;
	}

	static bool finally_called = false;

	static void regress_30472 (int a, int b) {
			checked {
				try {
					int sum = a + b;
				} finally {
					finally_called = true;
				}
            }
		}

	public static int test_0_regress_30472 () {
		finally_called = false;
		try {
		    regress_30472 (Int32.MaxValue - 1, 2);
		} catch (Exception ex) {
		}
		return finally_called ? 0 : 1;
	}

	static int array_len_1 = 1;

	public static int test_0_bounds_check_negative_constant () {
		try {
			byte[] arr = new byte [array_len_1];
			byte b = arr [-1];
			return 1;
		} catch {
		}
		try {
			byte[] arr = new byte [array_len_1];
			arr [-1] = 1;
			return 2;
		} catch {
		}
		return 0;
	}

	public static int test_0_string_bounds_check_negative_constant () {
		try {
			string s = "A";
			char c = s [-1];
			return 1;
		} catch {
		}
		return 0;
	}

	public class MyException : Exception {
		public int marker = 0;
		public string res = "";

		public MyException (String res) {
			this.res = res;
		}

		public bool FilterWithoutState () {
			return this.marker == 0x666;
		}

		public bool FilterWithState () {
			bool ret = this.marker == 0x566;
			this.marker += 0x100;
			return ret;
		}

		public bool FilterWithStringState () {
			bool ret = this.marker == 0x777;
			this.res = "fromFilter_" + this.res;
			return ret;
		}
	}

	[Category ("!BITCODE")]
	public static int test_1_basic_filter_catch () {
		try {
			MyException e = new MyException ("");
			e.marker = 0x1337;
			throw e;
		} catch (MyException ex) when (ex.marker == 0x1337) {
			return 1;
		}
		return 0;
	}

	[Category ("!BITCODE")]
	public static int test_1234_complicated_filter_catch () {
		string res = "init";
		try {
			MyException e = new MyException (res);
			e.marker = 0x566;
			try {
				try {
					throw e;
				} catch (MyException ex) when (ex.FilterWithoutState ()) {
					res = "WRONG_" + res;
				} finally {
					e.marker = 0x777;
					res = "innerFinally_" + res;
				}
			} catch (MyException ex) when (ex.FilterWithState ()) {
				res = "2ndcatch_" + res;
			}
			// "2ndcatch_innerFinally_init"
			// Console.WriteLine ("res1: " + res);
			e.res = res;
			throw e;
		} catch (MyException ex) when (ex.FilterWithStringState ()) {
			res = "fwos_" + ex.res;
		} finally {
			res = "outerFinally_" + res;
		}
		// Console.WriteLine ("res2: " + res);
		return "outerFinally_fwos_fromFilter_2ndcatch_innerFinally_init" == res ? 1234 : 0;
	}

    public struct FooStruct
    {
        public long Part1 { get; }
        public long Part2 { get; }

        public byte Part3 { get; }
    }

    [MethodImpl( MethodImplOptions.NoInlining )]
    private static bool ExceptionFilter( byte x, FooStruct item ) => true;

	[Category ("!BITCODE")]
	public static int test_0_filter_caller_area () {
        try {
            throw new Exception();
        }
        catch (Exception) when (ExceptionFilter (default(byte), default (FooStruct))) {
        }
		return 0;
	}

	public static int test_0_signed_ct_div () {
		int n = 2147483647;
		bool divide_by_zero = false;
		bool overflow = false;

		n = -n;
		n--; /* MinValue */
		try {
			int r = n / (-1);
		} catch (OverflowException) {
			overflow = true;
		}
		if (!overflow)
			return 7;

		try {
			int r = n / 0;
		} catch (DivideByZeroException) {
			divide_by_zero = true;
		}
		if (!divide_by_zero)
			return 8;

		if ((n / 35) != -61356675)
			return 9;
		if ((n / -35) != 61356675)
			return 10;
		n = -(n + 1);  /* MaxValue */
		if ((n / 35) != 61356675)
			return 11;
		if ((n / -35) != -61356675)
			return 12;

		return 0;
	}

	public static int test_0_unsigned_ct_div () {
		uint n = 4294967295;
		bool divide_by_zero = false;

		try {
			uint a = n / 0;
		} catch (DivideByZeroException) {
			divide_by_zero = true;
		}

		if (!divide_by_zero)
			return 5;

		if ((n / 35) != 122713351)
			return 9;

		return 0;
	}

	class C7ExA : Exception { }
	class C7ExB : Exception { }
	class C7ExC : C7ExB { }
	class C7ExD : Exception { }
	class C7ExE : Exception { }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void C7Throw (int which) {
		if (which == 1) throw new C7ExA ();
		if (which == 2) throw new C7ExB ();
		if (which == 3) throw new C7ExC ();
		if (which == 4) throw new C7ExD ();
		if (which == 5) throw new C7ExE ();
	}

	static int C7SibAB (int which) {
		try {
			C7Throw (which);
			return 0;
		} catch (C7ExA) {
			return 11;
		} catch (C7ExB) {
			return 22;
		}
	}

	/* Sibling catches select the handler by type; a subclass hits the base sibling. */
	public static int test_0_sibling_catch_by_type () {
		if (C7SibAB (1) != 11) return 1;   /* C7ExA -> first sibling  */
		if (C7SibAB (2) != 22) return 2;   /* C7ExB -> second sibling */
		if (C7SibAB (3) != 22) return 3;   /* C7ExC : C7ExB -> second */
		if (C7SibAB (0) != 0)  return 4;   /* no throw                */
		return 0;
	}

	static int C7SibDerivedFirst (int which) {
		try {
			C7Throw (which);
			return 0;
		} catch (C7ExC) {
			return 77;
		} catch (C7ExB) {
			return 88;
		}
	}

	/* catch(Derived) catch(Base): the FIRST type-matching clause must win. */
	public static int test_0_sibling_ordering_derived_first () {
		if (C7SibDerivedFirst (3) != 77) return 1;   /* C7ExC -> C7ExC clause (first) */
		if (C7SibDerivedFirst (2) != 88) return 2;   /* C7ExB -> C7ExB clause (second) */
		return 0;
	}

	static int C7Sib3 (int which) {
		try {
			C7Throw (which);
			return 0;
		} catch (C7ExA) {
			return 1;
		} catch (C7ExB) {
			return 2;
		} catch (C7ExD) {
			return 3;
		}
	}

	public static int test_0_sibling_three_way () {
		if (C7Sib3 (1) != 1) return 1;
		if (C7Sib3 (2) != 2) return 2;
		if (C7Sib3 (4) != 3) return 3;
		if (C7Sib3 (0) != 0) return 4;
		return 0;
	}

	/* A throw matching neither sibling propagates to the caller's catch. */
	public static int test_0_sibling_propagate () {
		try {
			C7SibAB (4);   /* C7ExD matches neither -> propagates */
		} catch (C7ExD) {
			return 0;
		}
		return 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void C7RethrowInner (int which) {
		try {
			C7Throw (which);
		} catch (C7ExA) {
			return;
		} catch (C7ExB) {
			throw;   /* rethrow the current exception to the caller */
		}
	}

	/* rethrow from inside a sibling catch re-propagates to an outer frame. */
	public static int test_0_sibling_rethrow () {
		try {
			C7RethrowInner (2);   /* throws C7ExB, inner catch rethrows */
		} catch (C7ExB) {
			return 0;
		}
		return 1;
	}
}

#if !__MOBILE__
class ExceptionTests : Tests
{
}
#endif
