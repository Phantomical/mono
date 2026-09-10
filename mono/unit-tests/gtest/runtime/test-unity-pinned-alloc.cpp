/*
 * test-unity-pinned-alloc.cpp: Unit test for routing a UnityEngine.Object
 * subclass's allocation to the major heap's pinned space.
 *
 * A UnityEngine.Object-derived class is a positive control for that routing.
 * It fails if alloc_pinned stops reaching mono_gc_alloc_obj () or
 * mono_gc_get_managed_allocator (), the two places that decide it.
 */

#include "config.h"

#define HAVE_SGEN_GC

#include "metadata/class-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/domain-method.hpp"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <mono/sgen/sgen-gc.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "unity-pinned-alloc.exe"

class UnityPinnedAlloc : public ::testing::Test {
protected:
	MonoImage *image = nullptr;

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		MonoDomain *domain = mono_domain_get ();
		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT, mono_domain_default_alc (domain));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (TESTPROG, &req, &status);
		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;

		image = mono_assembly_get_image_internal (assembly);
	}

	MonoClass *class_named (const char *name_space, const char *name)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (image, name_space, name, error);
		mono_error_assert_ok (error);
		return klass;
	}

	MonoMethod *method_named (const char *host_type, const char *method_name)
	{
		ERROR_DECL (error);
		MonoClass *host = class_named ("", host_type);
		EXPECT_NE (nullptr, host);
		if (host == nullptr)
			return nullptr;

		MonoMethod *method =
			mono_class_get_method_from_name_checked (host, method_name, 0, 0, error);
		mono_error_assert_ok (error);
		EXPECT_NE (nullptr, method);
		return method;
	}

	MonoObject *invoke_no_args (const char *host_type, const char *method_name)
	{
		ERROR_DECL (error);
		MonoMethod *method = method_named (host_type, method_name);
		if (method == nullptr)
			return nullptr;

		MonoObject *result = mono_runtime_invoke_checked (method, nullptr, nullptr, error);
		mono_error_assert_ok (error);
		return result;
	}
};

TEST_F (UnityPinnedAlloc, ClassBitFollowsHierarchy)
{
	MonoClass *derived = class_named ("", "Derived");
	MonoClass *grandchild = class_named ("", "GrandChild");
	MonoClass *control = class_named ("", "Control");
	MonoClass *through_generic = class_named ("", "ThroughGeneric");

	ASSERT_NE (nullptr, derived);
	ASSERT_NE (nullptr, grandchild);
	ASSERT_NE (nullptr, control);
	ASSERT_NE (nullptr, through_generic);

	EXPECT_TRUE (m_class_alloc_pinned (derived));
	EXPECT_TRUE (m_class_alloc_pinned (grandchild));
	EXPECT_FALSE (m_class_alloc_pinned (control));
	EXPECT_TRUE (m_class_alloc_pinned (through_generic));
}

/*
 * class_named () resolves GenericDerived to its open generic type
 * definition, not to the GenericDerived<int> instantiation ginst inheritance
 * needs. This reaches that instantiation the way the runtime does, through
 * mono_object_class () on an instance MakeGenericInstance () allocated.
 */
TEST_F (UnityPinnedAlloc, GenericInstanceClassBitFollowsHierarchy)
{
	MonoObject *instance = invoke_no_args ("UnityPinnedAllocHost", "MakeGenericInstance");
	ASSERT_NE (nullptr, instance);
	EXPECT_TRUE (m_class_alloc_pinned (mono_object_class (instance)));
}

TEST_F (UnityPinnedAlloc, NativeAllocationIsPinned)
{
	MonoDomain *domain = mono_domain_get ();
	ERROR_DECL (error);

	MonoObject *derived = mono_object_new_checked (domain, class_named ("", "Derived"), error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, derived);
	EXPECT_FALSE (sgen_ptr_in_nursery ((char *) derived));

	MonoObject *through_generic =
		mono_object_new_checked (domain, class_named ("", "ThroughGeneric"), error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, through_generic);
	EXPECT_FALSE (sgen_ptr_in_nursery ((char *) through_generic));

	MonoObject *control = mono_object_new_checked (domain, class_named ("", "Control"), error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, control);
	EXPECT_TRUE (sgen_ptr_in_nursery ((char *) control));
}

TEST_F (UnityPinnedAlloc, ClassicTierAllocationIsPinned)
{
	MonoObject *derived = invoke_no_args ("UnityPinnedAllocHost", "MakeDerived");
	ASSERT_NE (nullptr, derived);
	EXPECT_FALSE (sgen_ptr_in_nursery ((char *) derived));

	MonoObject *control = invoke_no_args ("UnityPinnedAllocHost", "MakeControl");
	ASSERT_NE (nullptr, control);
	EXPECT_TRUE (sgen_ptr_in_nursery ((char *) control));
}

/*
 * mono_gc_get_managed_allocator ()'s decline reaches the classic compiler
 * and the LLVM backend through code of their own. ClassicTierAllocationIsPinned
 * above never compiles this newobj site through
 * mono/llvm/method-to-llvm/boxing.cpp, so this case promotes each method to
 * tier 1 with mono_llvm_jit_promote_now () before ever calling it.
 */
TEST_F (UnityPinnedAlloc, LlvmTierAllocationIsPinned)
{
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *make_derived = method_named ("UnityPinnedAllocHost", "MakeDerived");
	MonoMethod *make_control = method_named ("UnityPinnedAllocHost", "MakeControl");

	ASSERT_NE (nullptr, make_derived);
	ASSERT_NE (nullptr, make_control);

	ASSERT_TRUE (mono_llvm_jit_promote_now (
		make_derived, domain, (uint8_t) mono::MonoTier::tier1))
		<< "MakeDerived would not compile at tier 1";
	ASSERT_TRUE (mono_llvm_jit_promote_now (
		make_control, domain, (uint8_t) mono::MonoTier::tier1))
		<< "MakeControl would not compile at tier 1";

	ERROR_DECL (error);
	MonoObject *derived = mono_runtime_invoke_checked (make_derived, nullptr, nullptr, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, derived);
	EXPECT_FALSE (sgen_ptr_in_nursery ((char *) derived));

	MonoObject *control = mono_runtime_invoke_checked (make_control, nullptr, nullptr, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, control);
	EXPECT_TRUE (sgen_ptr_in_nursery ((char *) control));
}

/*
 * Two ctest registrations run this case, one with
 * MONO_DISABLE_UNITY_PINNED_ALLOC unset and one with it set
 * (CMakeLists.txt). It reads the variable back to know which arm it is in.
 */
TEST_F (UnityPinnedAlloc, RoutingRespectsKillSwitch)
{
	MonoDomain *domain = mono_domain_get ();
	MonoClass *klass = class_named ("", "Derived");
	ASSERT_NE (nullptr, klass);

	// The bit stays set either way. Only the routing below is gated.
	EXPECT_TRUE (m_class_alloc_pinned (klass));

	ERROR_DECL (error);
	MonoObject *derived = mono_object_new_checked (domain, klass, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, derived);

	if (g_hasenv ("MONO_DISABLE_UNITY_PINNED_ALLOC"))
		EXPECT_TRUE (sgen_ptr_in_nursery ((char *) derived));
	else
		EXPECT_FALSE (sgen_ptr_in_nursery ((char *) derived));
}

} // namespace
