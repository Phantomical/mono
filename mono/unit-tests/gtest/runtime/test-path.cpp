/*
 * test-path.cpp
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include "glib.h"

/*
 * The file rather than the header, because the helpers under test are static to
 * it.  extern "C" because mono_path_filename_in_basedir is the one entry point
 * here without MONO_API on it: without this the name it gets in a C++ TU is not
 * the one the runtime's callers ask for, the linker pulls mono-path.o out of
 * libmonosgen to satisfy them, and its other two definitions collide with ours.
 */
extern "C" {
#include "mono/utils/mono-path.c"
}

#include <string_view>

#include <gtest/gtest.h>

namespace {

/* A with 0, 1 or 2 trailing slashes, optionally spelled the Windows way. */
char*
make_path (const char *a, int itrail, int slash, int upcase)
{
	g_assert (itrail >= 0 && itrail <= 2);
	char trail [] = "//";
	trail [itrail] = 0;
	char *b = g_strdup_printf ("%s%s", a, trail);

#ifdef HOST_WIN32
	if (slash)
		g_strdelimit (b, '/', '\\');
	if (upcase)
		g_strdelimit (b, 'a', 'A');
#endif
	return b;
}

} // namespace

/*
 * The cross product of base and file, each spelled every way that must not
 * change the answer: trailing slashes, and on Windows also separator and case.
 */
TEST (MonoPath, FilenameInBasedir)
{
	// Use letters not numbers in this data to exercise case insensitivity.
	static const char * const bases [2] = {"/", "/a"};
	static const char * const files [6] = {"/a", "/a/b", "/a/b/c", "/ab", "/b", "/b/b/"};

	static const gboolean result [2][6] = {
		{ TRUE, FALSE, FALSE, TRUE, TRUE, FALSE },
		{ FALSE, TRUE, FALSE, FALSE, FALSE, FALSE }
	};

#ifdef HOST_WIN32
	const int win32 = 1;
#else
	const int win32 = 0;
#endif

	for (int upcase_file = 0; upcase_file <= win32; ++upcase_file) {
	for (int upcase_base = 0; upcase_base <= win32; ++upcase_base) {
	for (int itrail_base = 0; itrail_base <= 2; ++itrail_base) {
	for (int itrail_file = 0; itrail_file <= 2; ++itrail_file) {
	for (int ibase = 0; ibase < G_N_ELEMENTS (bases); ++ibase) {
	for (int ifile = 0; ifile < G_N_ELEMENTS (files); ++ifile) {
	for (int islash_base = 0; islash_base <= win32; ++islash_base) {
	for (int islash_file = 0; islash_file <= win32; ++islash_file) {
		char *base = make_path (bases [ibase], itrail_base, islash_base, upcase_base);
		char *file = make_path (files [ifile], itrail_file, islash_file, upcase_file);

		gboolean r = mono_path_filename_in_basedir (file, base);

		EXPECT_EQ (result [ibase][ifile], r)
			<< "mono_path_filename_in_basedir (" << file << ", " << base << ")";
		/* A path is never inside itself. */
		if (std::string_view (base) == file)
			EXPECT_FALSE (r)
				<< "mono_path_filename_in_basedir (" << file << ", " << base << ")";

		g_free (base);
		g_free (file);
	}}}}}}}}
}
