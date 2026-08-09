/*
 * Tests for the redirectable stubs a method is published as.
 *
 * Three things have to hold: a call through a stub reaches whatever the stub
 * currently points at; compiled code binds to the stub rather than to the
 * target it happened to have at link time; and a runtime detour written over a
 * stub - what Harmony does to every method it patches - keeps working across a
 * redirect, and lands on the newest target once it is removed.
 *
 * The stubs belong to the backend and the names belong to MonoJit, so the two
 * are driven together through the Engine below, which is what the backend does
 * around a method minus the method.
 */

#include "arch/arch.hpp"
#include "callbacks.hpp"
#include "codemem.hpp"
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

/// Where a lazy stub lands when the compile behind it failed, which no case here
/// arranges for.
[[noreturn]] static void
lazy_failed ()
{
	ADD_FAILURE () << "a lazy stub's compile failed";
	std::abort ();
}

/*
 * A domain's engine: one set of slabs, the linker over them, the stubs carved
 * out of them and the trampolines an uncompiled one points at.
 *
 * Publishing is two steps that always go together - carve the stub, and define
 * the name at it - and undefining is those two in reverse. That pairing is the
 * backend's, so it is reproduced here rather than reached for through MonoJit,
 * which now only ever hears the names.
 */
class Engine {
public:
	static Expected<std::unique_ptr<Engine>> create ()
	{
		auto self = std::unique_ptr<Engine> (new Engine ());
		auto jit = MonoJit::create (self->slabs_);

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

	/// Publish NAME as a stub jumping to TARGET and return the address callers
	/// reach it at.
	Expected<void *> publish (StringRef name, void *target)
	{
		Expected<void *> code = carve (name);

		if (!code)
			return code;
		stubs_.find (name)->redirect (target);
		return code;
	}

	/// Publish NAME as a stub that runs COMPILE on its first call and goes
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

				stubs_.find (owned)->redirect (*code);
				return *code;
			});
		if (!trampoline)
			return trampoline.takeError ();

		Expected<void *> code = carve (name);

		if (!code) {
			callbacks_->release (*trampoline);
			return code;
		}

		stubs_.find (name)->redirect (*trampoline);
		trampolines_[name] = *trampoline;
		return code;
	}

	Error redirect (StringRef name, void *target)
	{
		std::optional<Stub> stub = stubs_.find (name);

		if (!stub)
			return createStringError (inconvertibleErrorCode (),
			                          "no stub was published for %s",
			                          name.str ().c_str ());
		stub->redirect (target);
		return Error::success ();
	}

	Expected<void *> address (StringRef name)
	{
		std::optional<Stub> stub = stubs_.find (name);

		if (!stub)
			return createStringError (inconvertibleErrorCode (),
			                          "no stub was published for %s",
			                          name.str ().c_str ());
		return stub->code ();
	}

	Error undefine (ArrayRef<std::string> names)
	{
		if (Error err = jit_->undefine_stubs (names))
			return err;

		for (const std::string &name : names) {
			auto it = trampolines_.find (name);

			if (it == trampolines_.end ())
				continue;
			callbacks_->release (it->second);
			trampolines_.erase (it);
		}

		stubs_.remove_all (names);
		return Error::success ();
	}

private:
	Engine () : slabs_ (std::make_shared<CodeSlabs> ()), stubs_ (slabs_.get ()) {}

	/// Carve NAME a stub and tell the linker about it, leaving it pointing
	/// nowhere useful for the caller to set.
	Expected<void *> carve (StringRef name)
	{
		Expected<Stub> stub = stubs_.create (name);

		if (!stub)
			return stub.takeError ();

		std::pair<StringRef, void *> def{name, stub->code ()};

		if (Error err = jit_->define_stubs (def))
			return std::move (err);
		return stub->code ();
	}

	std::shared_ptr<CodeSlabs> slabs_;
	std::unique_ptr<MonoJit> jit_;
	StubTable stubs_;
	std::unique_ptr<LazyCallbacks> callbacks_;
	StringMap<void *> trampolines_;
};

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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto stub = engine->publish ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 1);

	ASSERT_FALSE (bool (engine->redirect ("m", (void *) &stub_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 2);

	/* The published address never moves - that is the whole point of it. */
	auto again = engine->address ("m");
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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto first = engine->publish ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*first) (), 1);

	ASSERT_FALSE (bool (engine->undefine ({ "m" })));

	auto second = engine->publish ("m", (void *) &stub_target_two);
	ASSERT_TRUE (bool (second)) << toString (second.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (*second) (), 2);
}

