# The Roslyn test run, from roslyn.mk.
#
# Nothing to port: the whole recipe is commented out upstream, leaving
# `check-roslyn:` as an empty target.  The comment there says why --
#
#     # Disabled because it doesn't build anymore due to removed myget feeds
#
# -- so the suite has not run since those feeds went away, and the roslyn
# checkout it names is only kept alive by validate-roslyn.  Reviving it means
# fixing the restore inside the roslyn checkout, not anything on this side, so
# there is no disabled-by-default CMake target here either; there would be
# nothing behind it.
#
# `print-versions` still reports the pinned roslyn revision.
