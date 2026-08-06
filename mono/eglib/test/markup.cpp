#include <string.h>
#include <string_view>

#include <glib.h>
#include <gtest/gtest.h>

namespace {

/* Returns the parse error message, or NULL if the document parsed. */
char *
markup_error (const char *s)
{
	GMarkupParser *parser = g_new0 (GMarkupParser, 1);
	GMarkupParseContext *context = g_markup_parse_context_new (parser, (GMarkupParseFlags) 0, 0, 0);
	GError *gerror = NULL;

	g_markup_parse_context_parse (context, s, strlen (s), &gerror);
	g_markup_parse_context_free (context);
	g_free (parser);

	if (gerror == NULL)
		return NULL;

	char *msg = g_strdup (gerror->message);
	g_error_free (gerror);
	return msg;
}

void
expect_rejected (const char *s)
{
	char *msg = markup_error (s);
	EXPECT_NE (nullptr, msg) << "should not have parsed: " << s;
	g_free (msg);
}

void
expect_parsed (const char *s)
{
	char *msg = markup_error (s);
	EXPECT_EQ (nullptr, msg) << "should have parsed: " << s << ": " << (msg ? msg : "");
	g_free (msg);
}

/*
 * This is a test for the kind of files that the code in mono/domain.c
 * parses;  This code comes from Mono
 */
struct AppConfigInfo {
	GSList *supported_runtimes;
	char *required_runtime;
	int configuration_count;
	int startup_count;
};

char *
get_attribute_value (const gchar **attribute_names,
		     const gchar **attribute_values,
		     const char *att_name)
{
	for (int n = 0; attribute_names [n] != NULL; n++) {
		if (std::string_view (attribute_names [n]) == att_name)
			return g_strdup (attribute_values [n]);
	}
	return NULL;
}

void
start_element (GMarkupParseContext *context,
	       const gchar         *element_name,
	       const gchar        **attribute_names,
	       const gchar        **attribute_values,
	       gpointer             user_data,
	       GError             **gerror)
{
	AppConfigInfo *app_config = (AppConfigInfo *) user_data;
	std::string_view name (element_name);

	if (name == "configuration") {
		app_config->configuration_count++;
		return;
	}
	if (name == "startup") {
		app_config->startup_count++;
		return;
	}

	if (app_config->configuration_count != 1 || app_config->startup_count != 1)
		return;

	if (name == "requiredRuntime") {
		app_config->required_runtime = get_attribute_value (attribute_names, attribute_values, "version");
	} else if (name == "supportedRuntime") {
		char *version = get_attribute_value (attribute_names, attribute_values, "version");
		app_config->supported_runtimes = g_slist_append (app_config->supported_runtimes, version);
	}
}

void
end_element (GMarkupParseContext *context,
	     const gchar         *element_name,
	     gpointer             user_data,
	     GError             **gerror)
{
	AppConfigInfo *app_config = (AppConfigInfo *) user_data;
	std::string_view name (element_name);

	if (name == "configuration")
		app_config->configuration_count--;
	else if (name == "startup")
		app_config->startup_count--;
}

const GMarkupParser mono_parser = {
	start_element,
	end_element,
	NULL,
	NULL,
	NULL
};

AppConfigInfo *
domain_test (const char *text)
{
	AppConfigInfo *app_config = g_new0 (AppConfigInfo, 1);
	GMarkupParseContext *context =
		g_markup_parse_context_new (&mono_parser, (GMarkupParseFlags) 0, app_config, NULL);

	if (g_markup_parse_context_parse (context, text, strlen (text), NULL))
		g_markup_parse_context_end_parse (context, NULL);
	g_markup_parse_context_free (context);

	return app_config;
}

void
domain_free (AppConfigInfo *info)
{
	g_free (info->required_runtime);
	for (GSList *l = info->supported_runtimes; l != NULL; l = l->next)
		g_free (l->data);
	g_slist_free (info->supported_runtimes);
	g_free (info);
}

}

TEST (markup, invalid_documents)
{
	expect_rejected ("<1>");
	expect_rejected ("<a<");
	expect_rejected ("</a>");
	expect_rejected ("<a b>");
	expect_rejected ("<a b=>");
	expect_rejected ("<a b=c>");
}

TEST (markup, good_documents)
{
	expect_parsed ("<a>");
	expect_parsed ("<a a=\"b\">");
}

TEST (markup, mono_domain)
{
	AppConfigInfo *info;

	info = domain_test ("<configuration><!--hello--><startup><!--world--><requiredRuntime version=\"v1\"><!--r--></requiredRuntime></startup></configuration>");
	EXPECT_STREQ ("v1", info->required_runtime);
	domain_free (info);

	info = domain_test ("<configuration><startup><requiredRuntime version=\"v1\"/><!--comment--></configuration><!--end-->");
	EXPECT_STREQ ("v1", info->required_runtime) << "on auto-close section";
	domain_free (info);

	info = domain_test ("<!--start--><configuration><startup><supportedRuntime version=\"v1\"/><!--middle--><supportedRuntime version=\"v2\"/></startup></configuration>");
	ASSERT_NE (nullptr, info->supported_runtimes);
	EXPECT_STREQ ("v1", (const char *) info->supported_runtimes->data);
	ASSERT_NE (nullptr, info->supported_runtimes->next) << "Expected 2 supported runtimes";
	EXPECT_STREQ ("v2", (const char *) info->supported_runtimes->next->data);
	EXPECT_EQ (nullptr, info->supported_runtimes->next->next) << "Expected v1, v2, got more";
	domain_free (info);
}

TEST (markup, mcs_config)
{
	expect_parsed ("<configuration>\r\n  <system.diagnostics>\r\n    <trace autoflush=\"true\" indentsize=\"4\">\r\n      <listeners>\r\n        <add name=\"compilerLogListener\" type=\"System.Diagnostics.TextWriterTraceListener,System\"/>      </listeners>    </trace>   </system.diagnostics> </configuration>");
}

TEST (markup, xml_parse)
{
	expect_parsed ("<?xml version=\"1.0\" encoding=\"utf-8\"?><a></a>");
}

/*
 * A real machine.config out of the tree, which is a good deal larger and
 * hairier than anything spelled out above.
 */
TEST (markup, machine_config)
{
	char *path = g_build_filename (EGLIB_TEST_DATA_DIR, "net_1_1", "machine.config", (const char*)NULL);
	char *data = NULL;
	gsize size;
	gboolean loaded = g_file_get_contents (path, &data, &size, NULL);

	if (!loaded)
		GTEST_SKIP () << "no machine.config at " << path;

	g_free (path);
	expect_parsed (data);
	g_free (data);
}
