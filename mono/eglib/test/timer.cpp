#include <config.h>

#include <math.h>
#include <stdlib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <glib.h>
#include <gtest/gtest.h>

#ifdef G_OS_WIN32
#include <windows.h>
#define sleep(t) Sleep((t) * 1000)
#endif

TEST (timer, elapsed)
{
	GTimer *timer = g_timer_new ();
	gulong usec = 0;

	sleep (1);
	gdouble elapsed1 = g_timer_elapsed (timer, NULL);
	ASSERT_GE (elapsed1 + 0.1, 1.0) << "Elapsed time should be around 1s";

	g_timer_stop (timer);
	elapsed1 = g_timer_elapsed (timer, NULL);
	gdouble elapsed2 = g_timer_elapsed (timer, &usec);
	ASSERT_LE (fabs (elapsed1 - elapsed2), 0.000001) << "A stopped timer must read the same twice";

	/* The usec out-parameter is the sub-second part of the same reading. */
	elapsed2 *= 1000000;
	while (elapsed2 > 1000000)
		elapsed2 -= 1000000;

	ASSERT_LE (fabs (usec - elapsed2), 100.0) << "usecs are wrong";

	g_timer_destroy (timer);
}
