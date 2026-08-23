#ifndef __MONO_INTERP_INTERP_SHARING_HPP__
#define __MONO_INTERP_INTERP_SHARING_HPP__

/**
 * \file
 * \brief Which methods run one body between their reference instantiations, and
 * what such a body has to ask its context for.
 *
 * The compiled tier answers the same questions in
 * mono/llvm/method-to-llvm/generic-sharing.cpp, and both engines read them out
 * of mini. Keeping the two on one set of predicates is what makes a method share
 * at both tiers or at neither.
 */

#include "internals.hpp"

namespace mono::interp {

/// Returns the method whose body serves every reference instantiation of
/// \p method, or NULL when this method gets a body of its own.
///
/// Reference sharing only, so a value type argument gets a body of its own. The
/// answer is NULL for a method that is already open, which is what stops a
/// shared form being asked for its own shared form.
MonoMethod *shared_form (MonoMethod *method);

/// Whether a body shared between instantiations has to ask its context for
/// \p klass rather than name it outright.
///
/// False for a generic type definition. Its type parameters are its own rather
/// than the enclosing method's, so typeof (List<>) is one object however the
/// body around it was instantiated.
bool depends_on_context (MonoClass *klass);

bool depends_on_context (MonoClassField *field);

bool depends_on_context (MonoMethod *target);

/// Whether \p method is entered with its context passed in rather than read out
/// of its receiver.
///
/// True for a static method, a value type's, a default interface method and any
/// method carrying type parameters of its own - the shapes with no receiver
/// whose vtable would answer the question.
bool takes_rgctx_argument (MonoMethod *method);

} // namespace mono::interp

#endif
