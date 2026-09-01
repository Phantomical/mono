/*
 * test-detour.cpp: Unit test for handing a method's entry address to native
 * code.
 *
 * A native code patcher - Harmony, MonoMod, and the Unity mod code built on
 * them - replaces a method by taking the address it is entered at and making
 * calls arrive somewhere else. mono_install_method_detour () is the way to say
 * so: the entry is redirected at the target, the method never promotes again,
 * and a compile already running for it does not take the entry when it lands.
 *
 * A detour reaches a caller only where the caller goes through the entry. A
 * compiled caller always does. An interpreted one does when it makes a jit call
 * to the entry, which is what resolve_code_type () settles, and does not when
 * the interpreter has copied the callee's body into it.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/metadata-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/jit.h"
#include "mini/mini.h"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "detour.exe"

/* What every detour in this file is pointed at, and what tells it apart from
 * the method's own body: Target () and Inlined () both answer x + 1. */
extern "C" int
detoured_body (int x)
{
	return x + 1000;
}

/* The same for an instance method, whose entry is handed the receiver first.
 * A detour target carries the method's own convention: nothing adapts one. */
extern "C" int
detoured_instance_body (void *self, int x)
{
	(void) self;
	return x + 1000;
}

/* Set to a compiled method's entry before the detour below is installed. */
void (*g_call_through_compiled) () = nullptr;

/*
 * A plain function rather than a lambda: mono_install_method_detour () takes
 * a bare function pointer, with nothing to close over the target with.
 */
extern "C" void
detoured_body_calls_managed ()
{
	g_call_through_compiled ();
}

MonoImage *g_image;

class MethodDetour : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		if (g_image != nullptr)
			return;

		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT,
			mono_domain_default_alc (mono_domain_get ()));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (TESTPROG, &req, &status);

		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;
		g_image = mono_assembly_get_image_internal (assembly);
	}

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		/*
		 * With tier 0 off every method is compiled, and the interpreted
		 * arms below then check nothing. Say so as a skip.
		 */
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
	}

