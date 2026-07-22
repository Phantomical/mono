/*
 * test-llvm-engine.cpp: unit tests for the mono/mini/llvm ORCv2 JIT engine.
 *
 * These used to live inside engine.cpp behind a single extern "C" entry point
 * (mono_llvm_engine_run_selftest), which meant they shipped in libmono in every
 * build and reported one aggregate pass/fail bit for several independent
 * checks. They are now ordinary unit tests: this is the only translation unit
 * that contains them, it is built only under `make check`, and each check is
 * reported on its own line.
 *
 * The file is C++ because it drives the engine through engine.hpp - the C++-only
 * header that mono/mini/llvm/ exposes to this directory - rather than through
 * the deliberately small extern "C" boundary in backend.h, which must not grow
 * to serve tests.
 *
 * It is a real .cpp rather than a .c, even though libtestlib's other sources are
 * C. Two reasons, and the first is not "the others are already C++": they are
 * not. libtestlib_la_CFLAGS is @CXX_ADD_CFLAGS@, which configure fills in only
 * under --enable-cxx (off here and by default), so it is empty and those .c
 * files compile as C.
 *   - automake compiles a mixed C/C++ convenience library without any special
 *     handling; the .lo lands under the same libtestlib_la- prefix as the rest,
 *     so the LDADD lines below are unchanged in shape.
 *   - had --enable-cxx been on, its -xc++ would have dragged CXXFLAGS_COMMON
 *     (-std=gnu++0x -fno-exceptions -fno-rtti) onto this TU, and the first two
 *     are wrong for LLVM 18 headers - llvm_config.mk asks for -std=c++17
 *     -fexceptions. A genuine C++ TU with its own libtestlib_la_CXXFLAGS gets
 *     those flags from llvm_config.mk instead, which is what mono/mini already
 *     does for mono/mini/llvm/*.cpp.
 *
 * A check that cannot run on this host reports SKIP and is counted separately.
 * A skip is never reported as a pass: the relocation-width check is x86-64-ELF
 * only, and claiming a pass elsewhere would be claiming coverage that the
 * relocation classifier does not have.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full
 * license information.
 */

#include "config.h"

#include <stdio.h>

#ifdef ENABLE_LLVM

/*
 * libtool compiles this TU with -DPIC. LLVM headers use `PIC` as an ordinary
 * identifier (PassInstrumentationCallbacks), so the macro would rewrite it and
 * break the header. Same reason, same fix as engine.cpp; nothing here wants
 * mono's PIC macro.
 */
#ifdef PIC
#undef PIC
#endif

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "mini/llvm/engine.hpp"
#include "mini/llvm/lsda.hpp"

using namespace llvm;
using mono::MonoLLVMJIT;

/* ------------------------------------------------------------ harness */

enum TestResult {
	TEST_PASS,
	TEST_FAIL,
	TEST_SKIP
};

static int passes, failures, skips;

/*
 * Report one check. SKIP is deliberately its own outcome rather than a quiet
 * pass, so a host where a check cannot run is visibly uncovered.
 */
static void
report (const char *name, TestResult r)
{
	switch (r) {
	case TEST_PASS:
		passes ++;
		printf ("ok   %s\n", name);
		break;
	case TEST_SKIP:
		skips ++;
		printf ("SKIP %s\n", name);
		break;
	case TEST_FAIL:
		failures ++;
		printf ("FAIL %s\n", name);
		break;
	}
}

