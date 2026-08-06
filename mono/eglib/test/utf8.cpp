#include <stdlib.h>
#include <string.h>
#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

namespace {

/*
 * The UTF-* fixture files are read at runtime.  EGLIB_TEST_DATA_SRCDIR is this
 * directory, baked in at configure time so the tests do not depend on the
 * working directory; $srcdir still wins if it is set, which is how the suite
 * has always been pointed at a different copy.
 */
const char *
fixture_dir (void)
{
	const char *srcdir = getenv ("srcdir");
	return (srcdir && *srcdir) ? srcdir : EGLIB_TEST_DATA_SRCDIR;
}

void
expect_utf8_equal (const gchar *expected, const gchar *actual, glong size)
{
	for (glong i = 0; i < size; i++)
		ASSERT_EQ (expected [i], actual [i]) << "differ at " << i;
}

void
expect_utf16_equal (const gunichar2 *expected, const gunichar2 *actual, glong size)
{
	for (glong i = 0; i < size; i++)
		ASSERT_EQ (expected [i], actual [i]) << "differ at " << i;
}

void
gchar_to_gunichar2 (gunichar2 ret [], const gchar *src)
{
	int i;

	for (i = 0; src [i]; i++)
		ret [i] = src [i];
	ret [i] = 0;
}

void
check_utf16_to_utf8 (const gchar *expected, const gunichar2 *utf16,
		     glong len_in, glong len_out, glong size_spec)
{
	GError *gerror = NULL;
	glong in_read, out_read;

	gchar *ret = g_utf16_to_utf8 (utf16, size_spec, &in_read, &out_read, &gerror);
	ASSERT_EQ (nullptr, gerror) << "conversion failed: " << (gerror ? gerror->message : "");
	ASSERT_EQ (len_in, in_read) << "Read size is incorrect";
	ASSERT_EQ (len_out, out_read) << "Converted size is incorrect";
	expect_utf8_equal (expected, ret, len_out);
	g_free (ret);
}

/* Once with an implicit length and once with the length spelled out. */
void
check_utf16_to_utf8_both (const gchar *expected, const gunichar2 *utf16,
			  glong len_in, glong len_out)
{
	check_utf16_to_utf8 (expected, utf16, len_in, len_out, -1);
	check_utf16_to_utf8 (expected, utf16, len_in, len_out, len_in);
}

void
check_utf8_to_utf16 (const gunichar2 *expected, const gchar *utf8,
		     glong len_in, glong len_out, glong size_spec, gboolean include_nuls)
{
	GError *gerror = NULL;
	glong in_read, out_read;
	gunichar2 *ret;

	if (include_nuls)
		ret = eg_utf8_to_utf16_with_nuls (utf8, size_spec, &in_read, &out_read, &gerror);
	else
		ret = g_utf8_to_utf16 (utf8, size_spec, &in_read, &out_read, &gerror);

	ASSERT_EQ (nullptr, gerror) << "conversion failed: " << (gerror ? gerror->message : "");
	ASSERT_EQ (len_in, in_read) << "Read size is incorrect";
	ASSERT_EQ (len_out, out_read) << "Converted size is incorrect";
	expect_utf16_equal (expected, ret, len_out);
	g_free (ret);
}

void
check_utf8_to_utf16_both (const gunichar2 *expected, const gchar *utf8,
			  glong len_in, glong len_out)
{
	check_utf8_to_utf16 (expected, utf8, len_in, len_out, -1, FALSE);
	check_utf8_to_utf16 (expected, utf8, len_in, len_out, len_in, FALSE);
}

void
check_utf8_to_utf16_with_nuls (const gunichar2 *expected, const gchar *utf8,
			       glong len_in, glong len_out)
{
	check_utf8_to_utf16 (expected, utf8, len_in, len_out, len_in, TRUE);
}

template <typename TChar>
void
check_conversion_result (const TChar *result_str, const TChar *expected_str,
			 glong result_items_read, glong expected_items_read,
			 glong result_items_written, glong expected_items_written,
			 GError *result_error, gboolean expect_error)
{
	ASSERT_EQ (expected_items_read, result_items_read) << "Incorrect number of items read";
	ASSERT_EQ (expected_items_written, result_items_written) << "Incorrect number of items written";

	if (expect_error) {
		ASSERT_NE (nullptr, result_error) << "There should be an error code.";
		ASSERT_EQ (nullptr, result_str) << "NULL should be returned when an error occurs.";
		return;
	}

	ASSERT_EQ (nullptr, result_error) << "There should not be an error code.";
	ASSERT_NE (nullptr, result_str) << "When no error occurs NULL should not be returned.";

	for (glong i = 0; i < expected_items_written; i++)
		ASSERT_EQ (expected_str [i], result_str [i]) << "at index " << i;

	ASSERT_EQ (0u, result_str [expected_items_written])
		<< "Null termination not found at the end of the string.";
}

void
check_utf8_case (const gchar *src, const gchar *expected, bool strup)
{
	glong len = (glong) strlen (src);
	gchar *tmp = strup ? g_utf8_strup (src, len) : g_utf8_strdown (src, len);
	glong len2 = (glong) strlen (tmp);

	expect_utf8_equal (expected, tmp, len < len2 ? len2 : len);
	g_free (tmp);
}

const char *const charsets [] = { "UTF-8", "UTF-16LE", "UTF-16BE", "UTF-32LE", "UTF-32BE" };

}