/*
 * The common case for a method that was published and then freed without ever
 * being called or linked against. Its symbol is there like any other - a stub
 * reaches the linker when it is carved - but nothing has ever looked it up, so
 * it is still unmaterialized, which is the state undefining has to accept
 * without first forcing it into existence.
 */
TEST (Stubs, AnUnmaterializedStubCanBeUndefined)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto first = engine->publish ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());
	ASSERT_FALSE (bool (engine->undefine ({ "m" })));

	auto again = engine->publish ("m", (void *) &stub_target_two);
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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto first = engine->publish ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());

	ASSERT_FALSE (bool (engine->undefine ({ "m" })));

	/* A different name, so this is the block being reused and not the name. */
	auto second = engine->publish ("n", (void *) &stub_target_two);
	ASSERT_TRUE (bool (second)) << toString (second.takeError ());

	EXPECT_EQ (*first, *second);
	EXPECT_EQ (reinterpret_cast<IntFn> (*second) (), 2);
}

/*
 * The other half of the case above: a module was compiled against the name, so
 * its symbol has settled rather than never having been searched for. The name
 * has to come back clean enough for the next method along to be linked against.
 */
TEST (Stubs, ANameAModuleLinkedAgainstCanBePublishedAgain)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	ASSERT_TRUE (bool (engine->publish ("m", (void *) &stub_target_one)));

	auto caller = engine->jit ().compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	ASSERT_FALSE (bool (engine->undefine ({ "m" })));
	ASSERT_TRUE (bool (engine->publish ("m", (void *) &stub_target_two)));

	/* The old caller is calling a stub that has been handed out again. */
	auto again = engine->jit ().compile (build_caller_module ("m"), "caller");
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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	Error err = engine->undefine ({ "m" });

	ASSERT_TRUE (bool (err));
	consumeError (std::move (err));
}

TEST (Stubs, CompiledCallersBindToTheStub)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	ASSERT_TRUE (bool (engine->publish ("m", (void *) &stub_target_one)));

	auto caller = engine->jit ().compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	/*
	 * The caller was compiled and linked while the stub pointed at target one.
	 * If it had bound to that address, this would still return 1 - a promotion
	 * would be invisible to everything already compiled.
	 */
	ASSERT_FALSE (bool (engine->redirect ("m", (void *) &stub_target_two)));
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2);
}

TEST (Stubs, GeometryLeavesRoomForADetour)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto a = engine->publish ("a", (void *) &stub_target_one);
	auto b = engine->publish ("b", (void *) &stub_target_two);
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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto stub = engine->publish ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	auto caller = engine->jit ().compile (build_caller_module ("m"), "caller");
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
		bool (engine->redirect ("m", (void *) &stub_target_three)));
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 99);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 99);

	/* Unpatching restores the jump, which lands on the newest tier. */
	std::memcpy (*stub, saved, detour_size);
	EXPECT_EQ (reinterpret_cast<IntFn> (*stub) (), 3);
	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 3);
}

/*
 * Promotion is a redirect performed under running code: the slot is rewritten
 * while other threads are calling through the stub and while compiled callers
 * hold its address. Every call has to come back with one of the targets the
 * stub has actually had - a torn slot would return something else, or jump into
 * nothing - and calls made after a redirect have to see it, so the redirects
 * are not simply lost.
 */
TEST (Stubs, RedirectsWhileThreadsRunThroughIt)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto stub = engine->publish ("m", (void *) &stub_target_one);
	ASSERT_TRUE (bool (stub)) << toString (stub.takeError ());

	auto caller = engine->jit ().compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

	void *const targets[] = { (void *) &stub_target_one,
		                      (void *) &stub_target_two,
		                      (void *) &stub_target_three };

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
			IntFn direct = reinterpret_cast<IntFn> (*stub);
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
			<< "reader " << i << " reached a target the stub never had";
		EXPECT_EQ (seen[i].load (), all_three)
			<< "reader " << i << " never saw all three targets";
	}
}

/*
 * One stub per method means a Unity-sized game publishes six figures of them,
 * so what a stub costs in address space is a real number rather than an
 * aesthetic one. Stubs published one at a time still have to pack: the version
 * of this that built a LinkGraph per stub spent two pages on each.
 */