#define CHECK(cond)                                                             \
	do {                                                                    \
		if (!(cond)) {                                                  \
			printf ("     check failed: %s (at %s:%d)\n",           \
			        #cond, __FILE__, __LINE__);                     \
			return TEST_FAIL;                                       \
		}                                                               \
	} while (0)

/* ------------------------------------------------------------ arithmetic */

/* int64 add_i64(int64 a, int64 b) { return a + b; } */
static TestResult
test_arithmetic (MonoLLVMJIT *jit)
{
	LLVMContext &ctx = jit->context ();
	auto module = std::make_unique<Module> ("selftest.arith", ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	FunctionType *fty = FunctionType::get (i64, {i64, i64}, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "add_i64", module.get ());
	BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
	IRBuilder<> b (bb);
	auto arg = fn->arg_begin ();
	Value *a = &*arg++;
	Value *c = &*arg;
	b.CreateRet (b.CreateAdd (a, c));

	/* compile() clones the module; our unique_ptr keeps owning the original and
	 * frees it on scope exit - no release(), no leak, no double free. */
	mono::CompileResult res = jit->compile (fn, {}, nullptr, "");
	uint64_t addr = res.entry;
	CHECK (addr != 0);
	/* The size channel must report a real body, not silently zero. */
	CHECK (res.code_size > 0);
	auto compiled = reinterpret_cast<int64_t (*) (int64_t, int64_t)> (addr);
	CHECK (compiled (20, 22) == 42);
	CHECK (compiled (-5, 5) == 0);
	return TEST_PASS;
}

/* ------------------------------------------------------- registered helper */

/*
 * The runtime helper the next check registers. It has internal linkage (static),
 * so it is NOT a process symbol, and it is registered under a deliberately
 * un-mangled name that does not exist anywhere in the process. Consequently: if
 * the engine ever resolved externals via a process-symbol search instead of our
 * explicit register_symbol(), this name would fail to resolve and the lookup
 * inside compile() would abort. A passing call therefore proves the JITed code
 * reached exactly the pointer we registered - never a process symbol.
 */
static int64_t
selftest_helper_impl (int64_t x)
{
	return x * 3 + 7;
}

static const char SELFTEST_HELPER_NAME[] = "mono$selftest$helper$absent_from_process";

/*
 * use_helper(x) = <registered helper>(x) + 1.
 * The IR's external callee is named SELFTEST_HELPER_NAME (absent from the
 * process) and registered to point at selftest_helper_impl (internal linkage).
 * Only our register_symbol() can satisfy that name, so correct results prove the
 * call resolved to the registered pointer, not to any process symbol.
 */
static TestResult
test_registered_helper (MonoLLVMJIT *jit)
{
	jit->register_symbol (SELFTEST_HELPER_NAME, (void *) &selftest_helper_impl);

	LLVMContext &ctx = jit->context ();
	auto module = std::make_unique<Module> ("selftest.helper", ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	FunctionType *helper_ty = FunctionType::get (i64, {i64}, false);
	Function *helper = Function::Create (helper_ty, Function::ExternalLinkage,
	                                     SELFTEST_HELPER_NAME, module.get ());
	FunctionType *fty = FunctionType::get (i64, {i64}, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "use_helper", module.get ());
	BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
	IRBuilder<> b (bb);
	Value *x = &*fn->arg_begin ();
	Value *called = b.CreateCall (helper, {x});
	b.CreateRet (b.CreateAdd (called, ConstantInt::get (i64, 1)));

	jit->optimize (fn); /* also exercise the optimizer */

	/* compile() clones; keep owning the original (freed on scope exit). */
	mono::CompileResult res = jit->compile (fn, {}, nullptr, "");
	uint64_t addr = res.entry;
	CHECK (addr != 0);
	CHECK (res.code_size > 0);
	auto compiled = reinterpret_cast<int64_t (*) (int64_t)> (addr);
	/* selftest_helper_impl(x) = x*3+7, then +1. Only reachable via registration. */
	CHECK (compiled (10) == 38); /* (10*3+7)+1 */
	CHECK (compiled (0) == 8);   /* (0*3+7)+1  */
	return TEST_PASS;
}

/* ---------------------------------------------------------- reloc widths */

/*
 * The probe module the controls below emit. It deliberately forces every way
 * codegen has of materialising an address, because each is affected by the code
 * model differently and a probe that exercises only one gives the negative
 * control a single point of failure:
 *
 *   - an external DATA address       (load of an external global)
 *   - an external CODE address       (call to an external function)
 *   - an internal RODATA address     (a wide switch, so the backend builds a
 *                                     jump table and must address its base -
 *                                     the case that yields R_X86_64_32S under
 *                                     the small model)
 *   - a constant-pool address        (an FP constant with no immediate form)
 *
 * With all four, forcing the small model flags 3 relocations here rather than
 * the 1 a load+call probe produced. Real mono methods flag ~620 apiece, so this
 * is still the weakest link in the check; it is cheap insurance, not parity.
 */
static void
build_reloc_probe (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	Type *i64 = Type::getInt64Ty (ctx);
	Type *dbl = Type::getDoubleTy (ctx);

	auto *gv = new GlobalVariable (m, i64, false, GlobalValue::ExternalLinkage,
	                               nullptr, "selftest_reloc_probe_data");
	FunctionType *fty = FunctionType::get (i64, {i64}, false);
	Function *callee = Function::Create (fty, Function::ExternalLinkage,
	                                     "selftest_reloc_probe_callee", &m);
	Function *fn = Function::Create (fty, Function::ExternalLinkage,
	                                 "selftest_reloc_probe", &m);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *dflt = BasicBlock::Create (ctx, "default", fn);
	IRBuilder<> b (entry);
	Value *arg = &*fn->arg_begin ();
	Value *loaded = b.CreateLoad (i64, gv);

	/* Constant pool: 3.14159... has no immediate encoding. */
	Value *fp = b.CreateFAdd (b.CreateSIToFP (arg, dbl),
	                          ConstantFP::get (dbl, 3.14159265358979));
	Value *fpi = b.CreateFPToSI (fp, i64);

	/*
	 * 40 dense cases is comfortably past the backend's jump-table threshold, so
	 * this becomes a table lookup rather than a compare chain.
	 */
	const unsigned num_cases = 40;
	SwitchInst *sw = b.CreateSwitch (arg, dflt, num_cases);
	for (unsigned i = 0; i < num_cases; ++i) {
		BasicBlock *bb = BasicBlock::Create (ctx, "case" + std::to_string (i), fn);
		IRBuilder<> cb (bb);
		cb.CreateRet (cb.CreateAdd (loaded, ConstantInt::get (i64, i * 7919)));
		sw->addCase (ConstantInt::get (cast<IntegerType> (i64), i), bb);
	}

	IRBuilder<> db (dflt);
	db.CreateRet (db.CreateCall (callee, {db.CreateAdd (loaded, fpi)}));
}

/*
 * Emit the probe module through a target machine that is identical to the
 * engine's except for the code model, and report what the auditor makes of the
 * result.
 */
static Expected<mono::RelocAudit>
audit_code_model (CodeModel::Model cm)
{
	auto jtmb = mono::host_target_machine_builder ();
	jtmb.setCodeModel (cm);
	auto tm = jtmb.createTargetMachine ();
	if (!tm)
		return tm.takeError ();

	LLVMContext ctx;
	Module m ("selftest.reloc", ctx);
	m.setDataLayout ((*tm)->createDataLayout ());
	build_reloc_probe (m);

	SmallVector<char, 0> buf;
	raw_svector_ostream os (buf);
	legacy::PassManager pm;
	if ((*tm)->addPassesToEmitFile (pm, os, nullptr, CodeGenFileType::ObjectFile))
		return createStringError (inconvertibleErrorCode (),
		                          "target cannot emit an object file");
	pm.run (m);

	auto obj = object::ObjectFile::createObjectFile (
		MemoryBufferRef (StringRef (buf.data (), buf.size ()), "selftest.o"));
	if (!obj)
		return obj.takeError ();
	return mono::audit_relocations (**obj);
}

/*
 * Guards the invariant the stock SectionMemoryManager depends on: nothing this
 * engine emits may squeeze a 64-bit address into a 32-bit field.
 *
 * WHY NOT JUST READ BACK THE CODE MODEL. Because that check is vacuous.
 * X86TargetMachine::getEffectiveCodeModel returns a requested model verbatim,
 * and on x86-64 the JIT default is Large anyway, so asking the target machine
 * what model it has can only ever answer "Large" whether or not anything set
 * it. The relocations of the emitted object are the observable that moves.
 *
 * Three parts, in order:
 *
 *   1. NEGATIVE CONTROL. Emit the probe with the code model forced to Small and
 *      require the auditor to flag something. Without this the check could pass
 *      by being blind, which is precisely how its predecessor passed.
 *
 *      WHY THE CHECK IS WIDER THAN R_X86_64_32/32S. Most of what small-model
 *      codegen emits here is NOT an absolute 32-bit relocation: measured over a
 *      603-module corpus of real mono methods forced to Small, the counts were
 *      REX_GOTPCRELX 3199, PC32 617 (603 in .eh_frame, 14 in .text), PLT32 10,
 *      R_X86_64_64 18 - and only 3 R_X86_64_32S. A 32/32S-only check would
 *      therefore have caught 3 modules out of 603 and missed the per-function
 *      .eh_frame PC32 entirely. Hence PC32 is in the set.
 *
 *      Data and code go through the GOT/PLT here despite the JIT's relocation
 *      model being STATIC, not PIC: mono's globals and callees are external and
 *      not dso_local, and ELF lowering routes those indirectly regardless of
 *      relocation model - which is why the GOT/PLT forms (REX_GOTPCRELX, PLT32)
 *      dominate the small-model counts above and are classified truncating; see
 *      x86_64_reloc_truncates_address in engine.cpp.
 *   2. POSITIVE CONTROL. The same probe under Large must come back clean, and
 *      with a non-zero relocation count so a silent "found nothing to look at"
 *      cannot masquerade as a pass.
 *   3. THE JIT'S OWN OUTPUT. The audit accumulated from every object this
 *      process JITted, via the NotifyLoaded hook. Scope this honestly: inside
 *      this test binary the only such objects are the two tiny modules the
 *      checks above built, so this part mostly proves the accumulator is wired
 *      up and reaches the real compile path. That is also why this check must
 *      run LAST. The coverage that matters comes from the same audit running
 *      always-on in the real runtime, where accumulate_reloc_audit() warns on
 *      stderr at the first offender - and it does so from OnLoaded, i.e. before
 *      RTDyld's resolveRelocations performs the truncating write, so that
 *      warning precedes the corruption rather than reporting it afterwards.
 *
 * SKIPPED off x86-64 ELF: the unsafe relocation set is per-ABI and only
 * x86-64's has been analysed (see x86_64_reloc_truncates_address in
 * engine.cpp). Reporting a pass there would be claiming coverage that does not
 * exist - hence TEST_SKIP, which the summary counts separately.
 */
static TestResult
test_reloc_widths (MonoLLVMJIT *jit)
{
	(void) jit;

	Triple host (sys::getProcessTriple ());
	if (host.getArch () != Triple::x86_64 || !host.isOSBinFormatELF ()) {
		printf ("     host %s is not x86-64 ELF; the relocation classifier "
		        "covers only x86-64\n", host.str ().c_str ());
		return TEST_SKIP;
	}

	auto small = audit_code_model (CodeModel::Small);
	if (!small) {
		printf ("     small-model probe: %s\n", toString (small.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (small->truncating > 0);

	auto large = audit_code_model (CodeModel::Large);
	if (!large) {
		printf ("     large-model probe: %s\n", toString (large.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (large->total > 0);
	CHECK (large->truncating == 0);

	mono::RelocAudit jitted = mono::jit_reloc_audit ();
	/* Zero here means "nothing was JITted", which must not pass as "clean". */
	CHECK (jitted.total > 0);
	if (jitted.truncating != 0) {
		printf ("     %llu of %llu relocations in JITted objects truncate a "
		        "64-bit address (first: %s)\n",
		        (unsigned long long) jitted.truncating,
		        (unsigned long long) jitted.total,
		        jitted.first_offender.c_str ());
		return TEST_FAIL;
	}
	return TEST_PASS;
}

/* ------------------------------------------------ .gcc_except_table (EH) */

/*
 * The external callee an EH probe invokes. Declared external and NOT nounwind so
 * neither the optimizer nor ISel may delete the invoke that wraps it - which is
 * what keeps the landingpad, and therefore the .gcc_except_table, alive. At
 * runtime it simply returns; the probe never actually throws, so the landingpad
 * block is emitted (for the table) but never entered.
 */
static void
eh_may_throw_impl (void)
{
}

static const char EH_MAY_THROW_NAME[] = "mono$selftest$eh$may_throw";

/*
 * Build, in `m`, a minimal function with exception handling that MIRRORS the
 * shape emit_handler_start() produces in the translator (translator-call.cpp):
 *
 *   - a call wrapped in an `invoke` with a `landingpad { ptr, i32 }`;
 *   - one `catch` clause referencing a `type_info_0` global (an i32 global whose
 *     value is the IL clause index - the "clause-index smuggling" trick);
 *   - if `with_personality`, the function carries `mono_personality` (an
 *     `i32 (...)` nounwind function returning 0, exactly as
 *     translator-call.cpp:948-958 defines it) via setPersonalityFn.
 *
 * Returns the entry function. `i64 eh_probe(void)` returns 1 on the normal path
 * (may_throw returned) and 2 on the landing pad (never reached at runtime).
 */
static Function *
build_eh_module (Module &m, bool with_personality)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	/* External, possibly-unwinding callee the invoke wraps. */
	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	/*
	 * type_info_0: the clause-index-smuggling global. The translator emits this
	 * with LLVMAddGlobal (external linkage) initialised to the IL clause index;
	 * mirror that. Clause index 0 here.
	 */
	auto *type_info = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                      ConstantInt::get (i32, 0), "type_info_0");

	FunctionType *fty = FunctionType::get (i64, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "eh_probe", &m);

	if (with_personality) {
		/* Mirror translator-call.cpp:948-958: i32 (...) nounwind, returns 0. */
		FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
		Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
		                                   "mono_personality", &m);
		pers->addFnAttr (Attribute::NoUnwind);
		BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
		IRBuilder<> pb (pbb);
		pb.CreateRet (ConstantInt::get (i32, 0));
		fn->setPersonalityFn (pers);
	}

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *cont = BasicBlock::Create (ctx, "cont", fn);
	BasicBlock *lpad = BasicBlock::Create (ctx, "lpad", fn);

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont, lpad, {});

	IRBuilder<> cb (cont);
	cb.CreateRet (ConstantInt::get (i64, 1));

	IRBuilder<> lb (lpad);
	StructType *lp_ty = StructType::get (ptr, i32);
	LandingPadInst *lp = lb.CreateLandingPad (lp_ty, 1);
	lp->addClause (type_info); /* one catch clause -> the type_info global */
	lb.CreateRet (ConstantInt::get (i64, 2));

	return fn;
}

/*
 * Build, in `m`, an EH function with TWO sibling catches - the exact shape mono
 * emits (emit_handler_start, translator-call.cpp:928-1037) and the probe's
 * eh.ll: each catch region is its OWN landing pad carrying a SINGLE catch clause
 * pointing at its OWN type_info_N global. Two invokes of the same possibly-
 * unwinding callee, each unwinding to its own landing pad; the two type_info
 * globals smuggle IL clause indices 7 and 3 (the values probe2 verified the
 * gather recovers). Returns `i32 eh_two_catch(void)`.
 *
 * This is one-landing-pad-per-clause, NOT one landing pad with multiple TypeIds -
 * confirmed to match mono's real emission (each emit_handler_start call builds a
 * fresh landingpad with exactly one LLVMAddClause). The gather nonetheless loops
 * over every positive TypeId, so it would also handle a multi-clause pad.
 */
static Function *
build_eh_two_catch_module (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	/* Two clause-index-smuggling globals: IL clause indices 7 and 3. */
	auto *ti0 = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                ConstantInt::get (i32, 7), "type_info_0");
	auto *ti1 = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                ConstantInt::get (i32, 3), "type_info_1");

	FunctionType *fty = FunctionType::get (i32, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "eh_two_catch", &m);

	/* Mirror translator-call.cpp:948-958: i32 (...) nounwind, returns 0. */
	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   "mono_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *cont1 = BasicBlock::Create (ctx, "cont1", fn);
	BasicBlock *cont2 = BasicBlock::Create (ctx, "cont2", fn);
	BasicBlock *lpad0 = BasicBlock::Create (ctx, "lpad0", fn);
	BasicBlock *lpad1 = BasicBlock::Create (ctx, "lpad1", fn);

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont1, lpad0, {});

	IRBuilder<> b1 (cont1);
	b1.CreateInvoke (callee, cont2, lpad1, {});

	IRBuilder<> b2 (cont2);
	b2.CreateRet (ConstantInt::get (i32, 0));

	StructType *lp_ty = StructType::get (ptr, i32);

	IRBuilder<> l0 (lpad0);
	LandingPadInst *lp0 = l0.CreateLandingPad (lp_ty, 1);
	lp0->addClause (ti0); /* catch -> type_info_0 (clause 7) */
	l0.CreateRet (l0.CreateExtractValue (lp0, 1));

	IRBuilder<> l1 (lpad1);
	LandingPadInst *lp1 = l1.CreateLandingPad (lp_ty, 1);
	lp1->addClause (ti1); /* catch -> type_info_1 (clause 3) */
	l1.CreateRet (l1.CreateExtractValue (lp1, 1));

	return fn;
}

/*
 * Build, in `m`, a single try protecting TWO calls that both unwind to ONE
 * shared catch landing pad - mono's real "try { a(); b(); } catch" shape, where
 * emit_call issues one invoke per protected call all converging on the clause's
 * single handler block. The one LandingPadInfo therefore carries TWO (begin,end)
 * invoke ranges against ONE handler and ONE type_info_0 (IL clause index 5).
 *
 * This is the shape the C2 review's probe_multi.cpp attacks: a gather that keeps
 * only the first range would publish a [try_start,try_end) covering only the
 * first call, so a throw from the second call would escape the handler. The
 * gather must produce TWO clause entries (one per invoke range). Returns
 * `i32 eh_multi_call(void)`.
 *
 * A plain (non-protected) nounwind call is placed BETWEEN the two invokes so
 * their PC ranges are NOT adjacent - otherwise LLVM's EHStreamer coalesces two
 * back-to-back same-landing-pad ranges into a single .gcc_except_table call-site,
 * and the "2 catch call-sites == 2 gathered clauses" cross-check could not
 * distinguish a correct 2-range gather from a bug that dropped one. The gap
 * forces two distinct call-sites, so a dropped range would fail the cross-check.
 */
static Function *
build_eh_multi_call_module (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	/* A nounwind gap call so the two invoke ranges are non-adjacent (see above). */
	Function *gap = Function::Create (callee_ty, Function::ExternalLinkage,
	                                  "mono$selftest$eh$gap", &m);
	gap->addFnAttr (Attribute::NoUnwind);

	auto *ti0 = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                ConstantInt::get (i32, 5), "type_info_0");

	FunctionType *fty = FunctionType::get (i32, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "eh_multi_call", &m);

	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   "mono_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *cont1 = BasicBlock::Create (ctx, "cont1", fn);
	BasicBlock *cont2 = BasicBlock::Create (ctx, "cont2", fn);
	BasicBlock *lpad = BasicBlock::Create (ctx, "lpad", fn); /* ONE shared landing pad */

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont1, lpad, {}); /* call #1 -> lpad */

	IRBuilder<> b1 (cont1);
	b1.CreateCall (gap, {}); /* unprotected gap: separates the two invoke ranges */
	b1.CreateInvoke (callee, cont2, lpad, {}); /* call #2 -> SAME lpad */

	IRBuilder<> b2 (cont2);
	b2.CreateRet (ConstantInt::get (i32, 0));

	StructType *lp_ty = StructType::get (ptr, i32);
	IRBuilder<> l (lpad);
	LandingPadInst *lp = l.CreateLandingPad (lp_ty, 1);
	lp->addClause (ti0); /* one catch clause -> type_info_0 (clause 5) */
	l.CreateRet (l.CreateExtractValue (lp, 1));

	return fn;
}

