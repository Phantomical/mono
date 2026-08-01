/*
 * Tests for the redirectable stubs MonoJit publishes methods as.
 *
 * Three things have to hold: a call through a stub reaches whatever the stub
 * currently points at; compiled code binds to the stub rather than to the
 * target it happened to have at link time; and a runtime detour written over a
 * stub - what Harmony does to every method it patches - keeps working across a
 * redirect, and lands on the newest target once it is removed.
 */

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
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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

static_assert (detour_size <= stub_block_size,
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

	auto stub = (*jit)->create_stub ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 1);

	ASSERT_FALSE (bool ((*jit)->redirect_stub ("m", (void *) &stub_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 2);

	/* The published address never moves - that is the whole point of it. */
	auto again = (*jit)->stub_address ("m");
	ASSERT_TRUE (bool (again)) << toString (again.takeError ());
	EXPECT_EQ (*again, *stub);
}

TEST (Stubs, CompiledCallersBindToTheStub)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	ASSERT_TRUE (bool ((*jit)->create_stub ("m", (void *) &stub_target_one)));

	auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*caller) (), 1);

	/*
	 * The caller was compiled and linked while the stub pointed at target one.
	 * If it had bound to that address, this would still return 1 - a promotion
	 * would be invisible to everything already compiled.
	 */
	ASSERT_FALSE (bool ((*jit)->redirect_stub ("m", (void *) &stub_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*caller) (), 2);
}

TEST (Stubs, GeometryLeavesRoomForADetour)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto a = (*jit)->create_stub ("a", (void *) &stub_target_one);
	auto b = (*jit)->create_stub ("b", (void *) &stub_target_two);
	ASSERT_TRUE (bool (a)) << toString (a.takeError ());
	ASSERT_TRUE (bool (b)) << toString (b.takeError ());

	char bytes[stub_block_size];
	std::memcpy (bytes, *a, sizeof (bytes));

	/* jmpq *ptr(%rip) ... */
	EXPECT_EQ (static_cast<unsigned char> (bytes[0]), 0xff);
	EXPECT_EQ (static_cast<unsigned char> (bytes[1]), 0x25);

	/* ... and the rest of the block is ours, so a 14-byte patch fits. */
	for (size_t i = 6; i < stub_block_size; i++)
		EXPECT_EQ (static_cast<unsigned char> (bytes[i]), 0xcc)
			<< "stub byte " << i << " is not padding";

	EXPECT_EQ (reinterpret_cast<uintptr_t> (*a) % stub_alignment, 0u);

	uintptr_t delta = reinterpret_cast<uintptr_t> (*a) >
	                          reinterpret_cast<uintptr_t> (*b)
	                      ? reinterpret_cast<uintptr_t> (*a) -
	                            reinterpret_cast<uintptr_t> (*b)
	                      : reinterpret_cast<uintptr_t> (*b) -
	                            reinterpret_cast<uintptr_t> (*a);
	EXPECT_GE (delta, stub_block_size)
		<< "stubs overlap: patching one would clobber the other";
}

TEST (Stubs, SurvivesADetourAcrossRedirects)
{
	auto jit = MonoJit::create ();
	ASSERT_TRUE (bool (jit)) << toString (jit.takeError ());

	auto stub = (*jit)->create_stub ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	auto caller = (*jit)->compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (*caller) (), 1);

	char saved[detour_size];
	write_detour (*stub, (void *) &stub_detour_target, saved);
	if (::testing::Test::HasFatalFailure ())
		return;

	/* Both the direct call and the compiled caller now divert. */
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (*caller) (), 99);

	/*
	 * A promotion while the method is patched writes a slot nothing reads any
	 * more. The patch has to win - that is the semantics a patched method
	 * needs - but the slot still has to track the newest tier.
	 */
	ASSERT_FALSE (
		bool ((*jit)->redirect_stub ("m", (void *) &stub_target_three)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (*caller) (), 99);

	/* Unpatching restores the jump, which lands on the newest tier. */
	std::memcpy (*stub, saved, detour_size);
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 3);
	EXPECT_EQ (reinterpret_cast<IntFn> (*caller) (), 3);
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
		auto stub = (*jit)->create_stub ("m" + std::to_string (i),
		                                 (void *) &stub_target_one);
		ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());
		addrs.push_back (reinterpret_cast<uintptr_t> (*stub));
	}

	std::sort (addrs.begin (), addrs.end ());
	for (size_t i = 1; i < addrs.size (); i++)
		ASSERT_GE (addrs[i] - addrs[i - 1], stub_block_size)
			<< "stubs " << i - 1 << " and " << i << " overlap";

	uintptr_t span = addrs.back () - addrs.front ();
	EXPECT_LE (span, count * stub_block_size)
		<< "stubs are spread over " << span / count << " bytes each";

	/* Every one of them still works. */
	EXPECT_EQ (reinterpret_cast<IntFn> (addrs.front ()) (), 1);
	EXPECT_EQ (reinterpret_cast<IntFn> (addrs.back ()) (), 1);
}

} // namespace
} // namespace test
} // namespace mono
