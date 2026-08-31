#!/bin/bash
#
# claim-any-worktree.sh - find and atomically claim the first unclaimed
# worktree under a pool directory, instead of one named up front.
#
# Candidates are every worktree `git worktree list` reports under pool-dir,
# excluding the main tree itself (which never lives inside its own pool
# directory). They are tried in randomized order when `shuf` is available, so
# several agents racing this script at once do not all pile onto the same
# first candidate - order is an efficiency choice, not a correctness one,
# since each candidate is still claimed through claim-worktree.sh's
# O_CREAT|O_EXCL open (see claim-worktree.sh's own header). A candidate
# already holding a claim.md is skipped without attempting the exclusive open,
# since that would only fail; a candidate with none is attempted, and a
# failed attempt - a peer's claim landing between this script's listing and
# its attempt - just moves on to the next candidate rather than erroring out.
#
# Usage: claim-any-worktree.sh [pool-dir] [options]
#
#   pool-dir         Directory to scan for worktrees. Default: the main
#                     tree's `.claude/worktrees`, resolved via
#                     `git rev-parse --git-common-dir` so this works whether
#                     invoked from the main tree or from any worktree.
#   --agent <name>   Passed through to claim-worktree.sh.
#   --task <text>    Passed through to claim-worktree.sh.
#   --list           Print every candidate's claim state (or "unclaimed") and
#                     exit 0. Claims nothing - read-only, like
#                     claim-worktree.sh --status, which this calls per
#                     candidate.
#
# Exit status: 0 with the claimed worktree's path on stdout, on success (or
# with --list, after printing every candidate). 1 if every candidate under
# pool-dir already holds a claim. 2 on usage error, including an empty or
# missing pool-dir.

set -u

usage () {
	sed -n '3,/^$/p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-2}"
}

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 2
CLAIM_SCRIPT="$SCRIPT_DIR/claim-worktree.sh"
[ -x "$CLAIM_SCRIPT" ] || { echo "claim-any-worktree: missing $CLAIM_SCRIPT" >&2; exit 2; }

AGENT=""
TASK=""
LIST=""
POOL=""

while [ $# -gt 0 ]; do
	case "$1" in
		--agent)
			shift
			[ $# -gt 0 ] || { echo "claim-any-worktree: --agent needs a value" >&2; exit 2; }
			AGENT="$1"
			;;
		--agent=*) AGENT="${1#--agent=}" ;;
		--task)
			shift
			[ $# -gt 0 ] || { echo "claim-any-worktree: --task needs a value" >&2; exit 2; }
			TASK="$1"
			;;
		--task=*) TASK="${1#--task=}" ;;
		--list) LIST=1 ;;
		-h|--help) usage 0 ;;
		--) shift; break ;;
		-*) echo "claim-any-worktree: unknown option: $1" >&2; exit 2 ;;
		*)
			if [ -n "$POOL" ]; then
				echo "claim-any-worktree: unexpected argument: $1" >&2
				exit 2
			fi
			POOL="$1"
			;;
	esac
	shift
done

if [ -z "$POOL" ]; then
	COMMON=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null) \
		|| { echo "claim-any-worktree: not inside a git tree" >&2; exit 2; }
	POOL="$(dirname "$COMMON")/.claude/worktrees"
fi
[ -d "$POOL" ] || { echo "claim-any-worktree: no such directory: $POOL" >&2; exit 2; }
POOL=$(cd "$POOL" && pwd) || exit 2

# Every worktree path `git worktree list` knows about, restricted to the ones
# actually inside $POOL - which excludes the main tree, since main never
# lives inside its own .claude/worktrees.
mapfile -t CANDIDATES < <(
	git worktree list --porcelain | awk '/^worktree /{print $2}' \
		| while read -r p; do
			case "$p/" in
				"$POOL"/*) echo "$p" ;;
			esac
		done
)

if [ "${#CANDIDATES[@]}" -eq 0 ]; then
	echo "claim-any-worktree: no worktrees found under $POOL" >&2
	exit 1
fi

if command -v shuf >/dev/null 2>&1; then
	mapfile -t CANDIDATES < <(printf '%s\n' "${CANDIDATES[@]}" | shuf)
fi

if [ -n "$LIST" ]; then
	for c in "${CANDIDATES[@]}"; do
		"$CLAIM_SCRIPT" "$c" --status
		echo
	done
	exit 0
fi

CLAIM_ARGS=()
[ -n "$AGENT" ] && CLAIM_ARGS+=(--agent "$AGENT")
[ -n "$TASK" ] && CLAIM_ARGS+=(--task "$TASK")

TRIED=0
for c in "${CANDIDATES[@]}"; do
	# Skip without attempting the exclusive open when a claim is already
	# visible - cheap, and avoids a failure message for the common case of
	# a mostly-claimed pool.
	[ -f "$c/.claude/claim.md" ] && continue

	TRIED=$((TRIED + 1))
	if "$CLAIM_SCRIPT" "$c" "${CLAIM_ARGS[@]}" 2>/dev/null; then
		exit 0
	fi
	# Lost a race for $c between the listing above and this attempt - try
	# the next candidate instead of failing the whole search over it.
done

echo "claim-any-worktree: no unclaimed worktree under $POOL (${#CANDIDATES[@]} total, $TRIED attempted)" >&2
exit 1