/*
 * Emit `m` through a target machine IDENTICAL to the engine's (host CPU, O3,
 * LLVM's default JIT code model - Large on x86-64), the same path
 * MonoLLVMJIT::compile drives, and return the object bytes. Used to (a) inspect
 * for a `.gcc_except_table` section and (b) run the relocation audit over the
 * EH-bearing object under the engine's effective code model (R5).
 */
static Expected<std::unique_ptr<MemoryBuffer>>
emit_object_engine_model (Module &m)
{
	auto jtmb = mono::host_target_machine_builder ();
	auto tm = jtmb.createTargetMachine ();
	if (!tm)
		return tm.takeError ();
	m.setDataLayout ((*tm)->createDataLayout ());

	auto buf = std::make_shared<SmallVector<char, 0>> ();
	{
		raw_svector_ostream os (*buf);
		legacy::PassManager pm;
		if ((*tm)->addPassesToEmitFile (pm, os, nullptr, CodeGenFileType::ObjectFile))
			return createStringError (inconvertibleErrorCode (),
			                          "target cannot emit an object file");
		pm.run (m);
	}
	return MemoryBuffer::getMemBufferCopy (StringRef (buf->data (), buf->size ()), "eh.o");
}

/* Raw bytes of the named section of an emitted object, or empty if absent. */
static Expected<std::vector<uint8_t>>
object_section_bytes (MemoryBuffer &buf, StringRef want)
{
	auto obj = object::ObjectFile::createObjectFile (buf.getMemBufferRef ());
	if (!obj)
		return obj.takeError ();
	for (const object::SectionRef &sec : (*obj)->sections ()) {
		Expected<StringRef> name = sec.getName ();
		if (!name) {
			consumeError (name.takeError ());
			continue;
		}
		if (*name != want)
			continue;
		Expected<StringRef> contents = sec.getContents ();
		if (!contents)
			return contents.takeError ();
		return std::vector<uint8_t> (contents->bytes_begin (), contents->bytes_end ());
	}
	return std::vector<uint8_t> ();
}

