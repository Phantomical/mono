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

#ifndef MONO_INTERP_MUSTTAIL_HPP
#define MONO_INTERP_MUSTTAIL_HPP

/* Ask the compiler which spelling it knows rather than deriving it from the
 * compiler and its version. GCC has the attribute from 15 and MSVC from 14.50,
 * and both accepted the surrounding code long before that, so a version test
 * has to carry the numbers and a bare __GNUC__ test picks an attribute GCC 14
 * rejects.  */
#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::musttail)
#define MONO_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(gnu::musttail)
#define MONO_MUSTTAIL [[gnu::musttail]]
#elif __has_cpp_attribute(msvc::musttail)
#define MONO_MUSTTAIL [[msvc::musttail]]
#endif
#endif

#ifndef MONO_MUSTTAIL
#error "this compiler has no musttail attribute: use clang 13, GCC 15, MSVC 14.50 or later"
#endif

#endif /* MONO_INTERP_MUSTTAIL_HPP */
