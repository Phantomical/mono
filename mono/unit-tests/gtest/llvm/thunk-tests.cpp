/*
 * Tests for the redirectable thunks a method is published as.
 *
 * Three things have to hold: a call through a thunk reaches whatever the
 * thunk currently points at; compiled code binds to the thunk rather than to
 * the target it happened to have at link time; and a runtime detour written
 * over a thunk - what Harmony does to every method it patches - keeps
 * working across a redirect, and lands on the newest target once it is
 * removed.
 *
 * MonoJit resolves a module's callee references per-compile now, from
 * addresses the caller already has rather than from anything published under
 * a name it keeps itself - see MonoJit::compile ()'s module_symbols
 * parameter. The Engine below stands in for that caller: it carves thunks the
 * same way the backend does, and keeps its own local name -> Thunk map so a
 * test can carve once and reference the same thunk from several compiles,
 * the way the backend's MonoDomainMethod table does.
 */

#include "arch/arch.hpp"
#include "callbacks.hpp"
#include "harness.hpp"
#include "jitlink-memory.hpp"
#include "jit.hpp"

#include <mono/mini/thunk.hpp>

#include <mono/arch/amd64/amd64-thunk.hpp>
#include <mono/metadata/abi-details.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/object.h>

#include <gtest/gtest.h>

#include <llvm/ADT/StringMap.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <mono/utils/mono-mmap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

namespace mono {
namespace test {
namespace {

extern "C" int32_t
thunk_target_one ()
{
	return 1;
}

extern "C" int32_t
thunk_target_two ()
{
	return 2;
}

extern "C" int32_t
thunk_target_three ()
{
	return 3;
}

extern "C" int32_t
thunk_detour_target ()
{
	return 99;
}

/* The two below stand in for a value type's method. They report the receiver
 * they were entered with, which is what the unbox entry acted on. */
extern "C" uint8_t *
thunk_receiver_target_one (uint8_t *receiver)
{
	return receiver;
}

extern "C" uint8_t *
thunk_receiver_target_two (uint8_t *receiver)
{
	return receiver + 1;
}

using IntFn = int32_t (*) ();
using ReceiverFn = uint8_t *(*) (uint8_t *);

/// Where a lazy thunk lands when the compile behind it failed, which no case
/// here arranges for.
[[noreturn]] static void
lazy_failed ()
{
	ADD_FAILURE () << "a lazy thunk's compile failed";
	std::abort ();
}

/*
 * A domain's engine: the domain's code manager, the linker over it, the
 * thunks carved out of it and the trampolines an uncompiled one points at.
 *
 * A published thunk is not named anywhere MonoJit can see. The map here is
 * this Engine's own bookkeeping - the equivalent of a backend's
 * MonoDomainMethod table - so a test can carve a thunk once under a name and
 * reference it from several compiles, the way a caller's module resolves a
 * callee it already has the record for.
 */
class Engine {
public:
	static Expected<std::unique_ptr<Engine>> create ()
	{
		auto self = std::unique_ptr<Engine> (new Engine ());
		auto jit = MonoJit::create (&self->arena_);

		if (!jit)
			return jit.takeError ();
		self->jit_ = std::move (*jit);

		auto callbacks = LazyCallbacks::create ((void *) &lazy_failed);
		if (!callbacks)
			return callbacks.takeError ();
		self->callbacks_ = std::move (*callbacks);

		return self;
	}

	MonoJit &jit () { return *jit_; }

	/// Publish NAME as a thunk jumping to TARGET and return the address
	/// callers reach it at.
	Expected<void *> publish (StringRef name, void *target)
	{
		Expected<Thunk> thunk = Thunk::allocate (&arena_, nullptr);

		if (!thunk)
			return thunk.takeError ();

		thunk->redirect (target);

		std::lock_guard<std::mutex> lock (published_mutex_);
		published_[name] = *thunk;
		return thunk->code ();
	}

