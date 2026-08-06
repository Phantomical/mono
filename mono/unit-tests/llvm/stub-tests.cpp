/*
 * Tests for the redirectable stubs MonoJit publishes methods as.
 *
 * Three things have to hold: a call through a stub reaches whatever the stub
 * currently points at; compiled code binds to the stub rather than to the
 * target it happened to have at link time; and a runtime detour written over a
 * stub - what Harmony does to every method it patches - keeps working across a
 * redirect, and lands on the newest target once it is removed.
 */

#include "arch/arch.hpp"
#include "jit.hpp"
#include "stubs.hpp"

#include <gtest/gtest.h>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace test {
namespace {

extern "C" int32_t
stub_target_one ()
{
	return 1;
}

extern "C" int32_t
stub_target_two ()
{
	return 2;
}

extern "C" int32_t
stub_target_three ()
{
	return 3;
}

extern "C" int32_t
stub_detour_target ()
{
	return 99;
}

using IntFn = int32_t (*) ();

/*
 * create_stub ()/create_lazy_stub () reserve the stub; the tests want the
 * address it was reserved at too, which is a call of its own.
 */
static Expected<void *>
make_stub (MonoJit &jit, const std::string &name, void *target)
{
	if (Error err = jit.create_stub (name, target))
		return std::move (err);
	return jit.stub_address (name);
}

static Expected<void *>
make_lazy_stub (MonoJit &jit, const std::string &name,
                MonoJit::LazyCompileFunction compile)
{
	if (Error err = jit.create_lazy_stub (name, std::move (compile)))
		return std::move (err);
	return jit.stub_address (name);
}

/*
 * Enough arguments to fill the SysV registers and spill: eight integers over
 * six integer registers, nine doubles over eight vector ones. A trampoline
 * that dropped a register, or that returned with the stack shifted, gets a
 * wrong answer here rather than a plausible one, since every argument is
 * weighted by its position.
 */
extern "C" int64_t
lazy_many_args (int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                int64_t f, int64_t g, int64_t h, double i, double j, double k,
                double l, double m, double n, double o, double p, double q)
{
	int64_t ints = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
	double doubles =
		i + 2 * j + 3 * k + 4 * l + 5 * m + 6 * n + 7 * o + 8 * p + 9 * q;
	return ints + static_cast<int64_t> (doubles);
}

using ManyArgsFn = int64_t (*) (int64_t, int64_t, int64_t, int64_t, int64_t,
                                int64_t, int64_t, int64_t, double, double,
                                double, double, double, double, double, double,
                                double);

/// Call FN with the fixed argument list the checks below compare against.
static int64_t
call_many_args (ManyArgsFn fn)
{
	return fn (1, 2, 3, 4, 5, 6, 7, 8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0,
	           9.0);
}

/// i32 constant() { return VALUE; }, so a lazy compile has something real to
/// produce rather than a function that was there all along.
static ThreadSafeModule
build_constant_module (int32_t value)
{
	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> ("stub.constant", *context);

	FunctionType *fty = FunctionType::get (Type::getInt32Ty (*context), false);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "constant",
	                                 module.get ());
	IRBuilder<> b (BasicBlock::Create (*context, "entry", fn));
	b.CreateRet (b.getInt32 (value));

	EXPECT_FALSE (verifyFunction (*fn, &errs ()));
	return ThreadSafeModule (std::move (module),
	                         ThreadSafeContext (std::move (context)));
}

/// i32 caller() { return callee(); } with callee external, so it can only be
/// satisfied by a published stub.
static ThreadSafeModule
build_caller_module (const char *callee_name)
{
	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> ("stub.caller", *context);

	FunctionType *fty = FunctionType::get (Type::getInt32Ty (*context), false);
	FunctionCallee callee = module->getOrInsertFunction (callee_name, fty);
	Function *fn = Function::Create (fty, Function::ExternalLinkage, "caller",
	                                 module.get ());
	IRBuilder<> b (BasicBlock::Create (*context, "entry", fn));
	b.CreateRet (b.CreateCall (callee));

	EXPECT_FALSE (verifyFunction (*fn, &errs ()));
	return ThreadSafeModule (std::move (module),
	                         ThreadSafeContext (std::move (context)));
}