protected:
	/* Each case detours a method of its own, since a detour is never undone. */
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass =
			mono_class_from_name_checked (g_image, "", "Detour", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/* The one instantiation over string of a one-type-parameter method. It is
	 * the same MonoMethod the caller's own token resolves to, since inflating
	 * is cached. */
	static MonoMethod *instantiated_over_string (MonoMethod *definition)
	{
		ERROR_DECL (error);
		MonoType *arguments[1] = { m_class_get_byval_arg (mono_get_string_class ()) };
		MonoGenericContext context;

		memset (&context, 0, sizeof (context));
		context.method_inst = mono_metadata_get_generic_inst (1, arguments);

		MonoMethod *inflated =
			mono_class_inflate_generic_method_checked (definition, &context, error);

		mono_error_assert_ok (error);
		return inflated;
	}

	/* A method of one reference instantiation of a generic class. */
	static MonoMethod *class_method_over (const char *type, const char *name, int argc,
	                                      MonoClass *argument)
	{
		ERROR_DECL (error);
		MonoClass *definition = mono_class_from_name_checked (g_image, "", type, error);

		mono_error_assert_ok (error);
		if (definition == nullptr)
			return nullptr;

		MonoType *arguments[1] = { m_class_get_byval_arg (argument) };
		MonoGenericContext context;

		memset (&context, 0, sizeof (context));
		context.class_inst = mono_metadata_get_generic_inst (1, arguments);

		MonoClass *inflated =
			mono_class_inflate_generic_class_checked (definition, &context, error);

		mono_error_assert_ok (error);
		if (inflated == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (inflated, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/*
	 * Where \p method is entered once a promotion has given it a body, or null
	 * when none landed.
	 *
	 * Through the queue rather than through PromoteNow. The queue is how a
	 * method that is merely called gets compiled, so it is the route these
	 * cases have to be taken on; PromoteNow reaches the backend by another
	 * door and would answer for that one instead.
	 */
	static void *promoted_body (MonoMethod *method)
	{
		ERROR_DECL (error);
		MonoDomain *domain = mono_domain_get ();

		/* The record a promotion is decided on, which nothing has built while
		 * the method has not been asked for. */
		mono_compile_method_checked (method, error);
		mono_error_assert_ok (error);

		if (!mono_promote_method (method, domain))
			return nullptr;

		return await_body (domain, method);
	}

	/*
	 * Whether the body serving \p method is the shared form's rather than one
	 * compiled against this instantiation.
	 *
	 * The addresses do not answer this on their own. An instantiation whose
	 * shared body reads its context out of a register is entered at a stub of
	 * its own, so two that share are entered at different addresses; and the
	 * printed names cannot be compared either, since the shared form and the
	 * genuine <object> instantiation print the same.
	 */
	static bool is_served_by_a_shared_body (MonoMethod *method)
	{
		ERROR_DECL (error);
		MonoMethod *shared = mini_get_shared_method_full (method, SHARE_MODE_NONE, error);

		mono_error_cleanup (error);
		if (shared == nullptr || shared == method)
			return false;

		return mono_llvm_jit_find_body (mono_domain_get (), shared) != nullptr;
	}

	/* What a managed caller answers, run through an ordinary invoke. */
	static int invoke (MonoMethod *method, int argument)
	{
		ERROR_DECL (error);
		void *args[1] = { &argument };
		MonoObject *result = mono_runtime_invoke_checked (method, nullptr, args, error);

		mono_error_assert_ok (error);
		EXPECT_NE (nullptr, result);
		return result != nullptr ? *(int *) mono_object_unbox_internal (result) : 0;
	}

	/* The string a no-argument managed method answers. */
	static std::string invoke_string (MonoMethod *method)
	{
		ERROR_DECL (error);
		MonoObject *result = mono_runtime_invoke_checked (method, nullptr, nullptr, error);

		mono_error_assert_ok (error);
		EXPECT_NE (nullptr, result);
		if (result == nullptr)
			return {};

		char *text = mono_string_to_utf8_checked_internal ((MonoString *) result, error);

		mono_error_assert_ok (error);

		std::string answer (text != nullptr ? text : "");

		g_free (text);
		return answer;
	}

	/*
	 * Waits up to a second for a promotion to land, and answers the body or
	 * null. A promotion that is taken lands in a few milliseconds, so this
	 * is generous for proving one did not happen.
	 */
	static void *await_body (MonoDomain *domain, MonoMethod *method)
	{
		for (int waited = 0; waited < 1000; ++waited) {
			if (void *body = mono_llvm_jit_find_body (domain, method))
				return body;
			g_usleep (1000);
		}
		return nullptr;
	}
};

} // namespace

/*
 * The entry address reaches the detour. This is the compiled caller's case:
 * a compiled call site names the stub, which is the address handed out here.
 */
TEST_F (MethodDetour, TakesTheEntryAddress)
{
	ERROR_DECL (error);
	MonoMethod *method = method_named ("Target", 1);

	ASSERT_NE (nullptr, method);

	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, entry);

	mono_install_method_detour (method, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, entry (1));
	/* The same address as before: a patcher's cached pointer still works. */
	EXPECT_EQ ((void *) entry, mono_compile_method_checked (method, error));
	mono_error_assert_ok (error);
}

/*
 * A detoured method does not promote. A body compiled after the detour would
 * take the entry back off the patcher, and the entry is what every caller has.
 */
TEST_F (MethodDetour, OutranksAPromotion)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = method_named ("Inlined", 1);

	ASSERT_NE (nullptr, method);
	ASSERT_GT (mono_llvm_jit_tier0_calls (method), 0)
		<< "this method no longer starts at tier 0, so it checks nothing";

	mono_install_method_detour (method, domain, (void *) detoured_body);

	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	ASSERT_EQ (1001, entry (1)) << "the detour was never installed";

	/*
	 * True because there is nothing left for the counting engine to do, not
	 * because a request went out. Nothing must reach the compile queue.
	 */
	EXPECT_TRUE (mono_promote_method (method, domain));
	EXPECT_EQ (nullptr, await_body (domain, method));
}

/*
 * An interpreted caller reaches the detour, because resolve_code_type () sees
 * the entry is native and makes a jit call to it rather than interpreting the
 * callee.
 */
TEST_F (MethodDetour, IsSeenByAnInterpretedCaller)
{
	MonoMethod *target = method_named ("Target", 1);
	MonoMethod *caller = method_named ("CallTarget", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (target), 0)
		<< "the callee already has code, so this checks a compiled call";

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, invoke (caller, 1));
}

/*
 * An interpreted caller that already reached Target once, while it had no
 * code, still finds a detour installed afterward. resolve_code_type ()
 * latches this call site onto interpreting Target on the first call.
 * install_detour ()'s own notification is what overrides that latch (#277).
 */
