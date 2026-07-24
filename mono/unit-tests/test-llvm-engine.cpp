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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/JITLink/x86_64.h>
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
#include <llvm/Object/ELFObjectFile.h>
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

/* ------------------------------------------------------------ slab residency */

/*
 * Read one "Key:" line from /proc/self/status and return its value in bytes
 * (the file reports memory sizes in kB). Returns 0 when the field or the file
 * is unavailable - the caller treats that as "not measurable" (a skip), not a
 * failure.
 */
static uint64_t
read_proc_status_bytes (const char *key)
{
	FILE *f = fopen ("/proc/self/status", "r");
	if (!f)
		return 0;
	char line[256];
	size_t key_len = strlen (key);
	uint64_t value_kb = 0;
	while (fgets (line, sizeof (line), f)) {
		if (strncmp (line, key, key_len) == 0) {
			value_kb = strtoull (line + key_len, nullptr, 10);
			break;
		}
	}
	fclose (f);
	return value_kb * 1024;
}

/*
 * The engine backs its object-linking layer with a bounded (~2 GiB)
 * MapperJITLinkMemoryManager slab reservation (engine.cpp,
 * kSlabReservationGranularity). That reservation is a single PROT_READ|WRITE
 * anonymous mmap; Linux only backs its pages with physical frames as code is
 * emitted into them. This test pins down that "resident on demand" property,
 * which the design flagged as inferred from Linux's demand-paging contract
 * rather than independently confirmed:
 *
 *   - reserving the slab grows the process's VIRTUAL size by ~2 GiB - proving
 *     the reservation actually happens, so the resident-size check below is not
 *     passing vacuously; and
 *   - compiling a handful of small methods into it grows RESIDENT size (VmRSS)
 *     by only O(MB), NOT by the whole ~2 GiB.
 *
 * Runs FIRST in the suite so its first compile () is the process's first
 * compile - which is what triggers the slab reservation - making the
 * virtual-size jump observable here rather than in an earlier test.
 */