TEST (utf8, utf16_to_utf8)
{
	const gchar *src0 = "", *src1 = "ABCDE", *src2 = "\xE5\xB9\xB4\x27";
	const gchar *src3 = "\xEF\xBC\xA1", *src4 = "\xEF\xBD\x81", *src5 = "\xF0\x90\x90\x80";
	gunichar2 str0 [] = {0}, str1 [6], str2 [] = {0x5E74, 39, 0};
	gunichar2 str3 [] = {0xFF21, 0}, str4 [] = {0xFF41, 0}, str5 [] = {0xD801, 0xDC00, 0};

	gchar_to_gunichar2 (str1, src1);

	/* empty string */
	check_utf16_to_utf8_both (src0, str0, 0, 0);
	check_utf16_to_utf8_both (src1, str1, 5, 5);
	check_utf16_to_utf8_both (src2, str2, 2, 4);
	check_utf16_to_utf8_both (src3, str3, 1, 3);
	check_utf16_to_utf8_both (src4, str4, 1, 3);
	check_utf16_to_utf8_both (src5, str5, 2, 4);
}

TEST (utf8, utf8_to_utf16)
{
	const gchar *src0 = "", *src1 = "ABCDE", *src2 = "\xE5\xB9\xB4\x27";
	const gchar *src3 = "\xEF\xBC\xA1", *src4 = "\xEF\xBD\x81";
	gunichar2 str0 [] = {0}, str1 [6], str2 [] = {0x5E74, 39, 0};
	gunichar2 str3 [] = {0xFF21, 0}, str4 [] = {0xFF41, 0};

	gchar_to_gunichar2 (str1, src1);

	/* empty string */
	check_utf8_to_utf16_both (str0, src0, 0, 0);
	check_utf8_to_utf16_both (str1, src1, 5, 5);
	check_utf8_to_utf16_both (str2, src2, 4, 2);
	check_utf8_to_utf16_both (str3, src3, 3, 1);
	check_utf8_to_utf16_both (str4, src4, 3, 1);
}

TEST (utf8, utf8_to_utf16_with_nuls)
{
	const gchar *src0 = "", *src1 = "AB\0DE", *src2 = "\xE5\xB9\xB4\x27";
	const gchar *src3 = "\xEF\xBC\xA1", *src4 = "\xEF\xBD\x81";
	gunichar2 str0 [] = {0}, str1 [] = {'A', 'B', 0, 'D', 'E', 0}, str2 [] = {0x5E74, 39, 0};
	gunichar2 str3 [] = {0xFF21, 0}, str4 [] = {0xFF41, 0};

	/* implicit length is forbidden */
	ASSERT_EQ (nullptr, eg_utf8_to_utf16_with_nuls (src1, -1, NULL, NULL, NULL))
		<< "explicit nulls must fail with -1 length";

	/* empty string */
	check_utf8_to_utf16_with_nuls (str0, src0, 0, 0);
	check_utf8_to_utf16_with_nuls (str1, src1, 5, 5);
	check_utf8_to_utf16_with_nuls (str2, src2, 4, 2);
	check_utf8_to_utf16_with_nuls (str3, src3, 3, 1);
	check_utf8_to_utf16_with_nuls (str4, src4, 3, 1);
}