TEST_F (MethodDetour, IsSeenAfterAnInterpretedCallLatchedFirst)
{
	MonoMethod *target = method_named ("LateTarget", 1);
	MonoMethod *caller = method_named ("CallLateTarget", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (target), 0)
		<< "the callee no longer starts at tier 0, so this checks nothing";
	ASSERT_EQ (nullptr, mono_llvm_jit_find_body (mono_domain_get (), target))
		<< "the callee already has code, so this call site never latches onto interpreting it";

	EXPECT_EQ (2, invoke (caller, 1));

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, invoke (caller, 1));
}

/*
 * So does an interpreted caller of a generic method. An instantiation is
 * entered exactly like any other method - its entry supplies whatever context
 * the body behind it reads - so the interpreter has the same two choices for it
 * and takes the same one.
 *
 * The detour is the instrument rather than the subject here. It is what makes
 * the answer say which choice was taken, and it needs no promotion to land
 * first, so this arm is not racing a background compile.
 */
TEST_F (MethodDetour, IsSeenByAnInterpretedCallerOfAnInstantiation)
{
	MonoMethod *definition = method_named ("GenericTarget", 1);
	MonoMethod *caller = method_named ("CallGenericTarget", 1);

	ASSERT_NE (nullptr, definition);
	ASSERT_NE (nullptr, caller);

	MonoMethod *target = instantiated_over_string (definition);

	ASSERT_NE (nullptr, target);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (target), 0)
		<< "the callee already has code, so this checks a compiled call";

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, invoke (caller, 1));
}

/*
 * Two reference instantiations of one method end up entering the same code.
 *
 * Every case below rests on this. A detour that is per instantiation is only
 * worth asserting where one body serves several of them, and a run where
 * sharing never happened would pass those cases while checking nothing.
 */
TEST_F (MethodDetour, TwoInstantiationsShareOneBody)
{
	MonoMethod *over_string =
		class_method_over ("Shared`1", "Read", 1, mono_get_string_class ());
	MonoMethod *over_object =
		class_method_over ("Shared`1", "Read", 1, mono_get_object_class ());

	ASSERT_NE (nullptr, over_string);
	ASSERT_NE (nullptr, over_object);
	ASSERT_NE (over_string, over_object) << "these are the same method";

	void *body = promoted_body (over_string);
	void *also = promoted_body (over_object);

	ASSERT_NE (nullptr, body);
	ASSERT_NE (nullptr, also);
	EXPECT_EQ (body, also) << "each instantiation was compiled against itself";
}

/*
 * An instantiation whose shared body has no receiver still answers about its
 * own type argument. The context comes out of a register that the stub in
 * front of this instantiation writes, so a stub that wrote another one's
 * context is a wrong answer here rather than a crash.
 */
TEST_F (MethodDetour, AStaticInstantiationKeepsItsOwnContext)
{
	MonoMethod *over_string =
		class_method_over ("SharedStatic`1", "Name", 0, mono_get_string_class ());
	MonoMethod *over_object =
		class_method_over ("SharedStatic`1", "Name", 0, mono_get_object_class ());

	ASSERT_NE (nullptr, over_string);
	ASSERT_NE (nullptr, over_object);

	ASSERT_NE (nullptr, promoted_body (over_string));
	ASSERT_NE (nullptr, promoted_body (over_object));
	ASSERT_TRUE (is_served_by_a_shared_body (over_string))
		<< "this instantiation was compiled against itself, so no stub was written";
	ASSERT_TRUE (is_served_by_a_shared_body (over_object))
		<< "this instantiation was compiled against itself, so no stub was written";

	EXPECT_EQ ("String", invoke_string (over_string));
	EXPECT_EQ ("Object", invoke_string (over_object));
}

/*
 * A detour is per instantiation however thoroughly the instantiations share.
 * Sharing binds a thunk's target and never the thunk, so each instantiation
 * keeps an entry of its own for a patcher to take.
 */
TEST_F (MethodDetour, ADetourOnOneInstantiationLeavesTheOthers)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *detoured =
		class_method_over ("Shared`1", "Read", 1, mono_get_exception_class ());
	MonoMethod *untouched =
		class_method_over ("Shared`1", "Read", 1, mono_get_string_class ());

	ASSERT_NE (nullptr, detoured);
	ASSERT_NE (nullptr, untouched);

	void *body = promoted_body (detoured);

	ASSERT_NE (nullptr, body);
	ASSERT_EQ (body, promoted_body (untouched)) << "the two do not share a body";

	mono_install_method_detour (detoured, domain, (void *) detoured_instance_body);

	/* The receiver is not read, so the entries take one that is never
	 * dereferenced. */
	int (*moved) (void *, int) =
		(int (*) (void *, int)) mono_compile_method_checked (detoured, error);
	mono_error_assert_ok (error);
	int (*stayed) (void *, int) =
		(int (*) (void *, int)) mono_compile_method_checked (untouched, error);
	mono_error_assert_ok (error);

	ASSERT_NE (nullptr, moved);
	ASSERT_NE (nullptr, stayed);
	ASSERT_NE ((void *) moved, (void *) stayed) << "one thunk serves both";

	EXPECT_EQ (1001, moved (nullptr, 1));
	EXPECT_EQ (2, stayed (nullptr, 1)) << "the detour followed the shared body";
}

