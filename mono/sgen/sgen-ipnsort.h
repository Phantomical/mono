/**
 * \file
 * Pattern-defeating unstable sort for pointer arrays.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#ifndef __MONO_SGEN_IPNSORT_H__
#define __MONO_SGEN_IPNSORT_H__

#include <stddef.h>

/* Sorts \p array into ascending address order. */
void sgen_ipnsort (void **array, size_t size);

#endif