TEST (utf8, utf8_seq)
{
	const gchar *src = "\xE5\xB9\xB4\x27";
	glong in_read, out_read;
	GError *gerror = NULL;

	gunichar2 *dst = g_utf8_to_utf16 (src, (glong) strlen (src), &in_read, &out_read, &gerror);
	ASSERT_EQ (nullptr, gerror) << (gerror ? gerror->message : "");
	ASSERT_EQ (4, in_read);
	ASSERT_EQ (2, out_read);
	g_free (dst);
}

/* Every charset round-trips into every other one, byte for byte. */
TEST (utf8, convert)
{
	struct Sample {
		char *content;
		gsize length;
	};

	Sample expected [G_N_ELEMENTS (charsets)] = {};
	const char *srcdir = fixture_dir ();
	bool all_loaded = true;

	/* first load all our test samples... */
	for (guint i = 0; i < G_N_ELEMENTS (charsets); i++) {
		char *path = g_strdup_printf ("%s%c%s.txt", srcdir, G_DIR_SEPARATOR, charsets [i]);
		GError *err = NULL;

		if (!g_file_get_contents (path, &expected [i].content, &expected [i].length, &err)) {
			ADD_FAILURE () << "Failed to load " << path << ": " << err->message;
			g_error_free (err);
			all_loaded = false;
		}
		g_free (path);
	}

	/* test conversion from every charset to every other charset */
	for (guint i = 0; all_loaded && i < G_N_ELEMENTS (charsets); i++) {
		for (guint j = 0; j < G_N_ELEMENTS (charsets); j++) {
			gsize converted_length;
			char *converted = g_convert (expected [i].content, expected [i].length,
						     charsets [j], charsets [i], NULL,
						     &converted_length, NULL);

			ASSERT_NE (nullptr, converted)
				<< "Failed to convert from " << charsets [i] << " to " << charsets [j];
			ASSERT_EQ (expected [j].length, converted_length)
				<< "converting from " << charsets [i] << " to " << charsets [j];

			for (gsize n = 0; n < converted_length; n++) {
				if (converted [n] != expected [j].content [n]) {
					ADD_FAILURE () << "Failed to convert from " << charsets [i]
						       << " to " << charsets [j] << " at offset " << n;
					break;
				}
			}

			g_free (converted);
		}
	}

	for (guint k = 0; k < G_N_ELEMENTS (charsets); k++)
		g_free (expected [k].content);
}

TEST (utf8, unichar_xdigit_value)
{
	static const char test_chars [] = {
		'0', '1', '2', '3', '4',
		'5', '6', '7', '8', '9',
		'a', 'b', 'c', 'd', 'e', 'f', 'g',
		'A', 'B', 'C', 'D', 'E', 'F', 'G'};
	static const gint32 test_values [] = {
		0, 1, 2, 3, 4,
		5, 6, 7, 8, 9,
		10, 11, 12, 13, 14, 15, -1,
		10, 11, 12, 13, 14, 15, -1};

	for (size_t i = 0; i < sizeof (test_chars); i++)
		ASSERT_EQ (test_values [i], g_unichar_xdigit_value ((gunichar) test_chars [i]))
			<< "at index " << i;
}