/* True iff the emitted object carries a non-empty `.gcc_except_table` section. */
static Expected<bool>
object_has_gcc_except_table (MemoryBuffer &buf)
{
	auto obj = object::ObjectFile::createObjectFile (buf.getMemBufferRef ());
	if (!obj)
		return obj.takeError ();
	for (const object::SectionRef &sec : (*obj)->sections ()) {
		Expected<StringRef> name = sec.getName ();
		if (!name) {
			consumeError (name.takeError ());
			continue;
		}
		if (*name == ".gcc_except_table")
			return sec.getSize () > 0;
	}
	return false;
}

/*
 * Plumbing test for M2.1, and the R1 / R5 / M1-decode diagnostics the plan
 * (09-eh-m2-plan.md 8) asks to resolve as a by-product.
 *
 * R1: does LLVM 18 emit `.gcc_except_table` only when the function carries a
 *     personalityFn? Emit the same EH module both ways and compare.
 * Plumbing: with a personality, drive the module through the REAL engine and
 *     assert MonoLLVMJIT::compile captures a non-empty .gcc_except_table into
 *     CompileResult (the M2.1 out-param path).
 * M1-decode: feed the captured bytes to mono::decode_gcc_except_table - a smoke
 *     check that real LLVM-18 output flows through the M1 decoder.
 * R5: run the relocation audit over the EH object under the engine's effective
 *     (Large) code model and report any truncating relocation (gates M2.4).
 */