	/// Publish NAME as a thunk that runs COMPILE on its first call and goes
	/// straight to what that produced from then on - what a method published
	/// before it is compiled looks like.
	Expected<void *> publish_lazy (StringRef name,
	                               unique_function<Expected<void *> ()> compile)
	{
		std::string owned = name.str ();
		Expected<void *> trampoline = callbacks_->reserve (
			[this, owned, compile = std::move (compile)] () mutable -> void * {
				Expected<void *> code = compile ();

				if (!code) {
					ADD_FAILURE ()
						<< toString (code.takeError ());
					return (void *) &lazy_failed;
				}

				std::lock_guard<std::mutex> lock (published_mutex_);
				published_[owned].redirect (*code);
				return *code;
			});
		if (!trampoline)
			return trampoline.takeError ();

		Expected<Thunk> thunk = Thunk::allocate (&arena_, nullptr);

		if (!thunk) {
			callbacks_->release (*trampoline);
			return thunk.takeError ();
		}

		thunk->redirect (*trampoline);

		std::lock_guard<std::mutex> lock (published_mutex_);
		published_[name] = *thunk;
		trampolines_[name] = *trampoline;
		return thunk->code ();
	}

	Error redirect (StringRef name, void *target)
	{
		std::lock_guard<std::mutex> lock (published_mutex_);
		auto it = published_.find (name);

		if (it == published_.end ())
			return createStringError (inconvertibleErrorCode (),
			                          "no thunk was published for %s",
			                          name.str ().c_str ());
		it->second.redirect (target);
		return Error::success ();
	}

	Expected<void *> address (StringRef name)
	{
		std::lock_guard<std::mutex> lock (published_mutex_);
		auto it = published_.find (name);

		if (it == published_.end ())
			return createStringError (inconvertibleErrorCode (),
			                          "no thunk was published for %s",
			                          name.str ().c_str ());
		return it->second.code ();
	}

	/// Give NAMES' trampolines back and quarantine their thunks - what the
	/// backend does to a method's thunk when it is freed.
	Error retire (ArrayRef<std::string> names)
	{
		for (const std::string &name : names) {
			std::lock_guard<std::mutex> lock (published_mutex_);
			auto it = published_.find (name);

			if (it == published_.end ())
				return createStringError (inconvertibleErrorCode (),
				                          "no thunk was published for %s",
				                          name.c_str ());

			auto trampoline = trampolines_.find (name);
			if (trampoline != trampolines_.end ()) {
				callbacks_->release (trampoline->second);
				trampolines_.erase (trampoline);
			}

			it->second.quarantine ();
			published_.erase (it);
		}
		return Error::success ();
	}

	/// Compile a module whose undefined references name published thunks,
	/// resolving NAMES against what this engine has for them - the same
	/// per-module binding resolve_externals ()/MonoJit::compile () use in the
	/// backend, in place of a shared table a link would otherwise search.
	Expected<CompiledMethod> compile_against (ThreadSafeModule tsm, StringRef entry,
	                                          ArrayRef<StringRef> names)
	{
		std::vector<std::pair<StringRef, void *>> symbols;

		for (StringRef name : names) {
			Expected<void *> addr = address (name);

			if (!addr)
				return addr.takeError ();
			symbols.emplace_back (name, *addr);
		}

		return jit_->compile (std::move (tsm), entry, symbols);
	}

private:
	Engine () = default;