/*
 * i32 entry() { return 0; } plus an external global holding a pointer to every
 * one of NAMES, so linking the module asks mono.stubs for all of them in one
 * lookup without codegen having to emit a call to each.
 */
static ThreadSafeModule
build_table_module (const std::vector<std::string> &names)
{
	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> ("stub.table", *context);

	FunctionType *fty = FunctionType::get (Type::getInt32Ty (*context), false);
	PointerType *pty = PointerType::get (*context, 0);

	std::vector<Constant *> entries;
	entries.reserve (names.size ());
	for (const std::string &name : names)
		entries.push_back (
			cast<Constant> (module->getOrInsertFunction (name, fty).getCallee ()));

	ArrayType *aty = ArrayType::get (pty, entries.size ());
	new GlobalVariable (*module, aty, true, GlobalValue::ExternalLinkage,
	                    ConstantArray::get (aty, entries), "table");

	Function *fn = Function::Create (fty, Function::ExternalLinkage, "entry",
	                                 module.get ());
	IRBuilder<> b (BasicBlock::Create (*context, "entry", fn));
	b.CreateRet (b.getInt32 (0));

	return ThreadSafeModule (std::move (module),
	                         ThreadSafeContext (std::move (context)));
}

/// Make the page(s) holding [addr, addr + len) writable, the way a detour
/// library does before patching.
static void
make_writable (void *addr, size_t len)
{
	uintptr_t page = static_cast<uintptr_t> (sysconf (_SC_PAGESIZE));
	uintptr_t start = reinterpret_cast<uintptr_t> (addr) & ~(page - 1);
	uintptr_t end =
		(reinterpret_cast<uintptr_t> (addr) + len + page - 1) & ~(page - 1);
	ASSERT_EQ (mprotect (reinterpret_cast<void *> (start), end - start,
	                     PROT_READ | PROT_WRITE | PROT_EXEC),
	           0)
		<< std::strerror (errno);
}

/*
 * The widest patch Harmony writes: an absolute `jmpq *0(%rip)` plus the
 * 8-byte destination that follows it, 14 bytes in all.
 */
constexpr size_t detour_size = 14;

static_assert (detour_size <= arch::stub_block_size,
               "a detour has to fit inside the stub it patches");

static void
write_detour (void *stub, void *target, char (&saved)[detour_size])
{
	make_writable (stub, detour_size);
	std::memcpy (saved, stub, detour_size);

	char patch[detour_size] = { '\xff', '\x25', 0, 0, 0, 0 };
	std::memcpy (patch + 6, &target, sizeof (target));
	std::memcpy (stub, patch, detour_size);
}

TEST (Stubs, CallsInitialTargetAndFollowsRedirects)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto stub = make_stub (**jit, "m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 1);

	ASSERT_FALSE (bool ((*jit)->redirect_stub ("m", (void *) &stub_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 2);

	/* The published address never moves - that is the whole point of it. */
	auto again = (*jit)->stub_address ("m");
	ASSERT_TRUE (bool (again)) << toString (again.takeError ());
	EXPECT_EQ (*again, *stub);
}

/*
 * A method is published under a name built from its printed name and its
 * MonoMethod address, and a freed method hands that address straight back to the
 * allocator - so the next method along can want the very same name. That only
 * works if undefining releases the name.
 */
TEST (Stubs, AnUndefinedNameCanBePublishedAgain)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto first = make_stub (**jit, "m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*first) (), 1);

	ASSERT_FALSE (bool ((*jit)->undefine_stubs ({ "m" })));

	auto second = make_stub (**jit, "m", (void *) &stub_target_two);
	ASSERT_TRUE (bool (second)) << toString (second.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*second) (), 2);
}

/*
 * The common case for a method that was published and then freed without ever
 * being called or linked against: no module ever named it, so undefining it has
 * no symbol to take back - only the block.
 */
TEST (Stubs, AnUnmaterializedStubCanBeUndefined)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_FALSE (bool ((*jit)->create_stub ("m", (void *) &stub_target_one)));
	ASSERT_FALSE (bool ((*jit)->undefine_stubs ({ "m" })));

	auto again = make_stub (**jit, "m", (void *) &stub_target_two);
	ASSERT_TRUE (bool (again)) << toString (again.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*again) (), 2);
}