static TestResult
test_gcc_except_table (MonoLLVMJIT *jit)
{
	Triple host (sys::getProcessTriple ());
	bool x86_64_elf = host.getArch () == Triple::x86_64 && host.isOSBinFormatELF ();

	/* ---- R1: is a personalityFn required for the table / for codegen? ----
	 *
	 * The no-personality module cannot be codegen'd to probe for a table: an
	 * invoke/landingpad without a personality is INVALID IR - the verifier rejects
	 * it, and in this no-asserts LLVM build the SelectionDAG backend segfaults in
	 * lowerEndEH() dereferencing the (null) personality. So R1 is answered by the
	 * authority the plan cites (07 8/R1): the IR verifier. Without a personalityFn
	 * the module is BROKEN; with one it verifies and LLVM emits .gcc_except_table.
	 */
	bool broken_without_pers = false, broken_with_pers = false, table_with_pers = false;
	{
		LLVMContext c1;
		Module m_no ("selftest.eh.nopers", c1);
		build_eh_module (m_no, /*with_personality*/ false);
		broken_without_pers = verifyModule (m_no); /* true == broken */

		LLVMContext c2;
		Module m_yes ("selftest.eh.pers", c2);
		build_eh_module (m_yes, /*with_personality*/ true);
		broken_with_pers = verifyModule (m_yes);

		auto obj_yes = emit_object_engine_model (m_yes);
		if (!obj_yes) {
			printf ("     R1 (with personality) emit failed: %s\n",
			        toString (obj_yes.takeError ()).c_str ());
			return TEST_FAIL;
		}
		auto has_yes = object_has_gcc_except_table (**obj_yes);
		if (!has_yes) {
			printf ("     R1 (with personality) inspect failed: %s\n",
			        toString (has_yes.takeError ()).c_str ());
			return TEST_FAIL;
		}
		table_with_pers = *has_yes;

		printf ("     R1: invoke/landingpad WITHOUT personalityFn -> module %s; "
		        "WITH personalityFn -> module %s and .gcc_except_table emitted=%s\n",
		        broken_without_pers ? "REJECTED by verifier" : "accepted (!)",
		        broken_with_pers ? "REJECTED (!)" : "verifies",
		        table_with_pers ? "YES" : "no");

		/*
		 * Lock the observed R1 answer in: a personalityFn is REQUIRED (the module
		 * is invalid without it), and with it LLVM emits the table. This is what
		 * makes M2.2's LLVMSetPersonalityFn wiring load-bearing.
		 */
		CHECK (broken_without_pers);
		CHECK (!broken_with_pers);
		CHECK (table_with_pers);
	}

	/* ---- Plumbing: capture through the REAL engine (the M2.1 path) ---- */
	jit->register_symbol (EH_MAY_THROW_NAME, (void *) &eh_may_throw_impl);

	LLVMContext &ectx = jit->context ();
	auto emod = std::make_unique<Module> ("selftest.eh.engine", ectx);
	Function *efn = build_eh_module (*emod, /*with_personality*/ true);

	mono::CompileResult res = jit->compile (efn, {}, nullptr, "");
	CHECK (res.entry != 0);
	CHECK (res.code_size > 0);
	/* The whole point of M2.1: the section address+size reach the caller. */
	CHECK (res.gcc_except_table.addr != nullptr);
	CHECK (res.gcc_except_table.size > 0);

	/* The normal path runs (may_throw returns); landing pad is never entered. */
	auto compiled = reinterpret_cast<int64_t (*) (void)> (res.entry);
	CHECK (compiled () == 1);

	/* ---- M1-decode assertion on the captured bytes ---- */
	{
		/*
		 * Under the engine's effective (Large) code model, real LLVM-18 emits the
		 * TType table as DW_EH_PE_absptr (0x00): 8-byte ABSOLUTE ttype entries
		 * relocated with R_X86_64_64 (which is why R5 finds no truncating reloc) -
		 * not the DW_EH_PE_udata4 (0x03, 4-byte) form clang -mcmodel=small produces.
		 * Slice M2.1b extended M1 (lsda.cpp) to accept absptr, so the real captured
		 * table now DECODES. Assert that here: the decoder must accept it and report
		 * ttype_encoding == 0x00 (M2.3 reads that to size the 8-byte entries when it
		 * dereferences the loaded ttype table).
		 */
		mono::ParsedLsda parsed;
		bool decoded = mono::decode_gcc_except_table (
			res.gcc_except_table.addr, (std::size_t) res.gcc_except_table.size, parsed);
		printf ("     M1 decode: %s (ttype_enc=0x%02x cs_enc=0x%02x "
		        "call_sites=%zu has_ttype=%d)\n",
		        decoded ? "OK" : "DECLINED",
		        parsed.ttype_encoding, parsed.call_site_encoding,
		        parsed.call_sites.size (), (int) parsed.has_ttype_table);
		CHECK (decoded);
		CHECK (parsed.ttype_encoding == 0x00);
	}

	/* ---- R5: relocation audit over the EH object under the engine model ---- */
	if (!x86_64_elf) {
		printf ("     R5: host %s is not x86-64 ELF; reloc classifier not run\n",
		        host.str ().c_str ());
	} else {
		LLVMContext rc;
		Module rmod ("selftest.eh.reloc", rc);
		build_eh_module (rmod, /*with_personality*/ true);
		auto robj = emit_object_engine_model (rmod);
		if (!robj) {
			printf ("     R5 emit failed: %s\n", toString (robj.takeError ()).c_str ());
			return TEST_FAIL;
		}
		auto obj = object::ObjectFile::createObjectFile ((*robj)->getMemBufferRef ());
		if (!obj) {
			printf ("     R5 parse failed: %s\n", toString (obj.takeError ()).c_str ());
			return TEST_FAIL;
		}
		mono::RelocAudit audit = mono::audit_relocations (**obj);
		if (audit.truncating == 0) {
			printf ("     R5: EH object under the engine code model produces NO "
			        "truncating relocation (%llu total). type_info/LSDA relocs are "
			        "64-bit-safe.\n", (unsigned long long) audit.total);
		} else {
			printf ("     R5: *** BLOCKING for M2.4 *** EH object produces %llu of "
			        "%llu TRUNCATING relocations (first: %s). type_info/LSDA reads "
			        "may corrupt under the effective code model.\n",
			        (unsigned long long) audit.truncating,
			        (unsigned long long) audit.total,
			        audit.first_offender.c_str ());
		}
	}

	return TEST_PASS;
}