/*
 * And an interpreted caller sees the same split. This is the arm the
 * notification in enter_shared_body () governs: an instantiation bound to a
 * shared body has an entry, and nothing else tells the interpreter so.
 */
TEST_F (MethodDetour, ADetourOnOneInstantiationLeavesTheOthersForAnInterpretedCaller)
{
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *detoured =
		class_method_over ("Shared`1", "Read", 1, mono_get_exception_class ());
	MonoMethod *untouched =
		class_method_over ("Shared`1", "Read", 1, mono_get_object_class ());
	MonoMethod *calls_detoured = method_named ("CallSharedOverException", 1);
	MonoMethod *calls_untouched = method_named ("CallSharedOverObject", 1);

	ASSERT_NE (nullptr, detoured);
	ASSERT_NE (nullptr, untouched);
	ASSERT_NE (nullptr, calls_detoured);
	ASSERT_NE (nullptr, calls_untouched);
	ASSERT_GT (mono_llvm_jit_tier0_calls (calls_detoured), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (calls_untouched), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";

	void *body = promoted_body (detoured);

	ASSERT_NE (nullptr, body);
	ASSERT_EQ (body, promoted_body (untouched)) << "the two do not share a body";

	mono_install_method_detour (detoured, domain, (void *) detoured_instance_body);

	/* The first of these needs the interpreter to jit-call an instantiation at
	 * all, which interp_jit_call_refusal () decides. The second is the split
	 * itself, and holds whichever way the caller reaches its callee. */
	EXPECT_EQ (1001, invoke (calls_detoured, 1));
	EXPECT_EQ (2, invoke (calls_untouched, 1)) << "the detour followed the shared body";
}

/*
 * A callee the interpreter copied into its caller does not. The copy is not
 * reached through the entry, and nothing rewrites a body already transformed.
 *
 * This is the documented limitation, asserted so that putting the record's stub
 * on the interpreted call path has a case that flips.
 */
TEST_F (MethodDetour, IsMissedByAnInlinedCallee)
{
	ERROR_DECL (error);
	MonoMethod *target = method_named ("Inlined", 1);
	MonoMethod *caller = method_named ("CallInlined", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	/* The detour is there. What follows is about the path, not the install. */
	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (target, error);
	mono_error_assert_ok (error);
	ASSERT_EQ (1001, entry (1));

	EXPECT_EQ (2, invoke (caller, 1));
}

/*
 * A detour target is native code with no MonoJitInfo and no LMF of its own -
 * unlike a P/Invoke or an icall, which both go through a wrapper that pushes
 * one. The chain: an interpreted caller reaches a detour through a jit call,
 * the detour calls into a compiled method with no wrapper in between, and
 * that method calls back into the interpreter, which throws.
 */
TEST_F (MethodDetour, DetourCallsBackIntoManagedAndThrows)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *detoured = method_named ("Detoured", 0);
	MonoMethod *caller = method_named ("CallDetoured", 0);
	MonoMethod *through_compiled = method_named ("ThroughCompiled", 0);
	MonoMethod *deep_throw = method_named ("DeepThrow", 0);

	ASSERT_NE (nullptr, detoured);
	ASSERT_NE (nullptr, caller);
	ASSERT_NE (nullptr, through_compiled);
	ASSERT_NE (nullptr, deep_throw);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (deep_throw), 0)
		<< "the throw site no longer starts at tier 0, so this checks nothing";

	g_call_through_compiled =
		(void (*) ()) mono_compile_method_checked (through_compiled, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, g_call_through_compiled);

	mono_install_method_detour (detoured, domain, (void *) detoured_body_calls_managed);

	mono_runtime_invoke_checked (caller, nullptr, nullptr, error);

	ASSERT_FALSE (is_ok (error))
		<< "the throw from beneath the detour never reached the invoke";
	MonoException *exc = mono_error_convert_to_exception (error);
	ASSERT_NE (nullptr, exc);
	EXPECT_STREQ ("InvalidOperationException",
	             m_class_get_name (mono_object_class ((MonoObject *) exc)));
}