/*
 * A stub is 24 bytes of the low-memory pool, which is one gigabyte for the
 * whole process. A program that mints DynamicMethods forever - what a compiler
 * emitting lambdas does - would walk through it if freeing a method only ever
 * gave the name back.
 */
TEST (Stubs, AnUndefinedStubIsHandedOutAgain)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto first = make_stub (**jit, "m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());

	ASSERT_FALSE (bool ((*jit)->undefine_stubs ({ "m" })));

	/* A different name, so this is the block being reused and not the name. */
	auto second = make_stub (**jit, "n", (void *) &stub_target_two);
	ASSERT_TRUE (bool (second)) << toString (second.takeError ());

	EXPECT_EQ (*first, *second);
	EXPECT_EQ (reinterpret_cast<IntFn> (*second) (), 2);
}

/*
 * The one case where the name reached the linker: a module was compiled against
 * it, so undefining has a symbol to take back as well as a block. The name has
 * to come back clean enough for the next method along to be linked against.
 */
TEST (Stubs, ANameAModuleLinkedAgainstCanBePublishedAgain)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_TRUE (bool (make_stub (**jit, "m", (void *) &stub_target_one)));

	auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	ASSERT_FALSE (bool ((*jit)->undefine_stubs ({ "m" })));
	ASSERT_TRUE (bool (make_stub (**jit, "m", (void *) &stub_target_two)));

	/* The old caller is calling a stub that has been handed out again. */
	auto again = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (again)) << toString (again.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (again->entry) (), 2);
}

/*
 * Undefining is driven by the backend's own record of what it published, so a
 * name that was never published means that record is wrong. Saying so is what
 * keeps it from becoming a stub silently pointing at released code.
 */
TEST (Stubs, UndefiningANameThatWasNeverPublishedFails)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	Error err = (*jit)->undefine_stubs ({ "m" });

	ASSERT_TRUE (bool (err));
	consumeError (std::move (err));
}

TEST (Stubs, CompiledCallersBindToTheStub)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_TRUE (bool (make_stub (**jit, "m", (void *) &stub_target_one)));

	auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	/*
	 * The caller was compiled and linked while the stub pointed at target one.
	 * If it had bound to that address, this would still return 1 - a promotion
	 * would be invisible to everything already compiled.
	 */
	ASSERT_FALSE (bool ((*jit)->redirect_stub ("m", (void *) &stub_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2);
}

TEST (Stubs, GeometryLeavesRoomForADetour)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto a = make_stub (**jit, "a", (void *) &stub_target_one);
	auto b = make_stub (**jit, "b", (void *) &stub_target_two);
	ASSERT_TRUE (bool (a)) << toString (a.takeError ());
	ASSERT_TRUE (bool (b)) << toString (b.takeError ());

	char bytes[arch::stub_block_size];
	std::memcpy (bytes, *a, sizeof (bytes));

	/* jmpq *ptr(%rip) ... */
	EXPECT_EQ (static_cast<unsigned char> (bytes[0]), 0xff);
	EXPECT_EQ (static_cast<unsigned char> (bytes[1]), 0x25);

	/* ... and the rest of the block is ours, so a 14-byte patch fits. */
	for (size_t i = 6; i < arch::stub_block_size; i++)
		EXPECT_EQ (static_cast<unsigned char> (bytes[i]), 0xcc)
			<< "stub byte " << i << " is not padding";

	EXPECT_EQ (reinterpret_cast<uintptr_t> (*a) % arch::stub_alignment, 0u);

	uintptr_t delta = reinterpret_cast<uintptr_t> (*a) >
	                          reinterpret_cast<uintptr_t> (*b)
	                      ? reinterpret_cast<uintptr_t> (*a) -
	                            reinterpret_cast<uintptr_t> (*b)
	                      : reinterpret_cast<uintptr_t> (*b) -
	                            reinterpret_cast<uintptr_t> (*a);
	EXPECT_GE (delta, arch::stub_block_size)
		<< "stubs overlap: patching one would clobber the other";
}

