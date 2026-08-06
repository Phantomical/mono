#include <glib.h>
#include <gtest/gtest.h>

TEST (shell, parse_argv1)
{
	GError *gerror;
	gint argc;
	gchar **argv;

	/* The next line prints a critical error and returns FALSE
	ret = g_shell_parse_argv (NULL, NULL, NULL, NULL);
	*/
	ASSERT_FALSE (g_shell_parse_argv ("", NULL, NULL, NULL));
	ASSERT_TRUE (g_shell_parse_argv ("hola", NULL, NULL, NULL));

	argc = 0;
	ASSERT_TRUE (g_shell_parse_argv ("hola", &argc, NULL, NULL));
	ASSERT_EQ (1, argc);

	argc = 0;
	ASSERT_TRUE (g_shell_parse_argv ("hola bola", &argc, NULL, NULL));
	ASSERT_EQ (2, argc);

	argc = 0;
	ASSERT_TRUE (g_shell_parse_argv ("hola bola", &argc, &argv, NULL));
	ASSERT_EQ (2, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("bola", argv [1]);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola      'bola'", &argc, &argv, &gerror));
	ASSERT_EQ (2, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("bola", argv [1]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola    ''  'bola'", &argc, &argv, &gerror));
	ASSERT_EQ (3, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("", argv [1]);
	ASSERT_STREQ ("bola", argv [2]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola'' bola", &argc, &argv, &gerror));
	ASSERT_EQ (2, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("bola", argv [1]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);
}

TEST (shell, parse_argv2)
{
	GError *gerror = NULL;
	gint argc = 0;
	gchar **argv = NULL;

	ASSERT_TRUE (g_shell_parse_argv ("hola      \"bola\"", &argc, &argv, &gerror));
	ASSERT_EQ (2, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("bola", argv [1]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola    \"\"  \"bola \"", &argc, &argv, &gerror));
	ASSERT_EQ (3, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("", argv [1]);
	ASSERT_STREQ ("bola ", argv [2]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola\n\t    \"\t\"  \"bola \"", &argc, &argv, &gerror));
	ASSERT_EQ (3, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("\t", argv [1]);
	ASSERT_STREQ ("bola ", argv [2]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola\n\t  \\\n  \"\t\"  \"bola \"", &argc, &argv, &gerror));
	ASSERT_EQ (3, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("\t", argv [1]);
	ASSERT_STREQ ("bola ", argv [2]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);
}

TEST (shell, parse_argv3)
{
	GError *gerror = NULL;
	gint argc = 0;
	gchar **argv = NULL;

	/* Text ends before the matching quote is found for ". */
	ASSERT_FALSE (g_shell_parse_argv ("hola      \"bola", &argc, &argv, &gerror));
	ASSERT_EQ (0, argc);
	ASSERT_EQ (nullptr, argv);
	ASSERT_NE (nullptr, gerror);

	g_error_free (gerror);
	gerror = NULL;
	ASSERT_TRUE (g_shell_parse_argv ("hola      \\\"bola", &argc, &argv, &gerror));
	ASSERT_EQ (2, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("\"bola", argv [1]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);

	argv = NULL;
	argc = 0;
	ASSERT_TRUE (g_shell_parse_argv ("hola      \"\n\\'bola\"", &argc, &argv, &gerror))
		<< (gerror ? gerror->message : "");
	ASSERT_EQ (2, argc);
	ASSERT_STREQ ("hola", argv [0]);
	ASSERT_STREQ ("\n\\'bola", argv [1]);
	ASSERT_EQ (nullptr, gerror);
	g_strfreev (argv);
}

// This was the 2.8 showstopper error
TEST (shell, parse_argv4)
{
	const char *str = "'/usr/bin/gnome-terminal' -e \"bash -c 'read -p \\\"Press any key to continue...\\\" -n1;'\"";
	GError *gerror = NULL;
	gint argc = 0;
	gchar **argv = NULL;

	ASSERT_TRUE (g_shell_parse_argv (str, &argc, &argv, &gerror));
	ASSERT_EQ (3, argc);
	ASSERT_NE (nullptr, argv);
	ASSERT_EQ (nullptr, gerror);

	ASSERT_STREQ ("/usr/bin/gnome-terminal", argv [0]);
	ASSERT_STREQ ("-e", argv [1]);
	ASSERT_STREQ ("bash -c 'read -p \"Press any key to continue...\" -n1;'", argv [2]);

	g_strfreev (argv);
}

// This is https://bugzilla.novell.com/show_bug.cgi?id=655896
TEST (shell, parse_argv5)
{
	const char *str = "echo \"foo\",\"bar\"";
	GError *gerror = NULL;
	gint argc = 0;
	gchar **argv = NULL;

	ASSERT_TRUE (g_shell_parse_argv (str, &argc, &argv, &gerror));
	ASSERT_EQ (2, argc);
	ASSERT_NE (nullptr, argv);
	ASSERT_EQ (nullptr, gerror);

	ASSERT_STREQ ("echo", argv [0]);
	ASSERT_STREQ ("foo,bar", argv [1]);

	g_strfreev (argv);
}

TEST (shell, shell_quote)
{
	char *s;

	s = g_shell_quote ("foo");
	ASSERT_STREQ ("'foo'", s);
	g_free (s);

	s = g_shell_quote ("foo'bar");
	ASSERT_STREQ ("'foo'\\''bar'", s);
	g_free (s);

	s = g_shell_quote ("foo bar");
	ASSERT_STREQ ("'foo bar'", s);
	g_free (s);
}