	/// Declared first, so it outlives everything carved out of it.
	CodeArena arena_;
	std::unique_ptr<MonoJit> jit_;
	std::unique_ptr<LazyCallbacks> callbacks_;
	/// published_/trampolines_ are this Engine's own bookkeeping and need
	/// their own lock; allocate_thunk () itself has no shared state beyond
	/// CodeArena::reserve ()'s, which is already thread safe on its own.
	std::mutex published_mutex_;
	StringMap<Thunk> published_;
	StringMap<void *> trampolines_;
};

/*
 * Thunks come out of the root domain's code manager, so these need a runtime
 * - which needs a class library to boot on.
 */
class Thunks : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}
};

class LazyThunks : public Thunks {};

/// Build an engine, or fail the case that asked for one.
static std::unique_ptr<Engine>
make_engine ()
{
	Expected<std::unique_ptr<Engine>> engine = Engine::create ();

	if (!engine) {
		ADD_FAILURE () << toString (engine.takeError ());
		return nullptr;
	}
	return std::move (*engine);
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
	auto module = std::make_unique<Module> ("thunk.constant", *context);

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
/// satisfied by module_symbols naming it.
static ThreadSafeModule
build_caller_module (const char *callee_name)
{
	auto context = std::make_unique<LLVMContext> ();
	auto module = std::make_unique<Module> ("thunk.caller", *context);

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
	uintptr_t page = static_cast<uintptr_t> (mono_pagesize ());
	uintptr_t start = reinterpret_cast<uintptr_t> (addr) & ~(page - 1);
	uintptr_t end =
		(reinterpret_cast<uintptr_t> (addr) + len + page - 1) & ~(page - 1);
	ASSERT_EQ (mono_mprotect (reinterpret_cast<void *> (start), end - start,
	                          MONO_MMAP_READ | MONO_MMAP_WRITE | MONO_MMAP_EXEC),
	           0)
		<< std::strerror (errno);
}

/*
 * The widest patch Harmony writes: an absolute `jmpq *0(%rip)` plus the
 * 8-byte destination that follows it, 14 bytes in all.
 */
constexpr size_t detour_size = 14;

static_assert (detour_size <= arch::thunk_block_size,
               "a detour has to fit inside the thunk it patches");

static void
write_detour (void *thunk, void *target, char (&saved)[detour_size])
{
	make_writable (thunk, detour_size);
	std::memcpy (saved, thunk, detour_size);

	char patch[detour_size] = { '\xff', '\x25', 0, 0, 0, 0 };
	std::memcpy (patch + 6, &target, sizeof (target));
	std::memcpy (thunk, patch, detour_size);
}

TEST_F (Thunks, CallsInitialTargetAndFollowsRedirects)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto thunk = engine->publish ("m", (void *) &thunk_target_one);
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*thunk) (), 1);

	ASSERT_FALSE (bool (engine->redirect ("m", (void *) &thunk_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*thunk) (), 2);

	/* The published address never moves - that is the whole point of it. */
	auto again = engine->address ("m");
	ASSERT_TRUE (bool (again)) << toString (again.takeError ());
	EXPECT_EQ (*again, *thunk);
}

/*
 * The entry a call off a value type's vtable or IMT arrives at. It sits in
 * front of the thunk and runs into it, so it needs no target of its own.
 * Whatever the thunk points at is where the receiver arrives, one object
 * header on.
 *
 * Driven off allocate_thunk () directly rather than the Engine: nothing here
 * needs a name for the thunk at all.
 */
TEST_F (Thunks, TheUnboxEntryStepsTheReceiverPastTheObjectHeader)
{
	CodeArena arena;

	Expected<Thunk> thunk = Thunk::allocate (&arena, nullptr);
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

	thunk->redirect ((void *) &thunk_receiver_target_one);

	auto unbox = reinterpret_cast<ReceiverFn> (thunk->unbox ());
	uint8_t boxed[64] = {};

	EXPECT_EQ (unbox (boxed), boxed + MONO_ABI_SIZEOF (MonoObject));

	/* The fall-through is what makes this right at every tier. Nothing
	 * rewrites the entry, and a redirect of the thunk moves it too. */
	thunk->redirect ((void *) &thunk_receiver_target_two);
	EXPECT_EQ (unbox (boxed), boxed + MONO_ABI_SIZEOF (MonoObject) + 1);
}

