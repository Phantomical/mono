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

#include <cstdint>
#include <memory>
#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include "mini/llvm/engine.hpp"

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
 * so once host_target_machine_builder() pins Large, asking the target machine
 * what model it has can only ever answer "Large" - deleting the pin does not
 * change the answer, because on x86-64 the JIT default is Large as well. The
 * relocations of the emitted object are the observable that actually moves.
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
 *      relocation model. See the exclusion note in engine.cpp.
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
