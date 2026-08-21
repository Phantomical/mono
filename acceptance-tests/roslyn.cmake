# The Roslyn test run, from roslyn.mk.
#
# Nothing to port: the whole recipe is commented out upstream, leaving
# `check-roslyn:` as an empty target.  The comment there says why --
#
#     # Disabled because it doesn't build anymore due to removed myget feeds
#
# -- so the suite has not run since those feeds went away, and the roslyn
# checkout is now just another pinned submodule alongside the others in this
# directory.  Reviving it means fixing the restore inside the roslyn
# checkout, not anything on this side, so this file adds no disabled-by-default
# CMake target either: there would be nothing behind it.
#
# `print-versions` still reports the pinned roslyn revision.