TEST (utf8, ucs4_to_utf16)
{
	static gunichar str1 [12] = {'H','e','l','l','o',' ','W','o','r','l','d','\0'};
	static gunichar2 exp1 [12] = {'H','e','l','l','o',' ','W','o','r','l','d','\0'};
	static gunichar str2 [3] = {'h',0x80000000,'\0'};
	static gunichar2 exp2 [2] = {'h','\0'};
	static gunichar str3 [3] = {'h',0xDA00,'\0'};
	static gunichar str4 [3] = {'h',0x10FFFF,'\0'};
	static gunichar2 exp4 [4] = {'h',0xdbff,0xdfff,'\0'};
	static gunichar str5 [7] = {0xD7FF,0xD800,0xDFFF,0xE000,0x110000,0x10FFFF,'\0'};
	static gunichar2 exp5 [5] = {0xD7FF,0xE000,0xdbff,0xdfff,'\0'};
	static gunichar str6 [2] = {0x10400, '\0'};
	static gunichar2 exp6 [3] = {0xD801, 0xDC00, '\0'};
	static glong read_write [12] = {1,1,0,0,0,0,1,1,0,0,1,2};

	glong items_read, items_written;
	GError *err = NULL;
	gunichar2 *res;

	res = g_ucs4_to_utf16 (str1, 12, &items_read, &items_written, &err);
	check_conversion_result (res, exp1, items_read, 11, items_written, 11, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_ucs4_to_utf16 (str2, 0, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 0, items_written, 0, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_ucs4_to_utf16 (str2, 1, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 1, items_written, 1, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_ucs4_to_utf16 (str2, 2, &items_read, &items_written, &err);
	check_conversion_result<gunichar2> (res, 0, items_read, 1, items_written, 0, err, TRUE);
	g_free (res);

	items_read = items_written = 0;
	err = 0;
	res = g_ucs4_to_utf16 (str3, 2, &items_read, &items_written, &err);
	check_conversion_result<gunichar2> (res, 0, items_read, 1, items_written, 0, err, TRUE);
	g_free (res);

	items_read = items_written = 0;
	err = 0;
	res = g_ucs4_to_utf16 (str4, 5, &items_read, &items_written, &err);
	check_conversion_result (res, exp4, items_read, 2, items_written, 3, err, FALSE);
	g_free (res);

	// This loop tests the bounds of the conversion algorithm
	glong current_write_index = 0;
	for (glong i = 0; i < 6; i++) {
		items_read = items_written = 0;
		err = 0;
		res = g_ucs4_to_utf16 (&str5 [i], 1, &items_read, &items_written, &err);
		check_conversion_result (res, &exp5 [current_write_index],
					 items_read, read_write [i * 2],
					 items_written, read_write [(i * 2) + 1],
					 err, !read_write [(i * 2) + 1]);
		g_free (res);
		current_write_index += items_written;
	}

	items_read = items_written = 0;
	err = 0;
	res = g_ucs4_to_utf16 (str6, 1, &items_read, &items_written, &err);
	check_conversion_result (res, exp6, items_read, 1, items_written, 2, err, FALSE);
	g_free (res);
}

TEST (utf8, utf16_to_ucs4)
{
	static gunichar2 str1 [12] = {'H','e','l','l','o',' ','W','o','r','l','d','\0'};
	static gunichar exp1 [12] = {'H','e','l','l','o',' ','W','o','r','l','d','\0'};
	static gunichar2 str2 [7] = {'H', 0xD800, 0xDC01,0xD800,0xDBFF,'l','\0'};
	static gunichar exp2 [3] = {'H',0x00010001,'\0'};
	static gunichar2 str3 [4] = {'H', 0xDC00 ,'l','\0'};
	static gunichar exp3 [2] = {'H','\0'};
	static gunichar2 str4 [20] = {0xDC00,0xDFFF,0xDFF,0xD800,0xDBFF,0xD800,0xDC00,0xD800,0xDFFF,
				      0xD800,0xE000,0xDBFF,0xDBFF,0xDBFF,0xDC00,0xDBFF,0xDFFF,0xDBFF,0xE000,'\0'};
	static gunichar exp4 [6] = {0xDFF,0x10000,0x103ff,0x10fc00,0x10FFFF,'\0'};
	static gunichar2 str5 [3] = {0xD801, 0xDC00, 0};
	static gunichar exp5 [2] = {0x10400, 0};
	static glong read_write [33] = {1,0,0,1,0,0,1,1,1,2,1,0,2,2,1,2,2,1,2,1,0,2,1,0,2,2,1,2,2,1,2,1,0};

	glong items_read, items_written;
	GError *err = NULL;
	gunichar *res;

	res = g_utf16_to_ucs4 (str1, 12, &items_read, &items_written, &err);
	check_conversion_result (res, exp1, items_read, 11, items_written, 11, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_utf16_to_ucs4 (str2, 0, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 0, items_written, 0, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_utf16_to_ucs4 (str2, 1, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 1, items_written, 1, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_utf16_to_ucs4 (str2, 2, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 1, items_written, 1, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_utf16_to_ucs4 (str2, 3, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 3, items_written, 2, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_utf16_to_ucs4 (str2, 4, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 3, items_written, 2, err, FALSE);
	g_free (res);

	items_read = items_written = 0;
	res = g_utf16_to_ucs4 (str2, 5, &items_read, &items_written, &err);
	check_conversion_result (res, exp2, items_read, 4, items_written, 0, err, TRUE);
	g_free (res);

	items_read = items_written = 0;
	err = 0;
	res = g_utf16_to_ucs4 (str3, 5, &items_read, &items_written, &err);
	check_conversion_result (res, exp3, items_read, 1, items_written, 0, err, TRUE);
	g_free (res);

	// This loop tests the bounds of the conversion algorithm
	glong current_read_index = 0, current_write_index = 0;
	for (glong i = 0; i < 11; i++) {
		items_read = items_written = 0;
		err = 0;
		res = g_utf16_to_ucs4 (&str4 [current_read_index], read_write [i * 3],
				       &items_read, &items_written, &err);
		check_conversion_result (res, &exp4 [current_write_index],
					 items_read, read_write [(i * 3) + 1],
					 items_written, read_write [(i * 3) + 2],
					 err, !read_write [(i * 3) + 2]);
		g_free (res);
		current_read_index += read_write [i * 3];
		current_write_index += items_written;
	}

	items_read = items_written = 0;
	err = 0;
	res = g_utf16_to_ucs4 (str5, 2, &items_read, &items_written, &err);
	check_conversion_result (res, exp5, items_read, 2, items_written, 1, err, FALSE);
	g_free (res);
}

TEST (utf8, utf8_strlen)
{
	gchar word1 [] = {(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,(gchar)0xF1, (gchar)0x82, (gchar)0x82, (gchar)0x82,'\0'}; //Valid, len = 5
	gchar word2 [] = {(gchar)0xF1, (gchar)0x82, (gchar)0x82, (gchar)0x82,(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,'\0'}; //Valid, len = 5
	gchar word3 [] = {'h','e',(gchar)0xC2, (gchar)0x82,0x45,'\0'};                                     //Valid, len = 4
	gchar word4 [] = {0x62,(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,'\0'};             //Valid, len = 5

	ASSERT_EQ (5, g_utf8_strlen (word1, -1));
	//Do tests with different values for max parameter.
	ASSERT_EQ (0, g_utf8_strlen (word1, 1));
	ASSERT_EQ (1, g_utf8_strlen (word1, 2));
	ASSERT_EQ (2, g_utf8_strlen (word1, 3));

	ASSERT_EQ (5, g_utf8_strlen (word2, -1));
	ASSERT_EQ (4, g_utf8_strlen (word3, -1));
	ASSERT_EQ (5, g_utf8_strlen (word4, -1));

	ASSERT_EQ (0, g_utf8_strlen (NULL, 0));
}

TEST (utf8, utf8_get_char)
{
	gchar word1 [] = {(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,(gchar)0xF1, (gchar)0x82, (gchar)0x82, (gchar)0x82,'\0'}; //Valid, len = 5

	ASSERT_EQ (0x82u, g_utf8_get_char (&word1 [0]));
	ASSERT_EQ (0x45u, g_utf8_get_char (&word1 [2]));
	ASSERT_EQ (0x1043u, g_utf8_get_char (&word1 [3]));
	ASSERT_EQ (0x58u, g_utf8_get_char (&word1 [6]));
	ASSERT_EQ (0x42082u, g_utf8_get_char (&word1 [7]));
}

TEST (utf8, utf8_next_char)
{
	gchar word1 [] = {(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,(gchar)0xF1, (gchar)0x82, (gchar)0x82, (gchar)0x82,'\0'}; //Valid, len = 5
	gchar word2 [] = {(gchar)0xF1, (gchar)0x82, (gchar)0x82, (gchar)0x82,(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,'\0'}; //Valid, len = 5
	gchar word1Expected [] = {(gchar)0xC2, 0x45,(gchar)0xE1, 0x58, (gchar)0xF1};
	gchar word2Expected [] = {(gchar)0xF1, (gchar)0xC2, 0x45, (gchar)0xE1, 0x58};

	gchar *ptr = word1;
	gint count = 0;
	while (*ptr != 0) {
		ASSERT_LE (count, 4) << "Word1 has gone past its expected length";
		ASSERT_EQ (word1Expected [count], *ptr) << "Word1 at index " << count;
		ptr = g_utf8_next_char (ptr);
		count++;
	}

	count = 0;
	ptr = word2;
	while (*ptr != 0) {
		ASSERT_LE (count, 4) << "Word2 has gone past its expected length";
		ASSERT_EQ (word2Expected [count], *ptr) << "Word2 at index " << count;
		ptr = g_utf8_next_char (ptr);
		count++;
	}
}

TEST (utf8, utf8_validate)
{
	gchar invalidWord1 [] = {(gchar)0xC3, (gchar)0x82, (gchar)0xC1,(gchar)0x90,'\0'}; //Invalid, 1nd oct Can't be 0xC0 or 0xC1
	gchar invalidWord2 [] = {(gchar)0xC1, (gchar)0x89, 0x60, '\0'}; //Invalid, 1st oct can not be 0xC1
	gchar invalidWord3 [] = {(gchar)0xC2, 0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,'\0'}; //Invalid, oct after 0xC2 must be > 0x80

	gchar validWord1 [] = {(gchar)0xC2, (gchar)0x82, (gchar)0xC3,(gchar)0xA0,'\0'}; //Valid
	gchar validWord2 [] = {(gchar)0xC2, (gchar)0x82,0x45,(gchar)0xE1, (gchar)0x81, (gchar)0x83,0x58,(gchar)0xF1, (gchar)0x82, (gchar)0x82, (gchar)0x82,'\0'}; //Valid

	const gchar *end;

	ASSERT_FALSE (g_utf8_validate (invalidWord1, -1, &end));
	ASSERT_EQ (&invalidWord1 [2], end);

	end = NULL;
	ASSERT_FALSE (g_utf8_validate (invalidWord2, -1, &end));
	ASSERT_EQ (&invalidWord2 [0], end);

	end = NULL;
	ASSERT_FALSE (g_utf8_validate (invalidWord3, -1, &end));
	ASSERT_EQ (&invalidWord3 [0], end);

	end = NULL;
	ASSERT_TRUE (g_utf8_validate (validWord1, -1, &end));
	ASSERT_EQ (&validWord1 [4], end);

	end = NULL;
	ASSERT_TRUE (g_utf8_validate (validWord2, -1, &end));
	ASSERT_EQ (&validWord2 [11], end);
}

TEST (utf8, utf8_strup)
{
	check_utf8_case ("aBc", "ABC", true);
	check_utf8_case ("x86-64", "X86-64", true);
	// U+3B1 U+392 -> U+391 U+392
	check_utf8_case ("\xCE\xB1\xCE\x92", "\xCE\x91\xCE\x92", true);
	// U+FF21 -> U+FF21
	check_utf8_case ("\xEF\xBC\xA1", "\xEF\xBC\xA1", true);
	// U+FF41 -> U+FF21
	check_utf8_case ("\xEF\xBD\x81", "\xEF\xBC\xA1", true);
	// U+10428 -> U+10400
	check_utf8_case ("\xF0\x90\x90\xA8", "\xF0\x90\x90\x80", true);
}

TEST (utf8, utf8_strdown)
{
	check_utf8_case ("aBc", "abc", false);
	check_utf8_case ("X86-64", "x86-64", false);
	// U+391 U+3B2 -> U+3B1 U+3B2
	check_utf8_case ("\xCE\x91\xCE\xB2", "\xCE\xB1\xCE\xB2", false);
/*
	// U+FF41 -> U+FF41
	check_utf8_case ("\xEF\xBC\x81", "\xEF\xBC\x81", false);
	// U+FF21 -> U+FF41
	check_utf8_case ("\xEF\xBC\xA1", "\xEF\xBD\x81", false);
	// U+10400 -> U+10428
	check_utf8_case ("\xF0\x90\x90\x80", "\xF0\x90\x90\xA8", false);
*/
}