TEST (Stubs, PackTightlyWhenPublishedOneAtATime)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	constexpr int count = 512;
	std::vector<uintptr_t> addrs;
	for (int i = 0; i < count; i++) {
		auto stub = engine->publish ("m" + std::to_string (i),
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
 * Several links wanting the same stub at once. They all reach one symbol, and
 * the first of them to get there materializes it while the rest wait; a link
 * that came away with anything other than the stub's address would be calling
 * the target the stub happened to have rather than the stub.
 */
TEST (Stubs, ConcurrentLinksAgainstOneStubDefineItOnce)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	ASSERT_TRUE (bool (engine->publish ("m", (void *) &stub_target_one)));

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

			auto caller = engine->jit ().compile (build_caller_module ("m"), "caller");
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
 * Undefining a name while a link is asking mono.stubs for it. A symbol a link
 * has just been handed is materializing, and ORC refuses to remove one of those
 * - so an undefine landing in that window has to wait for the link rather than
 * come away thinking the name is gone and hand the block to the next method
 * along.
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

	auto publish_all = [&] (Engine &engine, void *target) {
		for (const std::string &name : names)
			if (Error err = engine.publish (name, target).takeError ())
				return toString (std::move (err));
		return std::string ();
	};

	int64_t compile_us = 0;
	{
		std::unique_ptr<Engine> engine = make_engine ();
		ASSERT_NE (engine, nullptr);
		ASSERT_EQ (publish_all (*engine, (void *) &stub_target_one), "");

		auto start = std::chrono::steady_clock::now ();
		auto compiled = engine->jit ().compile (build_table_module (names), "entry");
		ASSERT_TRUE (bool (compiled)) << toString (compiled.takeError ());
		compile_us = std::chrono::duration_cast<std::chrono::microseconds> (
			             std::chrono::steady_clock::now () - start)
		                     .count ();
	}

	for (int round = 0; round < rounds; round++) {
		std::unique_ptr<Engine> engine = make_engine ();
		ASSERT_NE (engine, nullptr);
		ASSERT_EQ (publish_all (*engine, (void *) &stub_target_one), "");

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
				engine->jit ().compile (build_table_module (names), "entry").takeError ());
		});
		std::thread undefiner ([&] {
			ready++;
			while (!go.load ())
				std::this_thread::yield ();
			std::this_thread::sleep_for (delay);
			if (Error err = engine->undefine (names))
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
		ASSERT_EQ (publish_all (*engine, (void *) &stub_target_two), "")
			<< "round " << round;

		auto caller = engine->jit ().compile (build_caller_module ("s0"), "caller");
		ASSERT_TRUE (bool (caller))
			<< "round " << round << ": " << toString (caller.takeError ());
		ASSERT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2)
			<< "round " << round;
	}
}

/*
 * Publishing takes the session lock to define a name, linking takes it to look
 * one up, and undefining takes it to remove one - all against the same dylib.
 * Threads doing all four at once are what says those orders agree; a
 * disagreement hangs rather than fails, so this test failing means it timed out.
 */
TEST (Stubs, PublishRedirectAndUndefineFromManyThreads)
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

			/* Each thread owns its names, so the contention is all in ORC. */
			for (int n = 0; n < iterations; n++) {
				std::string name =
					"t" + std::to_string (i) + "." + std::to_string (n);

				Expected<void *> stub =
					engine->publish (name, (void *) &stub_target_one);

				if (!stub) {
					fail (stub.takeError ());
					return;
				}

				auto caller = engine->jit ().compile (
					build_caller_module (name.c_str ()), "caller");
				if (!caller) {
					fail (caller.takeError ());
					return;
				}
				EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 1);

				if (Error err = engine->redirect (
				        name, (void *) &stub_target_two)) {
					fail (std::move (err));
					return;
				}
				EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 2);

				if (Error err = engine->undefine ({ name })) {
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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	int compiles = 0;
	auto stub = engine->publish_lazy ("m", [&] () -> Expected<void *> {
		compiles++;
		auto compiled = engine->jit ().compile (build_constant_module (7), "constant");
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
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	auto stub = engine->publish_lazy (
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

	/* Resolves against a method that has no code yet. */
	auto caller = engine->jit ().compile (build_caller_module ("m"), "caller");
	ASSERT_TRUE (bool (caller)) << toString (caller.takeError ());
	EXPECT_EQ (compiles, 0);

	EXPECT_EQ (reinterpret_cast<IntFn> (caller->entry) (), 7);
	EXPECT_EQ (compiles, 1);
}

TEST (LazyStubs, RacingFirstCallsCompileOnce)
{
	std::unique_ptr<Engine> engine = make_engine ();
	ASSERT_NE (engine, nullptr);

	std::atomic<int> compiles {0};
	auto stub = engine->publish_lazy ("m", [&] () -> Expected<void *> {
		compiles++;
		auto compiled = engine->jit ().compile (build_constant_module (7), "constant");
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