TEST (Stubs, SurvivesADetourAcrossRedirects)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto stub = make_stub (**jit, "m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	char saved[detour_size];
	write_detour (*stub, (void *) &stub_detour_target, saved);
	if (::testing::Test::HasFatalFailure ())
		return;

	/* Both the direct call and the compiled caller now divert. */
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 99);

	/*
	 * A promotion while the method is patched writes a slot nothing reads any
	 * more. The patch has to win - that is the semantics a patched method
	 * needs - but the slot still has to track the newest tier.
	 */
	ASSERT_FALSE (
		bool ((*jit)->redirect_stub ("m", (void *) &stub_target_three)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 99);

	/* Unpatching restores the jump, which lands on the newest tier. */
	std::memcpy (*stub, saved, detour_size);
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 3);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 3);
}

/*
 * One stub per method means a Unity-sized game publishes six figures of them,
 * so what a stub costs in address space is a real number rather than an
 * aesthetic one. Stubs published one at a time still have to pack: the version
 * of this that built a LinkGraph per stub spent two pages on each.
 */
TEST (Stubs, PackTightlyWhenPublishedOneAtATime)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	constexpr int count = 512;
	std::vector<uintptr_t> addrs;
	for (int i = 0; i < count; i++) {
		auto stub = make_stub (**jit, "m" + std::to_string (i),
		                                 (void *) &stub_target_one);
		ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());
		addrs.push_back (reinterpret_cast<uintptr_t> (*stub));
	}

	std::sort (addrs.begin (), addrs.end ());
	for (size_t i = 1; i < addrs.size (); i++)
		ASSERT_GE (addrs[i] - addrs[i - 1], arch::stub_block_size)
			<< "stubs " << i - 1 << " and " << i << " overlap";

	uintptr_t span = addrs.back () - addrs.front ();
	EXPECT_LE (span, count * arch::stub_block_size)
		<< "stubs are spread over " << span / count << " bytes each";

	/* Every one of them still works. */
	EXPECT_EQ (reinterpret_cast<IntFn> (addrs.front ()) (), 1);
	EXPECT_EQ (reinterpret_cast<IntFn> (addrs.back ()) (), 1);
}

/*
 * A stub reaches mono.stubs the first time some module names it, and the
 * definition generator that puts it there runs on whichever thread is linking.
 * Several links wanting the same undefined name at once must still produce one
 * definition: two of them defining it would be a duplicate-definition error out
 * of the second, which is a compile failure the runtime raises on.
 */
TEST (Stubs, ConcurrentLinksAgainstOneStubDefineItOnce)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_TRUE (bool (make_stub (**jit, "m", (void *) &stub_target_one)));

	constexpr int threads = 16;
	std::atomic<int> ready {0};
	std::atomic<bool> go {false};
	std::vector<std::string> errors (threads);
	std::vector<int32_t> results (threads, 0);

	std::vector<std::thread> workers;
	for (int i = 0; i < threads; i++)
		workers.emplace_back ([&, i] {
			ready++;
			while (!go.load ())
				std::this_thread::yield ();

			auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
			if (!caller) {
				errors[i] = toString (caller.takeError ());
				return;
			}
			results[i] = reinterpret_cast<IntFn> (caller->entry) ();
		});

	while (ready.load () != threads)
		std::this_thread::yield ();
	go.store (true);
	for (std::thread &t : workers)
		t.join ();

	for (int i = 0; i < threads; i++) {
		EXPECT_EQ (errors[i], "") << "thread " << i;
		EXPECT_EQ (results[i], 1) << "thread " << i;
	}
}