TEST_F (Thunks, CompiledCallersBindToTheThunk)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	ASSERT_TRUE (bool (engine->publish ("m", (void *) &thunk_target_one)));

	auto caller = engine->compile_against (build_caller_module ("m"), "caller", { "m" });
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	/*
	 * The caller was compiled and linked while the thunk pointed at target
	 * one. If it had bound to that address, this would still return 1 - a
	 * promotion would be invisible to everything already compiled.
	 */
	ASSERT_FALSE (bool (engine->redirect ("m", (void *) &thunk_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2);
}

TEST_F (Thunks, GeometryLeavesRoomForADetour)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto a = engine->publish ("a", (void *) &thunk_target_one);
	auto b = engine->publish ("b", (void *) &thunk_target_two);
	ASSERT_TRUE (bool (a)) << toString (a.takeError ());
	ASSERT_TRUE (bool (b)) << toString (b.takeError ());

	char bytes[arch::thunk_block_size];
	std::memcpy (bytes, *a, sizeof (bytes));

	/* jmpq *ptr(%rip) ... */
	EXPECT_EQ (static_cast<unsigned char> (bytes[0]), 0xff);
	EXPECT_EQ (static_cast<unsigned char> (bytes[1]), 0x25);

	/* ... and the rest of the block is ours, so a 14-byte patch fits. */
	for (size_t i = 6; i < arch::thunk_block_size; i++)
		EXPECT_EQ (static_cast<unsigned char> (bytes[i]), 0xcc)
			<< "thunk byte " << i << " is not padding";

	EXPECT_EQ (reinterpret_cast<uintptr_t> (*a) % arch::thunk_alignment, 0u);

	uintptr_t delta = reinterpret_cast<uintptr_t> (*a) >
	                          reinterpret_cast<uintptr_t> (*b)
	                      ? reinterpret_cast<uintptr_t> (*a) -
	                            reinterpret_cast<uintptr_t> (*b)
	                      : reinterpret_cast<uintptr_t> (*b) -
	                            reinterpret_cast<uintptr_t> (*a);
	EXPECT_GE (delta, arch::thunk_block_size)
		<< "thunks overlap: patching one would clobber the other";
}

TEST_F (Thunks, SurvivesADetourAcrossRedirects)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto thunk = engine->publish ("m", (void *) &thunk_target_one);
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

	auto caller = engine->compile_against (build_caller_module ("m"), "caller", { "m" });
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	char saved[detour_size];
	write_detour (*thunk, (void *) &thunk_detour_target, saved);
	if (::testing::Test::HasFatalFailure ())
		return;

	/* Both the direct call and the compiled caller now divert. */
	EXPECT_EQ (reinterpret_cast<IntFn> (*thunk) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 99);

	/*
	 * A promotion while the method is patched writes a slot nothing reads any
	 * more. The patch has to win - that is the semantics a patched method
	 * needs - but the slot still has to track the newest tier.
	 */
	ASSERT_FALSE (
		bool (engine->redirect ("m", (void *) &thunk_target_three)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*thunk) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 99);

	/* Unpatching restores the jump, which lands on the newest tier. */
	std::memcpy (*thunk, saved, detour_size);
	EXPECT_EQ (reinterpret_cast<IntFn> (*thunk) (), 3);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 3);
}

/*
 * Promotion is a redirect performed under running code: the slot is
 * rewritten while other threads are calling through the thunk and while
 * compiled callers hold its address. Every call has to come back with one of
 * the targets the thunk has actually had - a torn slot would return
 * something else, or jump into nothing - and calls made after a redirect
 * have to see it, so the redirects are not simply lost.
 */
