/**
 * \file
 * \brief The plan an interpreted caller reaches a compiled method through.
 *
 * Unlike an interp-entry layout, a plan is keyed on the prototype alone.
 * interp.hpp's prototype_key () cannot tell a byref parameter from an
 * ordinary reference, which is why interp-entry needs signature_key_suffix ()
 * added to it. A dyn call reads every argument through a pointer to its own
 * storage either way, so the prototype alone already picks the plan.
 */

#ifndef MONO_LLVM_RUNTIME_DYN_CALL_PLAN_HPP
#define MONO_LLVM_RUNTIME_DYN_CALL_PLAN_HPP

#include "arch/arch.hpp"

#include <llvm/Support/Error.h>

typedef struct _MonoMethod MonoMethod;

namespace mono {

/// Returns the plan \p method's compiled body is reached through, building
/// and caching one for its prototype on first request.
///
/// An error says this method's call cannot be planned. The caller then has to
/// reach it another way, a compiled wrapper for the interpreter.
llvm::Expected<const arch::DynCallPlan *> dyn_call_plan_for (MonoMethod *method);

} // namespace mono

#endif