/*
 * Undefining a name while a link is asking mono.stubs for it. The linker
 * claims a batch of names before it defines any of them, so an undefine landing
 * in the middle can decide the linker knows about a name whose definition has
 * not been written yet - and then find nothing to take back, leave the
 * definition standing, and hand the block to the next method along.
 *
 * The module names its stubs from a pointer table rather than by calling each
 * one, which makes the lookup set large without making codegen large, so the
 * window this is aiming at is wide enough to hit. Where in a compile the link
 * happens is a property of the machine, so one compile is timed first and the
 * undefines are spread across it rather than fired at a guessed delay.
 */
TEST (Stubs, UndefiningRacesALinkNamingTheSameStubs)
{
	constexpr int count = 4000;
	constexpr int rounds = 16;

	std::vector<std::string> names;
	for (int i = 0; i < count; i++)
		names.push_back ("s" + std::to_string (i));

	auto reserve_all = [&] (MonoJit &jit, void *target) {
		for (const std::string &name : names)
			if (Error err = jit.create_stub (name, target))
				return toString (std::move (err));
		return std::string ();
	};

	int64_t compile_us = 0;
	{
		auto jit = MonoJit::create ();
		ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());
		ASSERT_EQ (reserve_all (**jit, (void *) &stub_target_one), "");

		auto start = std::chrono::steady_clock::now ();
		auto compiled = (*jit)->compile (build_table_module (names), "entry");
		ASSERT_TRUE (bool (compiled)) << toString (compiled.takeError ());
		compile_us = std::chrono::duration_cast<std::chrono::microseconds> (
			             std::chrono::steady_clock::now () - start)
		                     .count ();
	}

	for (int round = 0; round < rounds; round++) {
		auto jit = MonoJit::create ();
		ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());
		ASSERT_EQ (reserve_all (**jit, (void *) &stub_target_one), "");

		std::atomic<int> ready {0};
		std::atomic<bool> go {false};
		std::string undef_err;
		auto delay = std::chrono::microseconds (compile_us * (round + 4) /
		                                        (rounds + 5));

		std::thread linker ([&] {
			ready++;
			while (!go.load ())
				std::this_thread::yield ();
			/*
			 * Either outcome is fine: the undefine either got there first, in
			 * which case there is nothing left to link against, or it did not.
			 */
			consumeError (
				(*jit)->compile (build_table_module (names), "entry").takeError ());
		});
		std::thread undefiner ([&] {
			ready++;
			while (!go.load ())
				std::this_thread::yield ();
			std::this_thread::sleep_for (delay);
			if (Error err = (*jit)->undefine_stubs (names))
				undef_err = toString (std::move (err));
		});

		while (ready.load () != 2)
			std::this_thread::yield ();
		go.store (true);
		linker.join ();
		undefiner.join ();

		ASSERT_EQ (undef_err, "") << "round " << round;

		/*
		 * Whatever the linker managed to do, every name has to have come back
		 * clean. A definition left standing over a removed table entry shows up
		 * here: publishing the name again and linking against it either finds
		 * the old stub - which nothing redirects any more - or collides with it.
		 */
		ASSERT_EQ (reserve_all (**jit, (void *) &stub_target_two), "")
			<< "round " << round;

		auto caller = (*jit)->compile (build_caller_module ("s0"), "caller");
		ASSERT_TRUE (bool (caller))
			<< "round " << round << ": " << toString (caller.takeError ());
		ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2)
			<< "round " << round;
	}
}

/*
 * The generator runs inside ORC's lookup machinery and defines into mono.stubs
 * from there; undefining goes the other way, taking the session lock while the
 * table is settled. Threads publishing, linking, redirecting and undefining at
 * once are what says those two orders agree - a disagreement hangs rather than
 * fails, so this test failing means it timed out.
 */