/* ------------------------------------------ compiler equivalence (C1) */

/*
 * C1 acceptance check: the engine's MonoIRCompiler (engine.cpp) must be an
 * observably-inert drop-in for LLJIT's default object-emission compiler. It
 * hand-inlines LLVMTargetMachine::addPassesToEmitMC so the EH port can later
 * splice a MachineFunctionPass and a custom MCStreamer into that pipeline; C1
 * adds neither, so its output must be BYTE-IDENTICAL to the stock
 * TMOwningSimpleCompiler's for the same module and the same target-machine
 * options.
 *
 * The comparison drives both compilers through engine.hpp's two hooks, which
 * build their TargetMachines from the SAME host JITTargetMachineBuilder the
 * engine JITs with - so code model (Large), host CPU/features and O3 match. The
 * module is the reloc probe: a rich NON-EH function (external data + call, a
 * jump table, a constant pool) so the check exercises real codegen rather than a
 * trivial add. It is built twice in separate LLVMContexts because the legacy
 * codegen pipeline runs destructively over the IR - each compiler gets a
 * pristine copy.
 */
static TestResult
test_compiler_equivalence (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m1 ("selftest.equiv", c1);
	build_reloc_probe (m1);
	auto mono_obj = mono::compile_object_with_mono_compiler (m1);
	if (!mono_obj) {
		printf ("     MonoIRCompiler emit failed: %s\n",
		        toString (mono_obj.takeError ()).c_str ());
		return TEST_FAIL;
	}

	LLVMContext c2;
	Module m2 ("selftest.equiv", c2);
	build_reloc_probe (m2);
	auto simple_obj = mono::compile_object_with_simple_compiler (m2);
	if (!simple_obj) {
		printf ("     SimpleCompiler emit failed: %s\n",
		        toString (simple_obj.takeError ()).c_str ());
		return TEST_FAIL;
	}

	StringRef a = (*mono_obj)->getBuffer ();
	StringRef b = (*simple_obj)->getBuffer ();

	if (a == b) {
		printf ("     MonoIRCompiler output is byte-identical to the stock "
		        "SimpleCompiler (%zu bytes)\n", (size_t) a.size ());
		return TEST_PASS;
	}

	printf ("     MonoIRCompiler output DIFFERS from SimpleCompiler "
	        "(mono=%zu bytes, simple=%zu bytes)\n",
	        (size_t) a.size (), (size_t) b.size ());
	return TEST_FAIL;
}