TEST_F (Thunks, RedirectsWhileThreadsRunThroughIt)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto thunk = engine->publish ("m", (void *) &thunk_target_one);
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

	auto caller = engine->compile_against (build_caller_module ("m"), "caller", { "m" });
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	void *const targets[] = { (void *) &thunk_target_one,
		                      (void *) &thunk_target_two,
		                      (void *) &thunk_target_three };

	constexpr int readers = 8;
	constexpr int all_three = 0b111;
	std::atomic<int> ready {0};
	std::atomic<bool> go {false};
	std::atomic<bool> done {false};
	/* Bit N-1 of a reader's mask says it saw target N. */
	std::vector<std::atomic<int>> seen (readers);
	std::vector<std::atomic<int>> bad (readers);

	for (int i = 0; i < readers; i++) {
		seen[i].store (0);
		bad[i].store (0);
	}

	std::vector<std::thread> workers;
	for (int i = 0; i < readers; i++)
		workers.emplace_back ([&, i] {
			IntFn direct = reinterpret_cast<IntFn> (*thunk);
			IntFn compiled = reinterpret_cast<IntFn> (caller->entry);

			ready++;
			while (!go.load ())
				std::this_thread::yield ();

			while (!done.load ()) {
				for (int32_t value : { direct (), compiled () }) {
					if (value >= 1 && value <= 3)
						seen[i] |= 1 << (value - 1);
					else
						bad[i]++;
				}
			}
		});

	while (ready.load () != readers)
		std::this_thread::yield ();
	go.store (true);

	/*
	 * Redirect until every reader has been through all three targets rather
	 * than a fixed number of times: a reader descheduled for the length of a
	 * fixed loop would report a missed redirect that never happened.
	 */
	auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);
	std::string error;
	int n = 0;

	for (;;) {
		if (Error err = engine->redirect ("m", targets[n++ % 3])) {
			error = toString (std::move (err));
			break;
		}

		bool everyone = true;

		for (int i = 0; i < readers; i++)
			everyone &= seen[i].load () == all_three;
		if (everyone || std::chrono::steady_clock::now () > deadline)
			break;
	}

	done.store (true);
	for (std::thread &t : workers)
		t.join ();

	EXPECT_EQ (error, "");
	for (int i = 0; i < readers; i++) {
		EXPECT_EQ (bad[i].load (), 0)
			<< "reader " << i << " reached a target the thunk never had";
		EXPECT_EQ (seen[i].load (), all_three)
			<< "reader " << i << " never saw all three targets";
	}
}

/*
 * allocate_thunk () carves out of CodeArena::reserve (), which is already its
 * own lock; there is no free list or batch offset here any more for
 * concurrent carves to disagree about. What this proves now is the wider
 * claim: many threads publishing, compiling a caller against what they
 * published, redirecting it and retiring it - all at once - land on
 * consistent answers rather than a corrupted thunk or a caller bound to the
 * wrong address.
 */
TEST_F (Thunks, PublishCompileRedirectAndRetireFromManyThreads)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

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

			/* Each thread owns its names, so the contention is all in
			 * CodeArena::reserve () and MonoJit::compile ()'s dylib
			 * bookkeeping. */
			for (int n = 0; n < iterations; n++) {
				std::string name =
					"t" + std::to_string (i) + "." + std::to_string (n);

				Expected<void *> thunk =
					engine->publish (name, (void *) &thunk_target_one);

				if (!thunk) {
					fail (thunk.takeError ());
					return;
				}

				auto caller = engine->compile_against (
					build_caller_module (name.c_str ()), "caller", { name });
				if (!caller) {
					fail (caller.takeError ());
					return;
				}
				EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

				if (Error err = engine->redirect (
				        name, (void *) &thunk_target_two)) {
					fail (std::move (err));
					return;
				}
				EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2);

				if (Error err = engine->retire ({ name })) {
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

TEST_F (LazyThunks, CompileOnceOnTheFirstCall)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	int compiles = 0;
	auto thunk = engine->publish_lazy ("m", [&] () -> Expected<void *> {
		compiles++;
		auto compiled = engine->jit ().compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
	});
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

	/* Publishing it compiles nothing. */
	EXPECT_EQ (compiles, 0);

	for (int i = 0; i < 3; i++)
		EXPECT_EQ (reinterpret_cast<IntFn> (*thunk) (), 7) << "call " << i;
	EXPECT_EQ (compiles, 1);
}

/*
 * The compile happens in the middle of the call it was triggered by, so the
 * trampoline has to hand every argument back untouched before continuing
 * into the code it just produced.
 */
