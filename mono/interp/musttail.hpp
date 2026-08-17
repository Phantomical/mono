/**
 * \file
 * \brief The attribute that makes a return statement a jump.
 *
 * Each compiler spells the guaranteed tail call with its own attribute.
 * MONO_MUSTTAIL expands to the one the compiler in use accepts, and goes in
 * front of the return:
 *
 *     MONO_MUSTTAIL return next (ip, sp, frame);
 *
 * Clang and GCC report an error where they cannot make the call a jump. MSVC
 * takes the attribute on x64 only, and it can emit an ordinary call with no
 * diagnostic, so a clean build there is not a guarantee.
 */

#ifndef __MONO_INTERP_MUSTTAIL_HPP__
#define __MONO_INTERP_MUSTTAIL_HPP__

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::musttail)
#define MONO_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(gnu::musttail)
#define MONO_MUSTTAIL [[gnu::musttail]]
#elif __has_cpp_attribute(msvc::musttail)
#define MONO_MUSTTAIL [[msvc::musttail]]
#endif
#endif

#ifdef MONO_MUSTTAIL
#define MONO_HAVE_MUSTTAIL 1
#else
#define MONO_HAVE_MUSTTAIL 0
#endif

#endif /* __MONO_INTERP_MUSTTAIL_HPP__ */
