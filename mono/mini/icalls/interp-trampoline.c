/**
 * \file
 * Forwards to the interpreter's entry and pinvoke trampolines.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void
mono_interp_entry_from_trampoline (gpointer ccontext, gpointer imethod)
{
	mini_get_interp_callbacks ()->entry_from_trampoline (ccontext, imethod);
}

void
mono_interp_to_native_trampoline (gpointer addr, gpointer ccontext)
{
	mini_get_interp_callbacks ()->to_native_trampoline (addr, ccontext);
}
