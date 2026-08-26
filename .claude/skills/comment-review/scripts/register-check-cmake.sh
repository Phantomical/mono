#!/bin/bash
# Mechanical register check over the comments of one or more CMake files.
#
# Same checks as register-check.sh, over `#` comments instead of C ones. Every
# hit is a candidate, not a verdict - read each one.
#
# Usage: register-check-cmake.sh <file>...

set -u

for f in "$@"; do
	echo "=== $f"

	# Comment text only: whole-line # comments, and trailing ones. A `#` inside
	# a quoted argument is picked up too, so a hit on a line holding a string
	# is worth a second look.
	awk '
		{
			line = $0
			if (line ~ /^[[:space:]]*#/)
				sub(/^[[:space:]]*#+[[:space:]]?/, "", line)
			else if (line ~ /#/)
				sub(/^[^#]*#+[[:space:]]?/, "", line)
			else
				next
			# A FIXME/TODO is a note to whoever picks the work up, not
			# documentation. Rewriting one is churn on words the author
			# left deliberately, so keep it out of every check below.
			if (line ~ /^(FIXME|TODO|XXX|HACK|NOTE)/) next
			print FILENAME ":" FNR ":" line
		}
	' "$f" > /tmp/.crc-text.$$

	echo "-- semicolon joining clauses"
	grep -nE '[a-z]; +[a-z]' /tmp/.crc-text.$$ | sed 's/^[0-9]*://'

	echo "-- hedging modals (should/might: say what happens, or say you do not know)"
	grep -nEi '\b(should|might)\b' /tmp/.crc-text.$$ | sed 's/^[0-9]*://'

	echo "-- conditional modals (would/could/may: correct in a counterfactual, a hedge otherwise)"
	grep -nEi '\b(would|could|may)\b' /tmp/.crc-text.$$ | sed 's/^[0-9]*://'

	echo "-- filler"
	grep -nEi '\b(simply|just|note that|essentially|basically|obviously|of course)\b' \
		/tmp/.crc-text.$$ | sed 's/^[0-9]*://'

	echo "-- empty subject / unfalsifiable"
	grep -nEi 'nothing (here|else)|load-bearing|important to note' /tmp/.crc-text.$$ |
		sed 's/^[0-9]*://'

	# Claudish scaffolding. These carry no fact, so the fix is a deletion. The
	# shapes that cost most - one proposition stated three ways, a contrast
	# against an invented alternative - are not greppable, so a clean run here
	# says nothing about the block. See reference/claudish.md.
	echo "-- rhetorical scaffolding (staged emphasis, orientation, aphoristic ending)"
	grep -nEi 'in other words|put differently|in one sentence|to be clear|the (key|real) (distinction|point|question|insight)|the deeper (point|issue)|the honest (answer|take)|that is the (boundary|constraint|point)|the whole point|not (merely|so much)' \
		/tmp/.crc-text.$$ | sed 's/^[0-9]*://'

	echo "-- sentences over 25 words"
	awk -F: '{
		line = $0
		sub(/^[^:]*:[0-9]*:/, "", line)
		buf = buf " " line
		while (match(buf, /[.!?]([[:space:]]|$)/)) {
			s = substr(buf, 1, RSTART)
			buf = substr(buf, RSTART + RLENGTH)
			n = split(s, w, /[[:space:]]+/)
			if (n > 25) print "  " n " words: " s
		}
	}' /tmp/.crc-text.$$

	# A banner rule is not a comment. These carry no information and go.
	echo "-- separator banners"
	grep -nE '^[[:space:]]*#[[:space:]]*[-=*#_]{5,}' "$f" | sed 's/:.*//' | tr '\n' ' '
	echo

	rm -f /tmp/.crc-text.$$
done