static TestResult
test_slab_residency (MonoLLVMJIT *jit)
{
	uint64_t vmsize_before = read_proc_status_bytes ("VmSize:");
	uint64_t vmrss_before = read_proc_status_bytes ("VmRSS:");
	if (vmsize_before == 0 || vmrss_before == 0) {
		printf ("     /proc/self/status unavailable; residency not measurable\n");
		return TEST_SKIP;
	}

	LLVMContext &ctx = jit->context ();
	const int num_methods = 16;
	for (int i = 0; i < num_methods; i++) {
		auto module = std::make_unique<Module> ("selftest.slab", ctx);
		Type *i64 = Type::getInt64Ty (ctx);
		FunctionType *fty = FunctionType::get (i64, {i64}, false);
		std::string name = "slab_fn_" + std::to_string (i);
		Function *fn = Function::Create (fty, Function::ExternalLinkage, name,
		                                 module.get ());
		BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
		IRBuilder<> b (bb);
		Value *arg = &*fn->arg_begin ();
		b.CreateRet (b.CreateAdd (arg, ConstantInt::get (i64, i)));

		mono::CompileResult res = jit->compile (fn, {}, nullptr, "");
		CHECK (res.entry != 0);
		auto compiled = reinterpret_cast<int64_t (*) (int64_t)> (res.entry);
		CHECK (compiled (100) == 100 + i);
	}

	uint64_t vmsize_after = read_proc_status_bytes ("VmSize:");
	uint64_t vmrss_after = read_proc_status_bytes ("VmRSS:");

	uint64_t vmsize_delta = vmsize_after > vmsize_before ? vmsize_after - vmsize_before : 0;
	uint64_t vmrss_delta = vmrss_after > vmrss_before ? vmrss_after - vmrss_before : 0;

	printf ("     reserved slab: VmSize +%llu MiB, VmRSS +%llu KiB across %d compiles\n",
	        (unsigned long long) (vmsize_delta >> 20),
	        (unsigned long long) (vmrss_delta >> 10),
	        num_methods);

	/* The reservation happened: a slab-sized (~2 GiB) chunk of address space
	 * appeared. A 1 GiB floor tolerates the exact granularity while still
	 * ruling out the old per-method-mmap behaviour (which would move VmSize by
	 * only kilobytes here). */
	CHECK (vmsize_delta >= ((uint64_t) 1 << 30));

	/* ...but the slab is resident on demand: only the emitted code faulted in,
	 * so RSS grew by megabytes - nowhere near the ~2 GiB reserved. The ceiling
	 * sits far below the reservation to make "O(MB), not O(GiB)" unambiguous. */
	CHECK (vmrss_delta < ((uint64_t) 256 << 20));

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

/* -------------------------------------------------------- owner lifetime */

/* i64 <fn_name>(void) { return retval; }, compiled under `owner`. */
static mono::CompileResult
compile_trivial_under_owner (MonoLLVMJIT *jit, const char *fn_name, int64_t retval, void *owner)
{
	LLVMContext &ctx = jit->context ();
	auto module = std::make_unique<Module> (std::string ("selftest.owner.") + fn_name, ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	FunctionType *fty = FunctionType::get (i64, {}, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, fn_name, module.get ());
	BasicBlock *bb = BasicBlock::Create (ctx, "entry", fn);
	IRBuilder<> b (bb);
	b.CreateRet (ConstantInt::get (i64, retval));
	return jit->compile (fn, {}, nullptr, "", owner);
}

/*
 * MonoLLVMJIT::release_owner drives the map in engine.hpp's owners_: one
 * opaque owner key -> the JITDylibs compiled under it, erased (not merely
 * decremented) as soon as it is released. Five properties, none of which the
 * existing checks touch (they all pass owner=nullptr, i.e. "never reclaim"):
 *
 *   (a) one dylib under a fresh owner -> release reports exactly 1;
 *   (b) releasing that SAME (now-erased) key again reports 0, not a second 1 -
 *       the map entry must be erased, not left present with an empty vector,
 *       and there must be no double-counting of an already-torn-down dylib;
 *   (c) two compiles under the SAME owner key before any release -> one
 *       release reports 2 (both dylibs torn down together, as engine.hpp's
 *       "everything compiled under one key is torn down together" promises);
 *   (d) both a nullptr owner and a fresh, never-passed-to-compile() owner
 *       report 0 without crashing (nullptr is the documented no-op; a key
 *       that was simply never used must be an equally quiet no-op, not a
 *       lookup into never-initialised state);
 *   (e) releasing owners A/B/C must not disturb a still-live owner D: D's
 *       compiled code must still resolve and execute correctly after A/B/C
 *       are torn down, proving release_owner () removes only ITS OWN key's
 *       dylibs and nothing from a sibling entry in the map.
 */
static TestResult
test_release_owner (MonoLLVMJIT *jit)
{
	/* Five distinct local objects give five distinct, guaranteed-unique
	 * addresses to use as opaque owner keys - exactly mono's usage, which
	 * keys by a MonoDomain *. */
	int tag_a = 0, tag_b = 0, tag_c = 0, tag_d = 0, tag_unused = 0;
	void *owner_a = &tag_a;
	void *owner_b = &tag_b;
	void *owner_c = &tag_c;
	void *owner_d = &tag_d;
	void *owner_fresh = &tag_unused; /* never passed to compile() */

	/* (a) one dylib under owner_a. */
	mono::CompileResult res_a = compile_trivial_under_owner (jit, "owner_a_fn", 111, owner_a);
	CHECK (res_a.entry != 0);
	CHECK (jit->release_owner (owner_a) == 1);

	/* (b) the same key again: erased, not double-counted. */
	CHECK (jit->release_owner (owner_a) == 0);

	/* (c) two dylibs under the SAME owner_b -> one release reports 2. */
	mono::CompileResult res_b1 = compile_trivial_under_owner (jit, "owner_b_fn1", 222, owner_b);
	mono::CompileResult res_b2 = compile_trivial_under_owner (jit, "owner_b_fn2", 333, owner_b);
	CHECK (res_b1.entry != 0);
	CHECK (res_b2.entry != 0);
	CHECK (jit->release_owner (owner_b) == 2);

	/* (d) nullptr and a never-used key are quiet no-ops. */
	CHECK (jit->release_owner (nullptr) == 0);
	CHECK (jit->release_owner (owner_fresh) == 0);

	/* (e) isolation: compile under owner_c and owner_d, release owner_c (and
	 * the already-erased owner_a/owner_b again for good measure), then prove
	 * owner_d's code is untouched - both that release_owner (owner_d) still
	 * correctly reports 1 dylib, and that the code itself still runs. */
	mono::CompileResult res_c = compile_trivial_under_owner (jit, "owner_c_fn", 444, owner_c);
	CHECK (res_c.entry != 0);

	mono::CompileResult res_d = compile_trivial_under_owner (jit, "owner_d_fn", 555, owner_d);
	CHECK (res_d.entry != 0);

	CHECK (jit->release_owner (owner_c) == 1);
	CHECK (jit->release_owner (owner_a) == 0); /* still erased */
	CHECK (jit->release_owner (owner_b) == 0); /* still erased */

	/* owner_d's dylib must be completely unaffected by the three releases above. */
	auto compiled_d = reinterpret_cast<int64_t (*) (void)> (res_d.entry);
	CHECK (compiled_d () == 555);
	CHECK (jit->release_owner (owner_d) == 1);

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

/*
 * Emit the reloc probe module (see build_reloc_probe () above) through the
 * given code model and return the raw object bytes, rather than the audit
 * audit_code_model () computes. Used by the checks below, which need to
 * byte-patch the object before (re-)auditing it.
 */
static Expected<SmallVector<char, 0>>
emit_reloc_probe_bytes (CodeModel::Model cm)
{
	auto jtmb = mono::host_target_machine_builder ();
	jtmb.setCodeModel (cm);
	auto tm = jtmb.createTargetMachine ();
	if (!tm)
		return tm.takeError ();

	LLVMContext ctx;
	Module m ("selftest.reloc.raw", ctx);
	m.setDataLayout ((*tm)->createDataLayout ());
	build_reloc_probe (m);

	SmallVector<char, 0> buf;
	raw_svector_ostream os (buf);
	legacy::PassManager pm;
	if ((*tm)->addPassesToEmitFile (pm, os, nullptr, CodeGenFileType::ObjectFile))
		return createStringError (inconvertibleErrorCode (),
		                          "target cannot emit an object file");
	pm.run (m);
	return buf;
}

/*
 * audit_relocations () gates entirely on ELFObjectFileBase::getEMachine (): the
 * relocation TYPE NUMBERS it classifies are only meaningful relative to the ISA
 * that defines them (a REX_GOTPCRELX type code is an x86-64 psABI value; on
 * another machine the same numeric code means something else, or nothing), so
 * the switch in x86_64_reloc_truncates_address () must never run against an
 * object claiming a different machine. Prove the gate is checked - not merely
 * documented - by byte-patching ONLY the emitted object's e_machine field, from
 * EM_X86_64 to an unrelated ISA, and showing the exact same relocation bytes
 * that test_reloc_widths already relies on being flagged truncating now audit
 * as entirely clean.
 *
 * e_machine sits at file offset 18: the 16-byte e_ident, then e_type (a u16),
 * then e_machine (a u16), both little-endian on this target. EM_AARCH64 is
 * chosen as the unrelated ISA because it shares ELFCLASS64/ELFDATA2LSB with
 * x86-64 (byte 4 = ELFCLASS64, byte 5 = ELFDATA2LSB), so
 * object::ObjectFile::createObjectFile () - which dispatches purely on those
 * two e_ident bytes, never on e_machine - still parses the patched bytes as an
 * ELF64LE object and every section/relocation stays readable; only
 * getEMachine () changes.
 */
static TestResult
test_audit_relocations_gates_on_e_machine (MonoLLVMJIT *jit)
{
	(void) jit;

	Triple host (sys::getProcessTriple ());
	if (host.getArch () != Triple::x86_64 || !host.isOSBinFormatELF ()) {
		printf ("     host %s is not x86-64 ELF; the e_machine gate is only "
		        "meaningful there\n", host.str ().c_str ());
		return TEST_SKIP;
	}

	auto bytes = emit_reloc_probe_bytes (CodeModel::Small);
	if (!bytes) {
		printf ("     probe emit failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	SmallVector<char, 0> buf = std::move (*bytes);

	auto obj_before = object::ObjectFile::createObjectFile (
		MemoryBufferRef (StringRef (buf.data (), buf.size ()), "unpatched.o"));
	if (!obj_before) {
		printf ("     unpatched parse failed: %s\n",
		        toString (obj_before.takeError ()).c_str ());
		return TEST_FAIL;
	}
	/* Negative control: unpatched, this is exactly the probe test_reloc_widths
	 * already relies on containing truncating relocations under Small. */
	mono::RelocAudit before = mono::audit_relocations (**obj_before);
	CHECK (before.truncating > 0);

	CHECK (buf.size () > 20);
	uint16_t e_machine_before = (uint16_t) (uint8_t) buf[18]
	                          | (uint16_t) ((uint16_t) (uint8_t) buf[19] << 8);
	CHECK (e_machine_before == ELF::EM_X86_64);
	buf[18] = (char) (ELF::EM_AARCH64 & 0xff);
	buf[19] = (char) ((ELF::EM_AARCH64 >> 8) & 0xff);

	auto obj_after = object::ObjectFile::createObjectFile (
		MemoryBufferRef (StringRef (buf.data (), buf.size ()), "patched.o"));
	if (!obj_after) {
		printf ("     patched parse failed: %s\n",
		        toString (obj_after.takeError ()).c_str ());
		return TEST_FAIL;
	}
	const auto *elf_after = dyn_cast<object::ELFObjectFileBase> (&**obj_after);
	CHECK (elf_after != nullptr);
	CHECK (elf_after->getEMachine () == ELF::EM_AARCH64);

	mono::RelocAudit after = mono::audit_relocations (**obj_after);
	printf ("     same bytes, e_machine EM_X86_64(%u)->EM_AARCH64(%u): before "
	        "total=%llu truncating=%llu; after total=%llu truncating=%llu\n",
	        (unsigned) ELF::EM_X86_64, (unsigned) ELF::EM_AARCH64,
	        (unsigned long long) before.total, (unsigned long long) before.truncating,
	        (unsigned long long) after.total, (unsigned long long) after.truncating);

	/* All-zero: an unrecognised machine is "did not look", never an echo of
	 * the (unchanged, still objectively truncating) relocation bytes. */
	CHECK (after.total == 0);
	CHECK (after.truncating == 0);
	CHECK (after.first_offender.empty ());

	return TEST_PASS;
}

/* Little-endian scalar reads directly out of a raw ELF byte buffer. */
static uint32_t
raw_rd_le32 (const char *p)
{
	return (uint32_t) (uint8_t) p[0] | ((uint32_t) (uint8_t) p[1] << 8)
	       | ((uint32_t) (uint8_t) p[2] << 16) | ((uint32_t) (uint8_t) p[3] << 24);
}

static uint64_t
raw_rd_le64 (const char *p)
{
	return (uint64_t) raw_rd_le32 (p) | ((uint64_t) raw_rd_le32 (p + 4) << 32);
}

static uint16_t
raw_rd_le16 (const char *p)
{
	return (uint16_t) (uint8_t) p[0] | ((uint16_t) (uint8_t) p[1] << 8);
}

/*
 * Every raw Elf64_Rela entry in `buf` (24 bytes: r_offset u64, r_info u64,
 * r_addend s64, little-endian), located by walking the ELF64 header/section
 * table directly - NOT through llvm::object, since the checks below need the
 * entry's absolute file offset to byte-patch it, and that offset is exactly
 * what the psABI's own header/section-header layout gives for free. This
 * mirrors the direct-header-offset technique test_audit_relocations_gates_on_
 * e_machine already uses for e_ident, just walking further into the file.
 */
struct RawRela {
	size_t file_offset; /* absolute byte offset of this entry's r_info field */
	uint64_t r_offset;
	uint32_t type;
};

static std::vector<RawRela>
raw_elf64_rela_entries (const SmallVectorImpl<char> &buf)
{
	std::vector<RawRela> out;
	const char *p = buf.data ();
	size_t n = buf.size ();
	if (n < 0x40)
		return out;

	uint64_t e_shoff = raw_rd_le64 (p + 0x28);
	uint16_t e_shentsize = raw_rd_le16 (p + 0x3a);
	uint16_t e_shnum = raw_rd_le16 (p + 0x3c);

	for (uint16_t i = 0; i < e_shnum; i++) {
		size_t shdr_off = (size_t) e_shoff + (size_t) i * e_shentsize;
		if (shdr_off + 0x40 > n)
			break;
		const char *shdr = p + shdr_off;
		uint32_t sh_type = raw_rd_le32 (shdr + 4);
		uint64_t sh_offset = raw_rd_le64 (shdr + 0x18);
		uint64_t sh_size = raw_rd_le64 (shdr + 0x20);
		if (sh_type != 4 /* SHT_RELA - NOT 9, which is SHT_REL */)
			continue;
		for (uint64_t off = 0; off + 24 <= sh_size; off += 24) {
			size_t entry_off = (size_t) sh_offset + (size_t) off;
			if (entry_off + 24 > n)
				break;
			const char *e = p + entry_off;
			uint64_t r_offset = raw_rd_le64 (e);
			uint64_t r_info = raw_rd_le64 (e + 8);
			out.push_back ({ entry_off + 8, r_offset, (uint32_t) r_info });
		}
	}
	return out;
}

/*
 * Independently reasoned from the x86-64 psABI relocation-type ranges - NOT
 * calling, copying the switch of, or in any way deferring to
 * x86_64_reloc_truncates_address () in engine.cpp - so this file can pick a
 * relocation entry it is CERTAIN the audit currently flags truncating, before
 * mutating that entry's raw type byte out from under it. Every member here
 * carries either a narrow absolute/PC-relative address or a 32-bit
 * displacement into a separately-mapped GOT/PLT/TLS slab, matching the ABI's
 * own field-width documentation for these codes.
 */
static bool
psabi_x86_64_type_is_a_truncatable_address_form (uint32_t type)
{
	switch (type) {
	case ELF::R_X86_64_8:
	case ELF::R_X86_64_PC8:
	case ELF::R_X86_64_16:
	case ELF::R_X86_64_PC16:
	case ELF::R_X86_64_32:
	case ELF::R_X86_64_32S:
	case ELF::R_X86_64_PC32:
	case ELF::R_X86_64_PLT32:
	case ELF::R_X86_64_GOTPCREL:
	case ELF::R_X86_64_GOTPCRELX:
	case ELF::R_X86_64_REX_GOTPCRELX:
	case ELF::R_X86_64_GOTPC32:
	case ELF::R_X86_64_TLSGD:
	case ELF::R_X86_64_TLSLD:
	case ELF::R_X86_64_GOTTPOFF:
	case ELF::R_X86_64_GOTPC32_TLSDESC:
		return true;
	default:
		return false;
	}
}

/*
 * x86_64_reloc_truncates_address ()'s default case declines to guess: an
 * out-of-range relocation type (0xEE, which no x86-64 psABI revision has ever
 * defined - the highest assigned code as of this writing is in the 40s) must
 * be counted in `total` (the audit looked at it) but NOT in `truncating` (it
 * was not positively identified as unsafe). Prove this by finding one
 * relocation entry the audit currently flags truncating - independently, via
 * the local psABI-derived helper above, never via engine.cpp - and byte-
 * patching ONLY its low 32 bits of r_info (the type field) to 0xEE, leaving
 * r_offset, the symbol index (the high 32 bits of r_info) and r_addend
 * untouched. The mutated object must audit with the SAME total relocation
 * count and EXACTLY ONE FEWER truncating one.
 */
static TestResult
test_reloc_unclassified_type_not_truncating (MonoLLVMJIT *jit)
{
	(void) jit;

	Triple host (sys::getProcessTriple ());
	if (host.getArch () != Triple::x86_64 || !host.isOSBinFormatELF ()) {
		printf ("     host %s is not x86-64 ELF; the relocation classifier "
		        "covers only x86-64\n", host.str ().c_str ());
		return TEST_SKIP;
	}

	auto bytes = emit_reloc_probe_bytes (CodeModel::Small);
	if (!bytes) {
		printf ("     probe emit failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	SmallVector<char, 0> buf = std::move (*bytes);

	std::vector<RawRela> entries = raw_elf64_rela_entries (buf);
	CHECK (!entries.empty ());

	const RawRela *target = nullptr;
	for (const RawRela &e : entries) {
		if (psabi_x86_64_type_is_a_truncatable_address_form (e.type)) {
			target = &e;
			break;
		}
	}
	/* The Small-model probe is known (test_reloc_widths) to contain truncating
	 * relocations; the psABI-derived set above must find at least one of them. */
	CHECK (target != nullptr);
	uint32_t original_type = target->type;
	size_t patch_off = target->file_offset;

	auto obj_before = object::ObjectFile::createObjectFile (
		MemoryBufferRef (StringRef (buf.data (), buf.size ()), "before.o"));
	if (!obj_before) {
		printf ("     before-parse failed: %s\n", toString (obj_before.takeError ()).c_str ());
		return TEST_FAIL;
	}
	mono::RelocAudit before = mono::audit_relocations (**obj_before);
	CHECK (before.total == entries.size ());
	CHECK (before.truncating > 0);

	/* Patch only the low 4 bytes of r_info (the type field); r_offset, the
	 * symbol index and r_addend are untouched. */
	CHECK (patch_off + 4 <= buf.size ());
	buf[patch_off + 0] = (char) 0xEE;
	buf[patch_off + 1] = 0;
	buf[patch_off + 2] = 0;
	buf[patch_off + 3] = 0;

	auto obj_after = object::ObjectFile::createObjectFile (
		MemoryBufferRef (StringRef (buf.data (), buf.size ()), "after.o"));
	if (!obj_after) {
		printf ("     after-parse failed: %s\n", toString (obj_after.takeError ()).c_str ());
		return TEST_FAIL;
	}
	mono::RelocAudit after = mono::audit_relocations (**obj_after);

	printf ("     patched one relocation's type from %u to 0xEE: before "
	        "total=%llu truncating=%llu; after total=%llu truncating=%llu\n",
	        original_type, (unsigned long long) before.total,
	        (unsigned long long) before.truncating, (unsigned long long) after.total,
	        (unsigned long long) after.truncating);

	/* Still counted (the entry itself was not removed)... */
	CHECK (after.total == before.total);
	/* ...but no longer classified truncating: exactly one fewer. */
	CHECK (after.truncating == before.truncating - 1);

	return TEST_PASS;
}

/*
 * audit_relocations ()'s first_offender de-dups (only the FIRST truncating hit
 * is recorded) and reports "<section>/<type>". Cross-check both properties
 * against an INDEPENDENT scan of the same object, done here with the generic
 * object::SectionRef/RelocationRef iteration (the same public iteration order
 * audit_relocations () itself uses - obj.sections () then sec.relocations ()
 * in order - but with the truncating/non-truncating call made by this file's
 * own psabi_x86_64_type_is_a_truncatable_address_form (), not by calling into
 * engine.cpp) to find which section/type combination comes first, and confirm
 * it is exactly what first_offender reports.
 */
static TestResult
test_reloc_first_offender_matches_first_scan (MonoLLVMJIT *jit)
{
	(void) jit;

	Triple host (sys::getProcessTriple ());
	if (host.getArch () != Triple::x86_64 || !host.isOSBinFormatELF ()) {
		printf ("     host %s is not x86-64 ELF; the relocation classifier "
		        "covers only x86-64\n", host.str ().c_str ());
		return TEST_SKIP;
	}

	auto bytes = emit_reloc_probe_bytes (CodeModel::Small);
	if (!bytes) {
		printf ("     probe emit failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	SmallVector<char, 0> buf = std::move (*bytes);

	auto obj = object::ObjectFile::createObjectFile (
		MemoryBufferRef (StringRef (buf.data (), buf.size ()), "probe.o"));
	if (!obj) {
		printf ("     parse failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}

	mono::RelocAudit audit = mono::audit_relocations (**obj);
	CHECK (audit.truncating > 0);
	CHECK (!audit.first_offender.empty ());
	/* Shape: "<section>/<type>" - a real section name, never the "?" fallback,
	 * for a normal, well-formed object with a readable section-name table. */
	CHECK (audit.first_offender.find ('/') != std::string::npos);
	CHECK (audit.first_offender[0] != '?');

	/* Independent first-hit scan, same iteration order, own classifier. */
	std::string expect_section;
	uint32_t expect_type = 0;
	bool found = false;
	for (const object::SectionRef &sec : (*obj)->sections ()) {
		for (const object::RelocationRef &rel : sec.relocations ()) {
			if (!psabi_x86_64_type_is_a_truncatable_address_form (
			        (uint32_t) rel.getType ()))
				continue;
			Expected<StringRef> name = sec.getName ();
			CHECK ((bool) name);
			expect_section = name->str ();
			expect_type = (uint32_t) rel.getType ();
			found = true;
			break;
		}
		if (found)
			break;
	}
	CHECK (found);

	std::string expect = expect_section + "/";
	/* Recover the LLVM type-name string the same way engine.cpp does - via
	 * RelocationRef::getTypeName () - so the comparison is against the exact
	 * string audit_relocations () builds, not a re-derived guess at spelling. */
	for (const object::SectionRef &sec : (*obj)->sections ()) {
		Expected<StringRef> name = sec.getName ();
		if (!name || *name != expect_section)
			continue;
		for (const object::RelocationRef &rel : sec.relocations ()) {
			if ((uint32_t) rel.getType () != expect_type)
				continue;
			SmallString<32> tn;
			rel.getTypeName (tn);
			expect += tn.c_str ();
			goto built_expect;
		}
	}
built_expect:
	printf ("     first_offender=\"%s\" expect=\"%s\"\n",
	        audit.first_offender.c_str (), expect.c_str ());
	CHECK (audit.first_offender == expect);

	return TEST_PASS;
}

/*
 * Closes JL1 review finding #1: mono::audit_relocations_graph() (engine.hpp) -
 * the test-only accessor to accumulate_reloc_audit_from_graph()'s
 * classification (engine.cpp) - is exercised directly against hand-built
 * jitlink::LinkGraphs, one per shape, rather than only indirectly through a
 * real compile (test_reloc_widths's third part, which proves the accumulator
 * is wired up but cannot target a specific cross-boundary shape - only
 * observe whatever a real method happens to produce).
 *
 * Content bytes are irrelevant here - the classifier reads only edge Kind and
 * target metadata (defined/external/absolute), never block content - so every
 * block below is a zero-filled dummy buffer.
 *
 * The five shapes are exactly the design's re-spec of the graph-boundary
 * predicate (host_target_machine_builder()'s doc comment in engine.cpp):
 *   1. same-graph, cross-SECTION, narrow kind: NOT truncating. This is the
 *      shape the predicate fix changes the answer for - the dropped clause
 *      (target in a different section than the referring block) used to flag
 *      this.
 *   2. same-graph, SAME-section, narrow kind: not truncating either way - a
 *      regression guard that the pre-existing intra-section carve-out
 *      survives the fix.
 *   3. external target, narrow kind: truncating - unchanged by the fix, and
 *      the shape a real bug (a pass emitting a raw narrow edge straight to an
 *      external Symbol, bypassing PLTTableManager) would look like.
 *   4. external target, Pointer64: never truncating - x86_64_edge_kind_truncates()
 *      classifies Pointer64 as full-width regardless of target, so this is a
 *      negative control on the KIND check, independent of the boundary check.
 *   5. absolute target, narrow kind: truncating - unchanged by the fix; the
 *      shape doc 26 P5's Medium+PIC _GLOBAL_OFFSET_TABLE_-resolves-to-0
 *      failure would have looked like structurally.
 */
static TestResult
test_graph_audit_cross_boundary (MonoLLVMJIT *jit)
{
	(void) jit;

	using namespace llvm::jitlink;
	Triple tt ("x86_64-unknown-linux-gnu");
	static const char dummy[16] = {};

	auto make_block = [&] (LinkGraph &g, Section &sec, orc::ExecutorAddr addr) -> Block & {
		return g.createContentBlock (sec, ArrayRef<char> (dummy, sizeof (dummy)),
		                             addr, 1, 0);
	};

	/* Case 1: same-graph, cross-section, narrow (Delta32) -> not truncating. */
	{
		LinkGraph g ("case1", tt, 8, llvm::endianness::little, x86_64::getEdgeKindName);
		auto &text = g.createSection (".text", orc::MemProt::Read | orc::MemProt::Exec);
		auto &data = g.createSection (".data", orc::MemProt::Read | orc::MemProt::Write);
		Block &src = make_block (g, text, orc::ExecutorAddr (0x1000));
		Block &dst = make_block (g, data, orc::ExecutorAddr (0x2000));
		Symbol &target = g.addDefinedSymbol (dst, 0, "data_target", 8,
		                                     Linkage::Strong, Scope::Default, false, true);
		src.addEdge (x86_64::Delta32, 0, target, 0);

		mono::RelocAudit audit = mono::audit_relocations_graph (g);
		CHECK (audit.total == 1);
		CHECK (audit.truncating == 0);
	}

	/* Case 2: same-graph, same-section, narrow (Delta32) -> not truncating. */
	{
		LinkGraph g ("case2", tt, 8, llvm::endianness::little, x86_64::getEdgeKindName);
		auto &text = g.createSection (".text", orc::MemProt::Read | orc::MemProt::Exec);
		Block &src = make_block (g, text, orc::ExecutorAddr (0x1000));
		Block &dst = make_block (g, text, orc::ExecutorAddr (0x1100));
		Symbol &target = g.addDefinedSymbol (dst, 0, "text_target", 8,
		                                     Linkage::Strong, Scope::Default, true, true);
		src.addEdge (x86_64::Delta32, 0, target, 0);

		mono::RelocAudit audit = mono::audit_relocations_graph (g);
		CHECK (audit.total == 1);
		CHECK (audit.truncating == 0);
	}

	/* Case 3: external target, narrow (BranchPCRel32) -> truncating. */
	{
		LinkGraph g ("case3", tt, 8, llvm::endianness::little, x86_64::getEdgeKindName);
		auto &text = g.createSection (".text", orc::MemProt::Read | orc::MemProt::Exec);
		Block &src = make_block (g, text, orc::ExecutorAddr (0x1000));
		Symbol &target = g.addExternalSymbol ("ext_narrow", 0, false);
		src.addEdge (x86_64::BranchPCRel32, 0, target, 0);

		mono::RelocAudit audit = mono::audit_relocations_graph (g);
		CHECK (audit.total == 1);
		CHECK (audit.truncating == 1);
		CHECK (!audit.first_offender.empty ());
	}

	/* Case 4: external target, Pointer64 -> never truncating. */
	{
		LinkGraph g ("case4", tt, 8, llvm::endianness::little, x86_64::getEdgeKindName);
		auto &text = g.createSection (".text", orc::MemProt::Read | orc::MemProt::Exec);
		Block &src = make_block (g, text, orc::ExecutorAddr (0x1000));
		Symbol &target = g.addExternalSymbol ("ext_wide", 0, false);
		src.addEdge (x86_64::Pointer64, 0, target, 0);

		mono::RelocAudit audit = mono::audit_relocations_graph (g);
		CHECK (audit.total == 1);
		CHECK (audit.truncating == 0);
	}

	/* Case 5: absolute target, narrow (BranchPCRel32) -> truncating. */
	{
		LinkGraph g ("case5", tt, 8, llvm::endianness::little, x86_64::getEdgeKindName);
		auto &text = g.createSection (".text", orc::MemProt::Read | orc::MemProt::Exec);
		Block &src = make_block (g, text, orc::ExecutorAddr (0x1000));
		Symbol &target = g.addAbsoluteSymbol ("abs_narrow", orc::ExecutorAddr (0x600000000000),
		                                      0, Linkage::Strong, Scope::Default, true);
		src.addEdge (x86_64::BranchPCRel32, 0, target, 0);

		mono::RelocAudit audit = mono::audit_relocations_graph (g);
		CHECK (audit.total == 1);
		CHECK (audit.truncating == 1);
	}

	/*
	 * Non-x86-64 graph: left entirely unclassified (all-zero), matching
	 * audit_relocations ()'s contract - an all-zero audit is "did not look",
	 * not "looked and found nothing".
	 */
	{
		Triple arm_tt ("aarch64-unknown-linux-gnu");
		LinkGraph g ("case_non_x86_64", arm_tt, 8, llvm::endianness::little,
		            x86_64::getEdgeKindName);
		auto &text = g.createSection (".text", orc::MemProt::Read | orc::MemProt::Exec);
		Block &src = make_block (g, text, orc::ExecutorAddr (0x1000));
		Symbol &target = g.addExternalSymbol ("ext_on_other_arch", 0, false);
		src.addEdge (x86_64::BranchPCRel32, 0, target, 0);

		mono::RelocAudit audit = mono::audit_relocations_graph (g);
		CHECK (audit.total == 0);
		CHECK (audit.truncating == 0);
	}

	return TEST_PASS;
}

/* ------------------------------------------------ EH module builders */

/*
 * Name of the external, possibly-unwinding callee the EH probe modules below
 * invoke through an `invoke`/`landingpad` pair. Declared external and never
 * defined in these modules: the builders below are only run through
 * gather_eh_sidechannel() (static IR analysis, no JIT execution), so nothing
 * ever needs to resolve this symbol.
 */
static const char EH_MAY_THROW_NAME[] = "mono$selftest$eh$may_throw";

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

	/*
	 * Two clause smuggling globals: the 2-word {i32 clause_index, i32 kind} form
	 * the translator emits (emit_handler_start). IL clause indices 7 and 3, both
	 * catch (kind == 0). Mirrors mono's real v2 emission so the struct gather path
	 * is exercised.
	 */
	StructType *ti_ty = StructType::get (i32, i32);
	auto *ti0 = new GlobalVariable (m, ti_ty, false, GlobalValue::ExternalLinkage,
	                                ConstantStruct::get (ti_ty, ConstantInt::get (i32, 7),
	                                                     ConstantInt::get (i32, 0)),
	                                "type_info_0");
	auto *ti1 = new GlobalVariable (m, ti_ty, false, GlobalValue::ExternalLinkage,
	                                ConstantStruct::get (ti_ty, ConstantInt::get (i32, 3),
	                                                     ConstantInt::get (i32, 0)),
	                                "type_info_1");

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
 * back-to-back same-landing-pad ranges into a single call-site at the
 * object-emission level, muddying the two-distinct-ranges shape this test
 * wants to exercise.
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

	/* 2-word {clause_index, kind} smuggling global: IL clause 5, catch (kind 0). */
	StructType *ti_ty = StructType::get (i32, i32);
	auto *ti0 = new GlobalVariable (m, ti_ty, false, GlobalValue::ExternalLinkage,
	                                ConstantStruct::get (ti_ty, ConstantInt::get (i32, 5),
	                                                     ConstantInt::get (i32, 0)),
	                                "type_info_0");

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
 * Build, in `m`, an EH function whose ONLY landing pad carries a FILTER clause
 * (an exception-specification) rather than a catch - a shape outside the catch-
 * only milestone. LLVM assigns a filter a NEGATIVE TypeId, which the C2 gather
 * recognises (has_filter / declined) and which C3 must therefore emit NOTHING
 * for. Returns `i32 eh_filter(void)`.
 *
 * The gather marking this function `declined` is the point: a declined function
 * produces no .mono_lsda record, so the section must be absent/zero-size. This
 * pins the "declined => no bytes" fail-safe (CAP-EH-0) that no other test covers.
 */
static Function *
build_eh_filter_module (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	auto *ti0 = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                ConstantInt::get (i32, 9), "type_info_0");

	FunctionType *fty = FunctionType::get (i32, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "eh_filter", &m);

	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   "mono_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *cont = BasicBlock::Create (ctx, "cont", fn);
	BasicBlock *lpad = BasicBlock::Create (ctx, "lpad", fn);

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont, lpad, {});

	IRBuilder<> cb (cont);
	cb.CreateRet (ConstantInt::get (i32, 0));

	StructType *lp_ty = StructType::get (ptr, i32);
	IRBuilder<> lb (lpad);
	LandingPadInst *lp = lb.CreateLandingPad (lp_ty, 1);
	/*
	 * A filter clause is an ARRAY constant (not a bare type_info): [1 x ptr]
	 * naming the one permitted type. LLVM lowers it to a negative TypeId, which
	 * the gather flags as has_filter/declined.
	 */
	ArrayType *filter_ty = ArrayType::get (ptr, 1);
	lp->addClause (ConstantArray::get (filter_ty, { ti0 }));
	lb.CreateRet (lb.CreateExtractValue (lp, 1));

	return fn;
}

/*
 * Build, in `m`, an EH function whose single catch clause references a
 * type_info_0 that is a bare EXTERNAL DECLARATION - `new GlobalVariable (m,
 * i32, false, ExternalLinkage, nullptr, "type_info_0")` - rather than a
 * initialized definition. Every other EH probe in this file gives type_info_0 an
 * initializer so the gather can smuggle the IL clause index (and kind) through it
 * (var->hasInitializer () true, then a ConstantInt / 2-word struct read on the
 * initializer); here hasInitializer () is false, so there is no clause index
 * to recover at all. This is a DIFFERENT decline cause from
 * build_eh_filter_module's negative TypeId: the TypeId here is a normal
 * POSITIVE catch (TypeId 1), so has_filter must stay false, while
 * clause_resolved must still come back false and the function still decline
 * (CAP-EH-0: never guess a clause index). Returns `i32 eh_external_typeinfo(void)`.
 */
static Function *
build_eh_external_typeinfo_module (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	/* type_info_0: external declaration, NO initializer. */
	auto *type_info = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                      /*Initializer*/ nullptr, "type_info_0");

	FunctionType *fty = FunctionType::get (i32, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage,
	                                 "eh_external_typeinfo", &m);

	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   "mono_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *cont = BasicBlock::Create (ctx, "cont", fn);
	BasicBlock *lpad = BasicBlock::Create (ctx, "lpad", fn);

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont, lpad, {});

	IRBuilder<> cb (cont);
	cb.CreateRet (ConstantInt::get (i32, 0));

	StructType *lp_ty = StructType::get (ptr, i32);
	IRBuilder<> lb (lpad);
	LandingPadInst *lp = lb.CreateLandingPad (lp_ty, 1);
	lp->addClause (type_info); /* an ordinary catch clause -> a POSITIVE TypeId */
	lb.CreateRet (lb.CreateExtractValue (lp, 1));

	return fn;
}

/*
 * Build, in `m`, an EH function whose only landing pad is a pure CLEANUP: the
 * LandingPadInst carries lp->setCleanup (true) and NO catch/filter clauses at
 * all (IR-legal: the verifier only requires "at least one clause or [...] a
 * cleanup"). This is the TypeId-0 code path in MonoEHGatherPass -
 * distinct from build_eh_filter_module's NEGATIVE TypeId (a filter) - and,
 * like the filter, is a shape outside the catch-only milestone that C2 must
 * decline rather than silently treat as "no clauses to worry about". Returns
 * `i32 eh_cleanup(void)`.
 */
static Function *
build_eh_cleanup_module (Module &m)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	FunctionType *fty = FunctionType::get (i32, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "eh_cleanup", &m);

	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   "mono_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

	BasicBlock *entry = BasicBlock::Create (ctx, "entry", fn);
	BasicBlock *cont = BasicBlock::Create (ctx, "cont", fn);
	BasicBlock *lpad = BasicBlock::Create (ctx, "lpad", fn);

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont, lpad, {});

	IRBuilder<> cb (cont);
	cb.CreateRet (ConstantInt::get (i32, 0));

	StructType *lp_ty = StructType::get (ptr, i32);
	IRBuilder<> lb (lpad);
	LandingPadInst *lp = lb.CreateLandingPad (lp_ty, 1);
	lp->setCleanup (true); /* cleanup only - no addClause () at all */
	lb.CreateRet (lb.CreateExtractValue (lp, 1));

	return fn;
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

	/* Every gathered clause resolved and carries the three .text symbols C3 needs;
	 * both are catch, so the smuggled kind is NONE (0). */
	for (const mono::MonoEHClause &cl : fn.clauses) {
		CHECK (cl.clause_resolved);
		CHECK (cl.try_begin != nullptr);
		CHECK (cl.try_end != nullptr);
		CHECK (cl.handler != nullptr);
		CHECK (cl.kind == 0);
	}

	/* The smuggled clause indices are {3, 7} (order is landing-pad order; assert
	 * as a set so a codegen reorder cannot make this flaky). */
	std::vector<int> got = { fn.clauses[0].clause_index, fn.clauses[1].clause_index };
	std::sort (got.begin (), got.end ());
	CHECK (got[0] == 3);
	CHECK (got[1] == 7);

	return TEST_PASS;
}

/*
 * C2 regression for the review's blocking bug: a landing pad carries one
 * (begin,end) pair PER INVOKE that unwinds to it, so a single try protecting TWO
 * calls that share one catch landing pad must gather TWO clause entries (one per
 * invoke range) - same handler, same clause_index, DIFFERENT ranges. A gather
 * that kept only the first range would drop the second call's PC range and let a
 * throw from it escape the handler at C6.
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

	return TEST_PASS;
}

/*
 * v2 kind smuggling: the gather must read the SECOND word of the type_info_N
 * {i32 clause_index, i32 kind} struct, not assume 0. Build a single catch landing
 * pad whose smuggling global carries clause_index 6 and a NON-ZERO kind (2, the
 * FINALLY sentinel - used here only to prove the channel carries a nonzero value
 * end to end; F1's real translator only ever emits catch/kind 0). The gather must
 * resolve clause_index 6 AND kind 2, and not decline (a positive catch TypeId).
 */
static TestResult
test_eh_gather_kind_smuggling (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m ("selftest.eh.gather.kind", c1);
	Type *i32 = Type::getInt32Ty (c1);
	PointerType *ptr = PointerType::getUnqual (c1);

	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (c1), false);
	Function *callee = Function::Create (callee_ty, Function::ExternalLinkage,
	                                     EH_MAY_THROW_NAME, &m);

	StructType *ti_ty = StructType::get (i32, i32);
	auto *ti0 = new GlobalVariable (m, ti_ty, false, GlobalValue::ExternalLinkage,
	                                ConstantStruct::get (ti_ty, ConstantInt::get (i32, 6),
	                                                     ConstantInt::get (i32, 2)),
	                                "type_info_0");

	FunctionType *fty = FunctionType::get (i32, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "eh_kind", &m);

	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   "mono_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (c1, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

	BasicBlock *entry = BasicBlock::Create (c1, "entry", fn);
	BasicBlock *cont = BasicBlock::Create (c1, "cont", fn);
	BasicBlock *lpad = BasicBlock::Create (c1, "lpad", fn);

	IRBuilder<> b (entry);
	b.CreateInvoke (callee, cont, lpad, {});
	IRBuilder<> cb (cont);
	cb.CreateRet (ConstantInt::get (i32, 0));

	StructType *lp_ty = StructType::get (ptr, i32);
	IRBuilder<> lb (lpad);
	LandingPadInst *lp = lb.CreateLandingPad (lp_ty, 1);
	lp->addClause (ti0); /* catch -> type_info_0 {clause 6, kind 2} */
	lb.CreateRet (lb.CreateExtractValue (lp, 1));

	auto sc = mono::gather_eh_sidechannel (m);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (sc->functions.size () == 1);
	const mono::MonoEHFunctionClauses &fnc = sc->functions[0];
	CHECK (!fnc.declined);
	CHECK (!fnc.has_filter);
	CHECK (fnc.clauses.size () == 1);
	CHECK (fnc.clauses[0].clause_resolved);
	CHECK (fnc.clauses[0].clause_index == 6);
	CHECK (fnc.clauses[0].kind == 2); /* the second struct word, actually read */

	printf ("     kind-smuggling: clause_index=%d kind=%d (2-word struct read)\n",
	        fnc.clauses[0].clause_index, fnc.clauses[0].kind);
	return TEST_PASS;
}

/* --------------------------------------------- .mono_lsda emit (C3) */

/* Little-endian scalar reads out of a captured section's bytes. */
static uint32_t
rd_le32 (const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
	       | ((uint32_t) p[3] << 24);
}

static uint16_t
rd_le16 (const uint8_t *p)
{
	return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

/* One decoded .mono_lsda v2 entry: five code-relative u32 fields (incl. kind). */
struct LsdaEntry {
	uint32_t try_start_off, try_len, handler_off, clause_index, kind;
};

/*
 * Header-checked decode of a .mono_lsda section: magic 'MLSD', version 2, and a
 * body whose length is exactly 8 + count*20 (the v2 self-describing kind column).
 * Mirrors what the C4 load-side parser does; false on any mismatch/truncation.
 */
static bool
parse_mono_lsda (const std::vector<uint8_t> &b, std::vector<LsdaEntry> &out)
{
	if (b.size () < 8)
		return false;
	if (rd_le32 (b.data ()) != 0x4d4c5344u) /* 'MLSD' */
		return false;
	if (rd_le16 (b.data () + 4) != 2)
		return false;
	uint16_t count = rd_le16 (b.data () + 6);
	if (b.size () != (size_t) 8 + (size_t) count * 20)
		return false;
	for (uint16_t i = 0; i < count; i++) {
		const uint8_t *e = b.data () + 8 + (size_t) i * 20;
		out.push_back ({ rd_le32 (e), rd_le32 (e + 4), rd_le32 (e + 8),
		                 rd_le32 (e + 12), rd_le32 (e + 16) });
	}
	return true;
}

/* st_size of a named function symbol in an emitted object, 0 if absent. */
static Expected<uint64_t>
object_func_size (MemoryBuffer &buf, StringRef want)
{
	auto obj = object::ObjectFile::createObjectFile (buf.getMemBufferRef ());
	if (!obj)
		return obj.takeError ();
	for (const object::SymbolRef &sym : (*obj)->symbols ()) {
		Expected<StringRef> name = sym.getName ();
		if (!name) {
			consumeError (name.takeError ());
			continue;
		}
		if (*name != want)
			continue;
		return object::ELFSymbolRef (sym).getSize ();
	}
	return (uint64_t) 0;
}

/* Number of relocations that apply TO the named section (i.e. from .rela.<name>). */
static Expected<size_t>
object_section_reloc_count (MemoryBuffer &buf, StringRef want)
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
		size_t n = 0;
		for (const object::RelocationRef &r : sec.relocations ()) {
			(void) r;
			n++;
		}
		return n;
	}
	return (size_t) 0;
}

/* Hex dump of a captured section, for the report. */
static std::string
hex_dump (const std::vector<uint8_t> &b)
{
	std::string s;
	char buf[4];
	for (size_t i = 0; i < b.size (); i++) {
		snprintf (buf, sizeof buf, "%02x", b[i]);
		s += buf;
		if ((i & 15) == 15)
			s += '\n';
		else if ((i & 3) == 3)
			s += ' ';
	}
	return s;
}

/*
 * C3 acceptance check: MonoLSDAStreamer, installed in MonoIRCompiler's pipeline
 * in place of the stock object streamer, writes a .mono_lsda section built from
 * the C2 gather side channel. Drive the two-sibling-catch module through the FULL
 * MonoIRCompiler pipeline (compile_object_with_mono_compiler - the exact object
 * emission the runtime path uses) and assert the emitted bytes:
 *
 *   - header: magic 'MLSD', version 2, count 2 (two invoke ranges -> two entries);
 *   - clause indices exactly {3, 7} (the smuggled IL indices), as a set;
 *   - every offset self-consistent: try range within the entry function and
 *     handler_off inside it, try_len > 0 (offsets are code-relative, so their
 *     exact values are host-codegen-dependent and NOT hardcoded);
 *   - ZERO relocations on .mono_lsda (both labels fold to .text constants - the
 *     target-neutral, load-reloc-free property this format is built for);
 *   - and a cross-check that the count and clause-index set MATCH what the C2
 *     gather pass reports for the same module: the streamer emits what the pass
 *     gathered, nothing else.
 */
static TestResult
test_eh_mono_lsda_two_catch (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m1 ("selftest.eh.lsda.twocatch", c1);
	build_eh_two_catch_module (m1);

	auto obj = mono::compile_object_with_mono_compiler (m1);
	if (!obj) {
		printf ("     emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}

	auto bytes = object_section_bytes (**obj, ".mono_lsda");
	if (!bytes) {
		printf ("     section read failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (!bytes->empty ());

	std::vector<LsdaEntry> es;
	CHECK (parse_mono_lsda (*bytes, es));
	CHECK (es.size () == 2);

	/* Clause indices are {3, 7} (landing-pad order is not load-bearing). */
	std::vector<uint32_t> ci = { es[0].clause_index, es[1].clause_index };
	std::sort (ci.begin (), ci.end ());
	CHECK (ci[0] == 3);
	CHECK (ci[1] == 7);

	/* v2 kind column: both entries are catch, so kind == 0 (round-tripped from
	 * the struct globals through the gather and the streamer). */
	CHECK (es[0].kind == 0);
	CHECK (es[1].kind == 0);

	/* Every offset lands inside the entry function's own machine code. */
	auto fsz = object_func_size (**obj, "eh_two_catch");
	if (!fsz) {
		printf ("     func size read failed: %s\n", toString (fsz.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (*fsz > 0);
	for (const LsdaEntry &e : es) {
		CHECK (e.try_len > 0);
		CHECK (e.try_start_off < *fsz);
		CHECK ((uint64_t) e.try_start_off + e.try_len <= *fsz);
		CHECK (e.handler_off < *fsz);
	}

	/* ZERO relocations - the acceptance signal (plan 12 1.2 / 2). */
	auto rc = object_section_reloc_count (**obj, ".mono_lsda");
	if (!rc) {
		printf ("     reloc count failed: %s\n", toString (rc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (*rc == 0);

	/* Cross-check count + clause set against the C2 gather for the same module. */
	LLVMContext c2;
	Module m2 ("selftest.eh.lsda.twocatch.xcheck", c2);
	build_eh_two_catch_module (m2);
	auto sc = mono::gather_eh_sidechannel (m2);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (sc->functions.size () == 1);
	CHECK (sc->functions[0].clauses.size () == es.size ());
	std::vector<int> gci;
	for (const mono::MonoEHClause &cl : sc->functions[0].clauses)
		gci.push_back (cl.clause_index);
	std::sort (gci.begin (), gci.end ());
	CHECK (gci.size () == 2 && gci[0] == 3 && gci[1] == 7);

	printf ("     two-catch: %zu-byte .mono_lsda, %zu entries, clauses {%u,%u}, "
	        "func=%llu bytes, 0 relocs\n     bytes: %s\n",
	        bytes->size (), es.size (), ci[0], ci[1],
	        (unsigned long long) *fsz, hex_dump (*bytes).c_str ());
	return TEST_PASS;
}

/*
 * C3 regression, the C2-fix invariant now visible in the emitted bytes: a single
 * try protecting TWO calls that share one catch landing pad emits TWO .mono_lsda
 * entries - SAME clause_index (5) and SAME handler_off, but DIFFERENT
 * try_start_off (disjoint invoke ranges). A one-range gather (the silent
 * mis-catch bug) would emit count 1 here; the byte assertion catches it.
 */
static TestResult
test_eh_mono_lsda_multi_call (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m1 ("selftest.eh.lsda.multicall", c1);
	build_eh_multi_call_module (m1);

	auto obj = mono::compile_object_with_mono_compiler (m1);
	if (!obj) {
		printf ("     emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}

	auto bytes = object_section_bytes (**obj, ".mono_lsda");
	if (!bytes) {
		printf ("     section read failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (!bytes->empty ());

	std::vector<LsdaEntry> es;
	CHECK (parse_mono_lsda (*bytes, es));

	/* TWO entries for ONE landing pad's two invoke ranges. */
	CHECK (es.size () == 2);

	/* Same clause_index (5) and same handler_off... */
	CHECK (es[0].clause_index == 5);
	CHECK (es[1].clause_index == 5);
	CHECK (es[0].handler_off == es[1].handler_off);

	/* v2 kind column: catch clause, kind == 0 on both invoke-range entries. */
	CHECK (es[0].kind == 0);
	CHECK (es[1].kind == 0);

	/* ...but DIFFERENT try_start_off (disjoint ranges - the whole point). */
	CHECK (es[0].try_start_off != es[1].try_start_off);

	/* Both ranges land inside the entry function. */
	auto fsz = object_func_size (**obj, "eh_multi_call");
	if (!fsz) {
		printf ("     func size read failed: %s\n", toString (fsz.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (*fsz > 0);
	for (const LsdaEntry &e : es) {
		CHECK (e.try_len > 0);
		CHECK (e.try_start_off < *fsz);
		CHECK ((uint64_t) e.try_start_off + e.try_len <= *fsz);
		CHECK (e.handler_off < *fsz);
	}

	/* ZERO relocations. */
	auto rc = object_section_reloc_count (**obj, ".mono_lsda");
	if (!rc) {
		printf ("     reloc count failed: %s\n", toString (rc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (*rc == 0);

	/* Cross-check against the C2 gather. */
	LLVMContext c2;
	Module m2 ("selftest.eh.lsda.multicall.xcheck", c2);
	build_eh_multi_call_module (m2);
	auto sc = mono::gather_eh_sidechannel (m2);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (sc->functions.size () == 1);
	CHECK (sc->functions[0].clauses.size () == es.size ());

	printf ("     multi-call: %zu-byte .mono_lsda, 2 entries clause 5, handler_off "
	        "0x%x shared, try_start_off 0x%x vs 0x%x, 0 relocs\n     bytes: %s\n",
	        bytes->size (), es[0].handler_off, es[0].try_start_off, es[1].try_start_off,
	        hex_dump (*bytes).c_str ());
	return TEST_PASS;
}

/*
 * C3 fail-safe: a module whose ONLY EH function is DECLINED (here a filter
 * landing pad - a negative TypeId the C2 gather marks has_filter/declined)
 * produces NO .mono_lsda section at all. This is the "declined => no bytes"
 * contract (CAP-EH-0): the streamer must emit nothing for a declined function so
 * the load side (C4) sees an absent section and declines the method to the
 * classic JIT, never a partial/misattributed clause table.
 *
 * First confirm the gather actually declines this shape (so the test is
 * exercising the declined path, not an incidentally clause-less one), then
 * confirm the emitted object carries a zero-size / absent .mono_lsda.
 */
static TestResult
test_eh_mono_lsda_declined_emits_nothing (MonoLLVMJIT *jit)
{
	(void) jit;

	/* The gather must mark this function declined (via the filter TypeId). */
	LLVMContext c1;
	Module m1 ("selftest.eh.lsda.declined.gather", c1);
	build_eh_filter_module (m1);
	auto sc = mono::gather_eh_sidechannel (m1);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (sc->functions.size () == 1);
	CHECK (sc->functions[0].declined);
	CHECK (sc->functions[0].has_filter);

	/* The emitted object must therefore carry NO .mono_lsda bytes. */
	LLVMContext c2;
	Module m2 ("selftest.eh.lsda.declined", c2);
	build_eh_filter_module (m2);
	auto obj = mono::compile_object_with_mono_compiler (m2);
	if (!obj) {
		printf ("     emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}
	auto bytes = object_section_bytes (**obj, ".mono_lsda");
	if (!bytes) {
		printf ("     section read failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (bytes->empty ()); /* absent section reads back as zero bytes */

	printf ("     declined (filter) EH function: gather declined=1, .mono_lsda "
	        "absent (%zu bytes) - fail-safe holds\n", bytes->size ());
	return TEST_PASS;
}

/*
 * C2 fail-safe, distinct decline cause: a type_info_0 that is an external
 * DECLARATION (no initializer) rather than a ConstantInt-initialized global.
 * The clause is an ordinary POSITIVE-TypeId catch - unlike build_eh_filter_
 * module's negative-TypeId filter - so has_filter must stay false, while the
 * missing initializer means the clause index cannot be recovered at all:
 * clause_resolved must be false, clause_index must be left at its declared
 * default (-1, see MonoEHClause in engine.hpp), and the function must decline
 * (CAP-EH-0) exactly like the filter case, so no .mono_lsda record is ever
 * built from a guessed clause index.
 */
static TestResult
test_eh_gather_external_typeinfo_declines (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m1 ("selftest.eh.externaltypeinfo.gather", c1);
	build_eh_external_typeinfo_module (m1);

	auto sc = mono::gather_eh_sidechannel (m1);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (sc->functions.size () == 1);
	const mono::MonoEHFunctionClauses &fn = sc->functions[0];
	CHECK (fn.function == "eh_external_typeinfo");
	CHECK (fn.declined);    /* an unresolvable clause_index must decline */
	CHECK (!fn.has_filter); /* a positive (catch) TypeId, NOT a filter - the
	                          * decline here has a different cause than
	                          * build_eh_filter_module's */
	CHECK (fn.clauses.size () == 1);
	CHECK (!fn.clauses[0].clause_resolved);
	CHECK (fn.clauses[0].clause_index == -1); /* left at its declared default */

	/* The emitted object must therefore carry NO .mono_lsda bytes - the same
	 * fail-safe shape as the filter/declined test, for a different cause. */
	LLVMContext c2;
	Module m2 ("selftest.eh.externaltypeinfo.emit", c2);
	build_eh_external_typeinfo_module (m2);
	auto obj = mono::compile_object_with_mono_compiler (m2);
	if (!obj) {
		printf ("     emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}
	auto bytes = object_section_bytes (**obj, ".mono_lsda");
	if (!bytes) {
		printf ("     section read failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (bytes->empty ());

	printf ("     external (uninitialized) type_info_0: declined=1 has_filter=0 "
	        "clause_resolved=0 clause_index=%d, .mono_lsda absent (%zu bytes)\n",
	        fn.clauses[0].clause_index, bytes->size ());
	return TEST_PASS;
}

/*
 * C2 fail-safe, cleanup TypeId path: a landing pad with lp->setCleanup (true)
 * and NO clauses at all - LLVM's "implicit cleanup" encoding (see
 * MachineFunction::addLandingPad in LLVM's CodeGen: a cleanup flag with a
 * non-empty clause list gets an explicit TypeId 0 pushed onto LandingPadInfo::
 * TypeIds ahead of the real clauses; a cleanup flag with NO clauses pushes
 * nothing at all, leaving TypeIds completely empty). Either way this is out of
 * the catch-only milestone and must not be silently treated as "no clauses to
 * gather, therefore nothing to decline": build_eh_filter_module's negative-
 * TypeId filter DOES decline via the type_id<0 branch, so this check pins that
 * a cleanup landing pad takes the SAME fail-safe path via a route that never
 * enters that branch at all (its TypeIds list has zero entries, so the
 * per-TypeId loop body never executes for this landing pad).
 */
static TestResult
test_eh_gather_cleanup_declines (MonoLLVMJIT *jit)
{
	(void) jit;

	LLVMContext c1;
	Module m1 ("selftest.eh.cleanup.gather", c1);
	build_eh_cleanup_module (m1);

	auto sc = mono::gather_eh_sidechannel (m1);
	if (!sc) {
		printf ("     gather failed: %s\n", toString (sc.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (sc->functions.size () == 1);
	const mono::MonoEHFunctionClauses &fn = sc->functions[0];
	CHECK (fn.function == "eh_cleanup");
	CHECK (!fn.has_filter); /* cleanup, not a filter - has_filter must stay false */

	/*
	 * A pure-cleanup landing pad (isCleanup () with zero clauses) leaves
	 * LandingPadInfo::TypeIds completely empty, so MonoEHGatherPass's per-TypeId
	 * loop never runs for it. The gather instead reads the cleanup bit straight
	 * off the IR LandingPadInst, so a cleanup is declined exactly like a filter -
	 * the `declined` side channel is correct on its own, not merely rescued by
	 * MonoLSDAStreamer's downstream empty-clauses guard.
	 */
	CHECK (fn.declined);

	/* Whichever way the gather side channel came back, the emitted object
	 * must still carry NO .mono_lsda bytes - MonoLSDAStreamer's own
	 * "declined || clauses.empty ()" guard is the second, independent
	 * fail-safe line and must hold regardless. */
	LLVMContext c2;
	Module m2 ("selftest.eh.cleanup.emit", c2);
	build_eh_cleanup_module (m2);
	auto obj = mono::compile_object_with_mono_compiler (m2);
	if (!obj) {
		printf ("     emit failed: %s\n", toString (obj.takeError ()).c_str ());
		return TEST_FAIL;
	}
	auto bytes = object_section_bytes (**obj, ".mono_lsda");
	if (!bytes) {
		printf ("     section read failed: %s\n", toString (bytes.takeError ()).c_str ());
		return TEST_FAIL;
	}
	CHECK (bytes->empty ());

	printf ("     cleanup-only landing pad: .mono_lsda absent (%zu bytes) - "
	        "fail-safe holds end-to-end\n", bytes->size ());
	return TEST_PASS;
}

/* --------------------------------------------- reclamation integration (J4) */

/*
 * Build, in `m`, an EH-bearing function that is SELF-CONTAINED: the invoked
 * callee is DEFINED (internal linkage, empty body) rather than an external
 * declaration, and the landing pad's type_info global carries its own
 * initializer. So compiling this through the real MonoLLVMJIT::compile() needs
 * no prior register_symbol() call and no ordering dependency on any other
 * test - it links standalone. It carries a personality-bearing
 * invoke/landingpad, which is what makes LLVM actually emit a `.eh_frame` FDE.
 * Returns `i64 <fn_name>(void)`, which returns 1 on the normal path (the
 * landing pad is never entered at runtime).
 */
static Function *
build_reclaim_eh_module (Module &m, const char *fn_name)
{
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	PointerType *ptr = PointerType::getUnqual (ctx);

	/* Internally-defined, not external: no runtime-helper registration
	 * dependency. */
	FunctionType *callee_ty = FunctionType::get (Type::getVoidTy (ctx), false);
	Function *callee = Function::Create (callee_ty, Function::InternalLinkage,
	                                     std::string (fn_name) + "_callee", &m);
	BasicBlock *cbb = BasicBlock::Create (ctx, "entry", callee);
	IRBuilder<> cib (cbb);
	cib.CreateRetVoid ();

	/* The clause-index-smuggling global, self-initialized (no external
	 * definition needed at link time). */
	auto *type_info = new GlobalVariable (m, i32, false, GlobalValue::ExternalLinkage,
	                                      ConstantInt::get (i32, 0),
	                                      std::string (fn_name) + "_type_info_0");

	FunctionType *fty = FunctionType::get (i64, false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, fn_name, &m);

	/* Mirror translator-call.cpp:948-958: i32 (...) nounwind, returns 0. */
	FunctionType *pers_ty = FunctionType::get (i32, /*isVarArg*/ true);
	Function *pers = Function::Create (pers_ty, Function::ExternalLinkage,
	                                   std::string (fn_name) + "_personality", &m);
	pers->addFnAttr (Attribute::NoUnwind);
	BasicBlock *pbb = BasicBlock::Create (ctx, "ENTRY", pers);
	IRBuilder<> pb (pbb);
	pb.CreateRet (ConstantInt::get (i32, 0));
	fn->setPersonalityFn (pers);

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
	lp->addClause (type_info);
	lb.CreateRet (lb.CreateExtractValue (lp, 1));

	return fn;
}

/* Compile build_reclaim_eh_module(fn_name) through the real engine, under `owner`. */
static mono::CompileResult
compile_reclaim_eh_module_under_owner (MonoLLVMJIT *jit, const char *fn_name, void *owner)
{
	LLVMContext &ctx = jit->context ();
	auto module = std::make_unique<Module> (std::string ("selftest.reclaim.") + fn_name, ctx);
	Function *fn = build_reclaim_eh_module (*module, fn_name);
	return jit->compile (fn, {}, nullptr, "", owner);
}

/*
 * The J4 integration test doc 26 Q5 asks for: prove the FDE ordering/lifecycle
 * invariant that JL1's dropped ~MonoJitMemoryManager assert used to guard, now
 * that there is no per-object destructor to assert from (see release_owner ()'s
 * comment in engine.cpp).
 *
 * Drives the exact path #16 (1e1427f7ab4) uses for reclamation -
 * MonoLLVMJIT::release_owner () -> ExecutionSession::removeJITDylibs () ->
 * EHFrameRegistrationPlugin::notifyRemovingResources () -> the mono-owned
 * MonoEHFrameRegistrar - and reads back mono::eh_frame_registry_stats () (the
 * accessor engine.hpp exposes for exactly this test) before compiling, right
 * after compiling, and right after release_owner ():
 *
 *   - compiling must register EXACTLY ONE new, NON-EMPTY range (the whole
 *     point: if this were ever a nounwind leaf emitting no .eh_frame at all,
 *     everything below would pass vacuously - CompileResult.eh_frame.addr/size
 *     are asserted non-null/non-zero to rule that out);
 *   - release_owner () must report exactly one dylib removed;
 *   - that release must deregister EXACTLY the range that was registered (the
 *     accounting delta), and it must no longer appear in the live set - i.e.
 *     no stale FDE survives its own reclamation.
 */
static TestResult
test_reclamation_deregisters_eh_frame (MonoLLVMJIT *jit)
{
	static int owner_tag;
	void *owner = &owner_tag;

	mono::EhFrameRegistryStats before = mono::eh_frame_registry_stats ();

	mono::CompileResult res =
		compile_reclaim_eh_module_under_owner (jit, "reclaim_probe", owner);
	CHECK (res.entry != 0);
	CHECK (res.code_size > 0);

	/* The load-bearing check: a REAL, non-zero .eh_frame range was captured -
	 * so the assertions below are exercising a live invariant, not a vacuous
	 * one. */
	CHECK (res.eh_frame.addr != nullptr);
	CHECK (res.eh_frame.size > 0);

	/* Normal path runs (the landing pad is never entered at runtime). */
	auto compiled = reinterpret_cast<int64_t (*) (void)> (res.entry);
	CHECK (compiled () == 1);

	auto is_our_range = [&] (const mono::EhFrameInfo &e) {
		return e.addr == res.eh_frame.addr && e.size == res.eh_frame.size;
	};

	mono::EhFrameRegistryStats after_compile = mono::eh_frame_registry_stats ();
	CHECK (after_compile.registered == before.registered + 1);
	CHECK (after_compile.deregistered == before.deregistered);
	CHECK (std::any_of (after_compile.live.begin (), after_compile.live.end (),
	                    is_our_range));

	/* Reclaim through the SAME path a domain unload uses (engine.hpp's
	 * release_owner (), which mono_llvm_jit_release_domain () calls). */
	CHECK (jit->release_owner (owner) == 1);

	mono::EhFrameRegistryStats after_release = mono::eh_frame_registry_stats ();
	CHECK (after_release.registered == after_compile.registered);
	CHECK (after_release.deregistered == after_compile.deregistered + 1);
	CHECK (!std::any_of (after_release.live.begin (), after_release.live.end (),
	                     is_our_range));

	printf ("     .eh_frame [%p, %p) registered then deregistered across "
	        "release_owner () (registered=%llu deregistered=%llu live=%zu)\n",
	        (void *) res.eh_frame.addr, (void *) (res.eh_frame.addr + res.eh_frame.size),
	        (unsigned long long) after_release.registered,
	        (unsigned long long) after_release.deregistered,
	        after_release.live.size ());

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

	/* First: its initial compile is the process's first, which triggers the
	 * slab reservation this check measures. */
	report ("slab-residency", test_slab_residency (jit));
	report ("arithmetic", test_arithmetic (jit));
	report ("registered-helper", test_registered_helper (jit));
	report ("release-owner", test_release_owner (jit));
	report ("compiler-equivalence", test_compiler_equivalence (jit));
	report ("eh-gather", test_eh_gather (jit));
	report ("eh-gather-multi-call", test_eh_gather_multi_call (jit));
	report ("eh-gather-kind-smuggling", test_eh_gather_kind_smuggling (jit));
	report ("eh-gather-external-typeinfo-declines",
	        test_eh_gather_external_typeinfo_declines (jit));
	report ("eh-gather-cleanup-declines", test_eh_gather_cleanup_declines (jit));
	report ("eh-mono-lsda-two-catch", test_eh_mono_lsda_two_catch (jit));
	report ("eh-mono-lsda-multi-call", test_eh_mono_lsda_multi_call (jit));
	report ("eh-mono-lsda-declined-emits-nothing",
	        test_eh_mono_lsda_declined_emits_nothing (jit));
	report ("reclamation-deregisters-eh-frame",
	        test_reclamation_deregisters_eh_frame (jit));

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

	/*
	 * All four checks here share the EXACT SAME skip boundary as reloc-widths
	 * (x86-64 ELF only, checked first thing inside each): grouping them in this
	 * program does not reintroduce the aggregation problem the comment above
	 * describes, because that problem was mixing a skippable check with
	 * unconditionally-runnable ones. These are homogeneous - either all four
	 * run, or all four skip together - so one program-level SKIP still means
	 * what it says.
	 */
	TestResult r_widths = test_reloc_widths (jit);
	report ("reloc-widths", r_widths);
	TestResult r_emachine = test_audit_relocations_gates_on_e_machine (jit);
	report ("audit-relocations-gates-on-e-machine", r_emachine);
	TestResult r_unclassified = test_reloc_unclassified_type_not_truncating (jit);
	report ("reloc-unclassified-type-not-truncating", r_unclassified);
	TestResult r_first_offender = test_reloc_first_offender_matches_first_scan (jit);
	report ("reloc-first-offender-matches-first-scan", r_first_offender);

	/*
	 * NOT gated on the host-arch skip boundary above: it builds its own
	 * x86-64-triple LinkGraphs by hand and never touches the host's own
	 * codegen, so it runs the same everywhere this TU builds at all (the same
	 * reason engine.cpp itself links the x86-64 JITLink backend
	 * unconditionally, not only on x86-64 hosts).
	 */
	TestResult r_graph_audit = test_graph_audit_cross_boundary (jit);
	report ("graph-audit-cross-boundary", r_graph_audit);

	printf ("%d passed, %d skipped, %d failed\n", passes, skips, failures);

	bool any_failed = r_widths == TEST_FAIL || r_emachine == TEST_FAIL
	                || r_unclassified == TEST_FAIL || r_first_offender == TEST_FAIL
	                || r_graph_audit == TEST_FAIL;
	bool any_ran = r_widths != TEST_SKIP || r_emachine != TEST_SKIP
	            || r_unclassified != TEST_SKIP || r_first_offender != TEST_SKIP
	            || r_graph_audit != TEST_SKIP;
	if (any_failed)
		return 1;
	/* 77 is automake's SKIP exit status; see the note above. */
	if (!any_ran)
		return 77;
	return 0;
}

#endif /* ENABLE_LLVM */