/* ------------------------------------------- EH clause gather (C2) */

/*
 * C2 acceptance check: MonoEHGatherPass, scheduled after addMachinePasses() in
 * MonoIRCompiler's pipeline, gathers the per-landing-pad {try range, handler,
 * clause_index} into the side channel and EMITS NOTHING.
 *
 * The module is the two-sibling-catch shape mono actually emits (each catch is
 * its own landing pad with a single type_info_N global): two landing pads
 * smuggling IL clause indices 7 and 3. gather_eh_sidechannel drives the very
 * pass the runtime path runs and hands the side channel back, so:
 *
 *   1. one EH-bearing function, two (landing pad, clause) tuples, every clause
 *      resolved and carrying valid begin/end/handler symbols;
 *   2. the recovered clause indices are exactly {3, 7} - the smuggled values,
 *      matching probe2's independently-verified gather;
 *   3. no filter, no decline.
 *
 * TWO-SOURCE CROSS-CHECK. The same module still gets LLVM's redundant
 * .gcc_except_table. It is decoded OFFLINE with the committed M1 decoder
 * (mono::decode_gcc_except_table) and the STRUCTURAL geometry is asserted to
 * agree: the number of call sites that reach a landing pad via a catch action
 * equals the number of (landing pad, clause) tuples the pass gathered. The
 * .gcc_except_table's ttype->clause_index mapping needs the RELOCATED ttype
 * table (the entries are R_X86_64_64 relocations, zero in an offline object), so
 * that half is not resolvable here; the clause INDICES are cross-checked directly
 * from the gathered type_info globals instead (which is exactly where the runtime
 * reader will get them). This independent agreement is the proof the gather reads
 * the right landing pads.
 */
