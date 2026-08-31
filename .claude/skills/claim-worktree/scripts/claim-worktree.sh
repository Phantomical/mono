#!/bin/bash
#
# claim-worktree.sh - atomically claim an existing worktree for one agent
# and one task, so a second agent picking the same worktree fails closed
# instead of racing it. SKILL.md one directory up is the procedure; this
# header is why the claim is a file rather than a look at the tree.
#
# Nothing inside a worktree records who is using it. Mtimes, `git status` and
# a merged branch describe the tree, and a tree that is clean, merged and
# quiet for hours can have been branched by a peer seconds ago; the observed
# gap between one agent's check and another's checkout was 42 seconds. So the
# claim is written where a peer's next command reads it, and taking a tree is
# an exclusive create rather than a check followed by a checkout.
#
# The claim lives at <worktree>/.claude/claim.md. `.claude/` is excluded via
# .git/info/exclude, so this file is invisible to git and cannot collide with
# anything tracked. Exclusivity comes from opening claim.md with
# O_CREAT|O_EXCL (bash's `set -o noclobber`), a single syscall that either
# creates the file or fails - there is no separate check-then-act window for
# a second agent to land in.
#
# `.claude/` is itself untracked, so a fresh worktree has no copy of this
# script (see init-submodules.sh). Invoke the main tree's copy and pass the
# worktree explicitly, exactly as init-submodules.sh's own header says to:
#
#   /home/swlynch/projects/mono/.claude/skills/claim-worktree/scripts/claim-worktree.sh \
#       <worktree> --agent <name> --task "<description>"
#
# Usage: claim-worktree.sh <worktree-path> [options]
#
#   --agent <name>   Identify the claiming agent. Defaults to
#                     $CLAUDE_CODE_SESSION_ID, then $AI_AGENT, then
#                     "<user>@<host>:<pid>".
#   --task <text>    What the agent is using the worktree for. Defaults to
#                     "(none)".
#   --status         Print the current claim (or "unclaimed") and exit; makes
#                     no change.
#   --update         Refresh your own claim's task/timestamp in place. Fails
#                     if the existing claim's agent does not match --agent,
#                     unless --force is also given.
#   --release        Remove the claim. Fails if the existing claim's agent
#                     does not match --agent, unless --force is also given.
#   --force          With --update or --release, act even if the existing
#                     claim belongs to a different agent. Never implied.
#
# Exit status: 0 on success. 1 if the worktree is already claimed by another
# agent (claim/update/release without --force) or unclaimed (release without
# --force is then a no-op success; update/release still need a claim to act
# on and report "nothing to <verb>" as an error). 2 on usage error.

set -u

usage () {
	sed -n '3,/^$/p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-2}"
}

AGENT=""
TASK="(none)"
MODE="claim"
FORCE=""
WORKTREE=""

while [ $# -gt 0 ]; do
	case "$1" in
		--agent)
			shift
			[ $# -gt 0 ] || { echo "claim-worktree: --agent needs a value" >&2; exit 2; }
			AGENT="$1"
			;;
		--agent=*) AGENT="${1#--agent=}" ;;
		--task)
			shift
			[ $# -gt 0 ] || { echo "claim-worktree: --task needs a value" >&2; exit 2; }
			TASK="$1"
			;;
		--task=*) TASK="${1#--task=}" ;;
		--status) MODE="status" ;;
		--update) MODE="update" ;;
		--release) MODE="release" ;;
		--force) FORCE=1 ;;
		-h|--help) usage 0 ;;
		--) shift; break ;;
		-*) echo "claim-worktree: unknown option: $1" >&2; exit 2 ;;
		*)
			if [ -n "$WORKTREE" ]; then
				echo "claim-worktree: unexpected argument: $1" >&2
				exit 2
			fi
			WORKTREE="$1"
			;;
	esac
	shift
done

[ -n "$WORKTREE" ] || usage 2
[ -d "$WORKTREE" ] || { echo "claim-worktree: no such directory: $WORKTREE" >&2; exit 2; }
[ -e "$WORKTREE/.git" ] || { echo "claim-worktree: $WORKTREE has no .git - not a worktree" >&2; exit 2; }

WORKTREE=$(cd "$WORKTREE" && pwd) || exit 2
CLAIM="$WORKTREE/.claude/claim.md"

if [ -z "$AGENT" ]; then
	AGENT="${CLAUDE_CODE_SESSION_ID:-${AI_AGENT:-"$(whoami)@$(hostname):$$"}}"
fi

# Pull one field out of an existing claim.md by its label, e.g. "agent" from
# "- **agent:** foo". Empty if the file or the field is missing.
claim_field () {
	[ -f "$CLAIM" ] || return 0
	sed -n "s/^- \*\*$1:\*\* //p" "$CLAIM" | head -n1
}

print_claim () {
	if [ -f "$CLAIM" ]; then
		cat "$CLAIM"
	else
		echo "unclaimed: $WORKTREE"
	fi
}

if [ "$MODE" = "status" ]; then
	print_claim
	exit 0
fi

if [ "$MODE" = "update" ] || [ "$MODE" = "release" ]; then
	if [ ! -f "$CLAIM" ]; then
		echo "claim-worktree: nothing to $MODE - $WORKTREE is unclaimed" >&2
		exit 1
	fi
	OWNER=$(claim_field agent)
	if [ "$OWNER" != "$AGENT" ] && [ -z "$FORCE" ]; then
		echo "claim-worktree: $WORKTREE is claimed by '$OWNER', not '$AGENT' - pass --force to override" >&2
		print_claim >&2
		exit 1
	fi

	if [ "$MODE" = "release" ]; then
		rm -f "$CLAIM"
		echo "claim-worktree: released $WORKTREE (was: $OWNER)"
		exit 0
	fi
	# --update falls through to the write below, reusing the same content
	# format as a fresh claim; it does not need O_EXCL because only the
	# verified owner (or an explicit --force) reaches this point.
fi

write_claim () {
	# $1: destination fd, already open for writing.
	cat >&"$1" <<EOF
# Worktree claim

- **agent:** $AGENT
- **task:** $TASK
- **claimed_at:** $(date -u +%Y-%m-%dT%H:%M:%SZ)
- **host:** $(hostname)
- **pid:** $$
- **branch:** $(git -C "$WORKTREE" symbolic-ref -q --short HEAD || git -C "$WORKTREE" rev-parse --short HEAD 2>/dev/null || echo "(unknown)")
EOF
}

mkdir -p "$WORKTREE/.claude" || { echo "claim-worktree: cannot create $WORKTREE/.claude" >&2; exit 2; }

if [ "$MODE" = "update" ]; then
	write_claim 1 > "$CLAIM"
	echo "claim-worktree: updated claim on $WORKTREE for '$AGENT'"
	exit 0
fi

# The exclusive open: this is the entire race window, and it is one syscall.
# `noclobber` makes `>` open O_CREAT|O_EXCL, so a second agent's `exec`
# against the same path fails here rather than after a separate check. It has
# to run in this shell, not a subshell, or fd 3 does not survive to the write
# below.
set -o noclobber
if ! exec 3>"$CLAIM" 2>/dev/null; then
	set +o noclobber
	echo "claim-worktree: $WORKTREE is already claimed" >&2
	print_claim >&2
	exit 1
fi
set +o noclobber

write_claim 3
exec 3>&-

echo "claim-worktree: claimed $WORKTREE for '$AGENT' (task: $TASK)"
