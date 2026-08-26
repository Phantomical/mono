#!/bin/bash
# Mechanical register check over the comments of one or more C/C++ files.
#
# Every hit is a candidate, not a verdict - a semicolon inside quoted standard
# text stays, and an UPPERCASE hit can be a real macro. Read each one.
#
# Usage: register-check.sh <file>...

set -u

for f in "$@"; do
	echo "=== $f"

	# Comment text only: /// and // lines, and the body of /* */ blocks.
	awk '
		/^[[:space:]]*\/\*/ { inblock = 1 }
		inblock || /^[[:space:]]*\/\// {
			text = $0
			sub(/^[[:space:]]*(\/\/\/?|\/\*+|\*+\/?)[[:space:]]?/, "", text)
			sub(/[[:space:]]*\*\/[[:space:]]*$/, "", text)
			# A FIXME/TODO is a note to whoever picks the work up, not
			# documentation. Rewriting one is churn on words the author
			# left deliberately, so keep it out of every check below.
			if (text ~ /^(FIXME|TODO|XXX|HACK|NOTE)/) next
			print FILENAME ":" FNR ":" text
		}
		/\*\// { inblock = 0 }
	' "$f" > /tmp/.cr-text.$$

	echo "-- semicolon joining clauses"
	grep -nE '[a-z]; +[a-z]' /tmp/.cr-text.$$ | sed 's/^[0-9]*://'

	echo "-- hedging modals (should/might: say what happens, or say you do not know)"
	grep -nEi '\b(should|might)\b' /tmp/.cr-text.$$ | sed 's/^[0-9]*://'

	echo "-- conditional modals (would/could/may: correct in a counterfactual, a hedge otherwise)"
	grep -nEi '\b(would|could|may)\b' /tmp/.cr-text.$$ | sed 's/^[0-9]*://'

	echo "-- filler"
	grep -nEi '\b(simply|just|note that|essentially|basically|obviously|of course)\b' \
		/tmp/.cr-text.$$ | sed 's/^[0-9]*://'

	echo "-- empty subject / unfalsifiable"
	grep -nEi 'nothing (here|else)|load-bearing|important to note' /tmp/.cr-text.$$ |
		sed 's/^[0-9]*://'

	# Claudish scaffolding. These carry no fact, so the fix is a deletion. The
	# shapes that cost most - one proposition stated three ways, a contrast
	# against an invented alternative - are not greppable, so a clean run here
	# says nothing about the block. See reference/claudish.md.
	echo "-- rhetorical scaffolding (staged emphasis, orientation, aphoristic ending)"
	grep -nEi 'in other words|put differently|in one sentence|to be clear|the (key|real) (distinction|point|question|insight)|the deeper (point|issue)|the honest (answer|take)|that is the (boundary|constraint|point)|the whole point|not (merely|so much)' \
		/tmp/.cr-text.$$ | sed 's/^[0-9]*://'

	echo "-- UPPERCASE words (a parameter name in prose, or a real macro)"
	grep -oE '(^|[^A-Za-z_])[A-Z][A-Z0-9_]+([^A-Za-z0-9_]|$)' /tmp/.cr-text.$$ |
		tr -cd 'A-Z0-9_\n' | sort -u | tr '\n' ' '
	echo

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
	}' /tmp/.cr-text.$$

	# A body remark takes /* */ only when it runs to several paragraphs. A doc
	# comment written /* */ is the worse finding, and it is above a declaration.
	echo "-- /* */ blocks with no blank comment line (a doc comment here is a finding)"
	# A /** */ banner is a doc comment and is exempt.
	awk '
		/^[[:space:]]*\/\*\*/ { inb = 0; next }
		/^[[:space:]]*\/\*/ { start = FNR; blank = 0; inb = 1 }
		inb && /^[[:space:]]*\*[[:space:]]*$/ { blank = 1 }
		inb && /\*\// {
			if (!blank && FNR > start) print "  " start "-" FNR
			inb = 0
		}
	' "$f"

	rm -f /tmp/.cr-text.$$
done