TEST (Stubs, PublishRedirectAndUndefineFromManyThreads)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	constexpr int threads = 8;
	constexpr int iterations = 24;
	std::atomic<int> ready {0};
	std::atomic<bool> go {false};
	std::vector<std::string> errors (threads);

	std::vector<std::thread> workers;
	for (int i = 0; i < threads; i++)
		workers.emplace_back ([&, i] {
			auto fail = [&] (Error err) {
				if (errors[i].empty ())
					errors[i] = toString (std::move (err));
				else
					consumeError (std::move (err));
			};

			ready++;
			while (!go.load ())
				std::this_thread::yield ();

			/* Each thread owns its names, so the contention is all in ORC. */
			for (int n = 0; n < iterations; n++) {
				std::string name =
					"t" + std::to_string (i) + "." + std::to_string (n);

				if (Error err = (*jit)->create_stub (
				        name, (void *) &stub_target_one)) {
					fail (std::move (err));
					return;
				}

				auto caller = (*jit)->compile (
					build_caller_module (name.c_str ()), "caller");
				if (!caller) {
					fail (caller.takeError ());
					return;
				}
				EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

				if (Error err = (*jit)->redirect_stub (
				        name, (void *) &stub_target_two)) {
					fail (std::move (err));
					return;
				}
				EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2);

				if (Error err = (*jit)->undefine_stubs ({ name })) {
					fail (std::move (err));
					return;
				}
			}
		});

	while (ready.load () != threads)
		std::this_thread::yield ();
	go.store (true);
	for (std::thread &t : workers)
		t.join ();

	for (int i = 0; i < threads; i++)
		EXPECT_EQ (errors[i], "") << "thread " << i;
}

TEST (LazyStubs, CompileOnceOnTheFirstCall)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	int compiles = 0;
	auto stub = make_lazy_stub (**jit, "m", [&] () -> Expected<void *> {
		compiles++;
		auto compiled = (*jit)->compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
	});
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	/* Publishing it compiles nothing. */
	EXPECT_EQ (compiles, 0);

	for (int i = 0; i < 3; i++)
		EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 7) << "call " << i;
	EXPECT_EQ (compiles, 1);
}

/*
 * The compile happens in the middle of the call it was triggered by, so the
 * trampoline has to hand every argument back untouched before continuing into
 * the code it just produced.
 */
TEST (LazyStubs, ArgumentsSurviveTheCompileTheyTriggered)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto stub = make_lazy_stub (**jit, 
		"m", [] () -> Expected<void *> { return (void *) &lazy_many_args; });
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	int64_t expected = call_many_args (&lazy_many_args);

	/* Through the trampoline ... */
	EXPECT_EQ (call_many_args (reinterpret_cast<ManyArgsFn> (*stub)), expected);
	/* ... and again now that the stub points straight at the code. */
	EXPECT_EQ (call_many_args (reinterpret_cast<ManyArgsFn> (*stub)), expected);
}

TEST (LazyStubs, CallersCanBeCompiledBeforeTheCodeExists)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	int compiles = 0;
	ASSERT_TRUE (
		bool (make_lazy_stub (**jit, "m", [&] () -> Expected<void *> {
			compiles++;
			auto compiled = (*jit)->compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
		})));

	/* Resolves against a method that has no code yet. */
	auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (compiles, 0);

	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 7);
	EXPECT_EQ (compiles, 1);
}

TEST (LazyStubs, RacingFirstCallsCompileOnce)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	std::atomic<int> compiles {0};
	auto stub = make_lazy_stub (**jit, "m", [&] () -> Expected<void *> {
		compiles++;
		auto compiled = (*jit)->compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
	});
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	constexpr int threads = 8;
	std::atomic<int> ready {0};
	std::atomic<bool> go {false};
	std::vector<int32_t> results (threads, 0);

	std::vector<std::thread> workers;
	for (int i = 0; i < threads; i++)
		workers.emplace_back ([&, i] {
			ready++;
			while (!go.load ())
				std::this_thread::yield ();
			results[i] = reinterpret_cast<IntFn> (*stub) ();
		});

	while (ready.load () != threads)
		std::this_thread::yield ();
	go.store (true);
	for (std::thread &t : workers)
		t.join ();

	EXPECT_EQ (compiles.load (), 1);
	for (int i = 0; i < threads; i++)
		EXPECT_EQ (results[i], 7) << "thread " << i;
}

} // namespace
} // namespace test
} // namespace mono
