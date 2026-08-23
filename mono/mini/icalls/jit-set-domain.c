/**
 * \file
 * The icall that puts the calling thread back in a domain.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#include <mono/metadata/threads-types.h>
#include <mono/utils/mono-threads-coop.h>

/*
 * mono_jit_set_domain:
 *
 * Set domain to @domain if @domain is not null
 */
void
mono_jit_set_domain (MonoDomain *domain)
{
	g_assert (!mono_threads_is_blocking_transition_enabled ());

	if (domain) {
		mono_domain_set_fast (domain, TRUE);
		mono_thread_pop_appdomain_ref ();
	}
}
