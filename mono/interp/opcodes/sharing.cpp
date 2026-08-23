/**
 * \file
 * \brief The metadata a body shared between reference instantiations reads out
 * of its generic context.
 */

#include "config.h"

#include "mintops.hpp"
#include "mono/interp/interp.hpp"
#include "mono/interp/runtime/object.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/mini/mini-runtime.h"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

/*
 * The receiver carries the context. shared_form () only shares a method that
 * reads its context that way, so the vtable below is the instantiation's own and
 * the slot answers for the instantiation the caller asked for.
 *
 * Filling a slot can build a vtable and run a class initializer, so the fetch
 * happens where the IL asked for the metadata rather than at frame entry. That
 * is also why it can throw.
 */
MONO_INTERP_OP_IMPL (MINT_RGCTX_FETCH)
{
	auto self = LOCAL_VAR (ip[2], MonoObject *);

	NULL_CHECK (self);

	ERROR_DECL (error);
	gpointer info = mono_class_fill_runtime_generic_context (self->vtable, READ32 (ip + 3), error);

	if (G_UNLIKELY (!is_ok (error)))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	LOCAL_VAR (ip[1], gpointer) = info;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