static TestResult
test_eh_gather (MonoLLVMJIT *jit)
{
	(void) jit;

	/* ---- gather through the MonoIRCompiler pipeline ---- */
	LLVMContext c1;
	Module m1 ("selftest.eh.gather", c1);
	build_eh_two_catch_module (m1);

	auto sc = mono::gather_eh_sidechannel (m1);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}

	/* Exactly one EH-bearing function (mono_personality has no landing pads). */
	CHECK (sc->functions.size () == 1);
	const mono::MonoEHFunctionClauses &fn = sc->functions[0];
	CHECK (fn.function == "eh_two_catch");
	CHECK (!fn.has_filter);
	CHECK (!fn.declined);
	CHECK (fn.clauses.size () == 2);

	/* Every gathered clause resolved and carries the three .text symbols C3 needs. */
	for (const mono::MonoEHClause &cl : fn.clauses) {
		CHECK (cl.clause_resolved);
		CHECK (cl.try_begin != nullptr);
		CHECK (cl.try_end != nullptr);
		CHECK (cl.handler != nullptr);
	}

	/* The smuggled clause indices are {3, 7} (order is landing-pad order; assert
	 * as a set so a codegen reorder cannot make this flaky). */
	std::vector<int> got = { fn.clauses[0].clause_index, fn.clauses[1].clause_index };
	std::sort (got.begin (), got.end ());
	CHECK (got[0] == 3);
	CHECK (got[1] == 7);

	/* ---- two-source cross-check against the redundant .gcc_except_table ---- */
	LLVMContext c2;
	Module m2 ("selftest.eh.gather.xcheck", c2);
	build_eh_two_catch_module (m2);

	auto obj = emit_object_engine_model (m2);
	if (!obj) {
		printf ("     cross-check emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}
	auto bytes = object_section_bytes (**obj, ".gcc_except_table");
	if (!bytes) {
		printf ("     cross-check section read failed: %s\n",
		        toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (!bytes->empty ());

	mono::ParsedLsda parsed;
	bool decoded = mono::decode_gcc_except_table (bytes->data (), bytes->size (), parsed);
	CHECK (decoded);

	/*
	 * Structural geometry: count call sites that reach a landing pad with a catch
	 * action. That must equal the number of (landing pad, clause) tuples gathered.
	 */
	std::size_t catch_sites = 0;
	for (const mono::LsdaCallSite &cs : parsed.call_sites) {
		bool has_catch = false;
		for (std::int32_t ti : cs.ttype_indices)
			if (ti > 0)
				has_catch = true;
		if (cs.landing_pad != 0 && has_catch)
			catch_sites ++;
	}
	printf ("     cross-check: gather=%zu clause(s); .gcc_except_table=%zu catch "
	        "call-site(s) of %zu call site(s) [geometry agreement; clause indices "
	        "checked from gathered globals, not the offline ttype table]\n",
	        fn.clauses.size (), catch_sites, parsed.call_sites.size ());
	CHECK (catch_sites == fn.clauses.size ());

	return TEST_PASS;
}

/*
 * C2 regression for the review's blocking bug: a landing pad carries one
 * (begin,end) pair PER INVOKE that unwinds to it, so a single try protecting TWO
 * calls that share one catch landing pad must gather TWO clause entries (one per
 * invoke range) - same handler, same clause_index, DIFFERENT ranges. A gather
 * that kept only the first range would drop the second call's PC range and let a
 * throw from it escape the handler at C6.
 *
 * The .gcc_except_table geometry cross-check is applied to THIS shape precisely
 * because a dropped range would break it: the table has two catch call-sites, so
 * a one-range gather would report 1 clause against 2 call-sites and fail here.
 */
static TestResult
test_eh_gather_multi_call (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m1 ("selftest.eh.multicall", c1);
	build_eh_multi_call_module (m1);

	auto sc = mono::gather_eh_sidechannel (m1);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}

	CHECK (sc->functions.size () == 1);
	const mono::MonoEHFunctionClauses &fn = sc->functions[0];
	CHECK (fn.function == "eh_multi_call");
	CHECK (!fn.has_filter);
	CHECK (!fn.declined);

	/* ONE landing pad, TWO invoke ranges -> TWO gathered clauses. */
	CHECK (fn.clauses.size () == 2);

	const mono::MonoEHClause &c0 = fn.clauses[0];
	const mono::MonoEHClause &c1v = fn.clauses[1];

	/* Both resolved to the SAME clause index (5) and the SAME handler symbol... */
	CHECK (c0.clause_resolved && c1v.clause_resolved);
	CHECK (c0.clause_index == 5);
	CHECK (c1v.clause_index == 5);
	CHECK (c0.handler != nullptr);
	CHECK (c0.handler == c1v.handler);

	/* ...but DIFFERENT (begin,end) invoke ranges - the whole point of the fix. */
	CHECK (c0.try_begin != nullptr && c0.try_end != nullptr);
	CHECK (c1v.try_begin != nullptr && c1v.try_end != nullptr);
	CHECK (c0.try_begin != c1v.try_begin);
	CHECK (c0.try_end != c1v.try_end);

	/* ---- cross-check: .gcc_except_table catch call-sites must equal 2 ---- */
	LLVMContext c2;
	Module m2 ("selftest.eh.multicall.xcheck", c2);
	build_eh_multi_call_module (m2);

	auto obj = emit_object_engine_model (m2);
	if (!obj) {
		printf ("     cross-check emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}
	auto bytes = object_section_bytes (**obj, ".gcc_except_table");
	if (!bytes) {
		printf ("     cross-check section read failed: %s\n",
		        toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (!bytes->empty ());

	mono::ParsedLsda parsed;
	bool decoded = mono::decode_gcc_except_table (bytes->data (), bytes->size (), parsed);
	CHECK (decoded);

	std::size_t catch_sites = 0;
	for (const mono::LsdaCallSite &cs : parsed.call_sites) {
		bool has_catch = false;
		for (std::int32_t ti : cs.ttype_indices)
			if (ti > 0)
				has_catch = true;
		if (cs.landing_pad != 0 && has_catch)
			catch_sites ++;
	}
	printf ("     multi-call: gather=%zu range(s) on one pad; .gcc_except_table=%zu "
	        "catch call-site(s) of %zu [a dropped range would fail this]\n",
	        fn.clauses.size (), catch_sites, parsed.call_sites.size ());
	CHECK (catch_sites == fn.clauses.size ());

	return TEST_PASS;
}

/* ------------------------------------------------------------ driver */

/*
 * TWO ENTRY POINTS, ON PURPOSE - and the split is exactly the skip boundary.
 *
 * automake's parallel-tests harness resolves one result per TESTS program, and
 * its SKIP result comes from a program exiting 77. So a check that can be
 * skipped only shows up as a skip in the suite summary (and in the
 * TestResult-unit-tests.xml that check-local builds from it) if it OWNS a
 * program. Left aggregated, a skipped reloc-widths reported "# SKIP: 0" and a
 * fully green board, with the skip visible only in test-llvm-engine.log - which
 * automake writes but nothing in this repo reads, because parallel-tests
 * concatenates only FAILING logs into test-suite.log. That is precisely the
 * "green board hiding an untested property" this file exists to eliminate.
 *
 * Hence: reloc-widths gets its own program (test-llvm-reloc-widths) so a skip
 * becomes "SKIP: test-llvm-reloc-widths" and "# SKIP: 1".
 *
 * arithmetic and registered-helper stay aggregated here. They have no skip path
 * - they either JIT and return the right answer or they fail - so per-program
 * granularity would buy nothing over the per-line ok/FAIL they already print,
 * and it is the same shape test-llvm-ehframe.c uses for its seven checks.
 */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_engine_main (void);

int
test_llvm_engine_main (void)
{
	MonoLLVMJIT *jit = MonoLLVMJIT::get_singleton ();

	passes = failures = skips = 0;

	report ("arithmetic", test_arithmetic (jit));
	report ("registered-helper", test_registered_helper (jit));
	report ("compiler-equivalence", test_compiler_equivalence (jit));
	report ("gcc-except-table", test_gcc_except_table (jit));
	report ("eh-gather", test_eh_gather (jit));
	report ("eh-gather-multi-call", test_eh_gather_multi_call (jit));

	printf ("%d passed, %d skipped, %d failed\n", passes, skips, failures);
	return failures ? 1 : 0;
}

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_reloc_widths_main (void);

int
test_llvm_reloc_widths_main (void)
{
	MonoLLVMJIT *jit = MonoLLVMJIT::get_singleton ();
	TestResult r;

	passes = failures = skips = 0;

	/*
	 * FIXTURES, not checks. Part 3 of reloc-widths audits the objects this
	 * process has JITted, and an empty audit is a "did not look" that the check
	 * fails on rather than passing - so this process must actually JIT
	 * something first. These are the same two modules test-llvm-engine builds;
	 * their own assertions are reported there, and a failure here means the
	 * reloc audit has nothing trustworthy to inspect.
	 */
	if (test_arithmetic (jit) != TEST_PASS || test_registered_helper (jit) != TEST_PASS) {
		printf ("FAIL reloc-widths: could not JIT the fixture modules; see "
		        "test-llvm-engine for the specific failure\n");
		return 1;
	}

	r = test_reloc_widths (jit);
	report ("reloc-widths", r);
	printf ("%d passed, %d skipped, %d failed\n", passes, skips, failures);

	/* 77 is automake's SKIP exit status; see the note above. */
	if (r == TEST_SKIP)
		return 77;
	return r == TEST_FAIL ? 1 : 0;
}

#endif /* ENABLE_LLVM */
