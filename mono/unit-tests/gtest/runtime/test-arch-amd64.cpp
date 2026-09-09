/*
 * test-arch-amd64.cpp: Unit test for mono_arch_get_call_info ()'s Windows
 * hidden-return-pointer placement.
 *
 * Windows x64 keeps the integer and SSE argument-register files at the same
 * positional slot: slot 0 is RCX/XMM0, slot 1 is RDX/XMM1, and so on, so a
 * leading floating-point argument still burns an integer register. A static
 * method whose return needs a hidden pointer places that pointer behind its
 * first argument (mono/llvm/hidden-return.hpp's own convention, which this
 * file mirrors for the classic tier-0 compiler), so the pointer's slot has to
 * follow from where that first argument actually landed, not from a general
 * register cursor that a floating-point argument never advances.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/object-internals.h"

/*
 * mini.h pulls this header in ahead of its own G_BEGIN_DECLS, so a C++
 * translation unit that reaches mono_arch_get_call_info () only through
 * mini.h sees it with C++ linkage while arch-amd64.c defines it with C
 * linkage. Including it here, wrapped, first establishes the declaration
 * with the linkage the definition actually has; mini.h's own unwrapped
 * include further down is then a no-op under this header's include guard.
 */
extern "C" {
#include "mini/mini-amd64.h"
}

#include "mini/mini.h"

#include <gtest/gtest.h>

#include "harness.hpp"

#ifdef HOST_WIN32

namespace {

/// A static (double, int32) signature returning ret_class by value.
MonoMethodSignature *
double_int_signature_returning (MonoClass *ret_class)
{
	MonoMethodSignature *sig = mono_metadata_signature_alloc (mono_defaults.corlib, 2);
	// sig->params is declared as a one-element array and grown past that by
	// mono_metadata_signature_alloc ()'s own trailing allocation; indexing
	// through a plain pointer is what keeps the compiler's static bounds
	// check from reading the declared size as the real one.
	MonoType **params = sig->params;

	sig->ret = m_class_get_byval_arg (ret_class);
	params [0] = m_class_get_byval_arg (mono_defaults.double_class);
	params [1] = m_class_get_byval_arg (mono_defaults.int32_class);
	return sig;
}

} // namespace

/*
 * Guid is 16 bytes, which MONO_WIN64_VALUE_TYPE_FITS_REG refuses, so its
 * return travels through a hidden pointer placed behind the leading double.
 * That double takes slot 0 as XMM0, so the locked-slot convention puts the
 * pointer at slot 1 (RDX) and the int parameter that follows it at slot 2
 * (R8) -- not RCX and RDX, which is what a general-register cursor left
 * unadvanced by the double would give both of them instead.
 */
TEST (ArchAmd64, HiddenReturnFollowsTheSlotAFloatFirstArgumentLeaves)
{
	MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
	mono::test::init_runtime ();

	ERROR_DECL (error);
	MonoClass *guid_class =
		mono_class_from_name_checked (mono_defaults.corlib, "System", "Guid", error);

	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, guid_class);

	MonoMethodSignature *sig = double_int_signature_returning (guid_class);
	CallInfo *cinfo = mono_arch_get_call_info (NULL, sig);

	ASSERT_EQ (ArgValuetypeAddrInIReg, cinfo->ret.storage);
	EXPECT_EQ (AMD64_RDX, cinfo->ret.reg);
	EXPECT_EQ (AMD64_R8, cinfo->args [1].reg);

	g_free (cinfo);
}

#endif /* HOST_WIN32 */