TEST_F (LazyThunks, ArgumentsSurviveTheCompileTheyTriggered)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto thunk = engine->publish_lazy (
		"m", [] () -> Expected<void *> { return (void *) &lazy_many_args; });
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

	int64_t expected = call_many_args (&lazy_many_args);

	/* Through the trampoline ... */
	EXPECT_EQ (call_many_args (reinterpret_cast<ManyArgsFn> (*thunk)), expected);
	/* ... and again now that the thunk points straight at the code. */
	EXPECT_EQ (call_many_args (reinterpret_cast<ManyArgsFn> (*thunk)), expected);
}

TEST_F (LazyThunks, CallersCanBeCompiledBeforeTheCodeExists)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	int compiles = 0;
	ASSERT_TRUE (
		bool (engine->publish_lazy ("m", [&] () -> Expected<void *> {
			compiles++;
			auto compiled = engine->jit ().compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
		})));

	/* Resolves against the thunk's own address - not the trampoline's,
	 * though that is all it points at right now - the same address a caller
	 * would keep once the lazy compile fires and redirects it. */
	auto caller = engine->compile_against (build_caller_module ("m"), "caller", { "m" });
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (compiles, 0);

	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 7);
	EXPECT_EQ (compiles, 1);
}

/*
 * Racing first calls do not compile only once, and must not: nothing of the
 * callback's is held across the compile, so threads that arrive together each
 * run it. What they owe the caller is one answer, not one compile.
 */
TEST_F (LazyThunks, RacingFirstCallsAgreeOnOneAnswer)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	std::atomic<int> compiles {0};
	auto thunk = engine->publish_lazy ("m", [&] () -> Expected<void *> {
		compiles++;
		auto compiled = engine->jit ().compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
	});
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

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
			results[i] = reinterpret_cast<IntFn> (*thunk) ();
		});

	while (ready.load () != threads)
		std::this_thread::yield ();
	go.store (true);
	for (std::thread &t : workers)
		t.join ();

	EXPECT_GE (compiles.load (), 1);
	EXPECT_LE (compiles.load (), threads);
	for (int i = 0; i < threads; i++)
		EXPECT_EQ (results[i], 7) << "thread " << i;
}

/*
 * A compile takes the runtime's locks - the loader lock, for a corlib class
 * an emitted null check names - and a thread that already holds one of those
 * can enter a lazy thunk. If anything of the callback's were held across
 * compile (), those two threads would close a cycle and neither would move
 * again. The recursive mutex below stands for the loader lock, which is
 * recursive too.
 *
 * A regression does not fail this test, it stops it finishing, because that
 * is what the defect does. CTest reports it as a timeout naming this case.
 */
TEST_F (LazyThunks, AThreadHoldingALockTheCompileNeedsIsNotBlocked)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	std::recursive_mutex runtime_lock;
	std::atomic<bool> compiling {false};
	std::atomic<bool> holder_has_lock {false};

	auto thunk = engine->publish_lazy ("m", [&] () -> Expected<void *> {
		compiling = true;

		/* Give the other thread the lock and the thunk, in that order. */
		while (!holder_has_lock.load ())
			std::this_thread::yield ();
		std::this_thread::sleep_for (std::chrono::milliseconds (50));

		std::lock_guard<std::recursive_mutex> lock (runtime_lock);
		auto compiled = engine->jit ().compile (build_constant_module (7), "constant");
		if (!compiled)
			return compiled.takeError ();
		return compiled->entry;
	});
	ASSERT_TRUE (bool (thunk)) << toString (thunk.takeError ());

	std::thread first ([&] { reinterpret_cast<IntFn> (*thunk) (); });

	while (!compiling.load ())
		std::this_thread::yield ();

	int32_t held = 0;
	std::thread holder ([&] {
		std::lock_guard<std::recursive_mutex> lock (runtime_lock);
		holder_has_lock = true;
		held = reinterpret_cast<IntFn> (*thunk) ();
	});

	first.join ();
	holder.join ();
	EXPECT_EQ (held, 7);
}

} // namespace
} // namespace test
} // namespace mono
