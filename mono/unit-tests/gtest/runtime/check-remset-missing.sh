#!/bin/sh
#
# Runs test-remset-missing and judges it the way MONO_SGEN_STRICT_REMSET_CHECK
# predicts. ctest's PASS_REGULAR_EXPRESSION treats a signal death as an
# unconditional fail before it reads the regex at all (verified against
# ctest 3.28), and the confirmed report this test drives towards under the
# option is exactly such a death, so that property cannot judge this test.
#
# $1: the test binary. $2: "on" or "off", MONO_SGEN_STRICT_REMSET_CHECK's
# setting at configure time.

set -u

binary=$1
expect=$2

output=$("$binary" 2>&1)
status=$?

printf '%s\n' "$output"

# Names RemsetHolder, so an unrelated missing barrier somewhere else in the
# run cannot stand in for the one this test builds.
confirmed=false
case "$output" in
	*"Missing write barrier: .RemsetHolder field 'Slot'"*) confirmed=true ;;
esac

excused=false
case "$output" in
	*"not found in remsets, but object is pinned"*) excused=true ;;
esac

if [ "$expect" = on ]; then
	# A confirmed miss aborts the process by design.
	[ "$status" -ne 0 ] && [ "$confirmed" = true ]
else
	# The excuse line has to be there. Without it the run proves nothing: a
	# test that built no pinned miss at all also exits 0 and logs nothing.
	[ "$status" -eq 0 ] && [ "$excused" = true ] && [ "$confirmed" = false ]
fi
