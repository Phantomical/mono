---
name: claim-worktree
description: Claim a worktree under .claude/worktrees before working in it, so two agents on this box do not take the same tree. Use before taking an existing worktree, immediately after `git worktree add` for one you create, to check who holds a tree, and to release one when you are done with it.
---

# Claiming a worktree

Claim a worktree before you work in it. This is required, and it covers a tree you
created as much as one you found.

`.claude/` is untracked, so a worktree holds no copy of these scripts. Invoke the main
tree's copy by absolute path:

    S=/home/swlynch/projects/mono/.claude/skills/claim-worktree/scripts

`--agent` defaults to `$CLAUDE_CODE_SESSION_ID`, then `$AI_AGENT`, then
`<user>@<host>:<pid>`. Give the same `--agent` to every later command on that tree, or
they refuse to act on your own claim.

## Take a worktree from the pool

    "$S/claim-any-worktree.sh" --agent <id> --task "<what the tree is for>"

It prints the claimed path on stdout. Exit 1 means every tree in the pool is already
claimed: ask the user which tree to use, or create one.

Then run the checks under "Before you write in the tree" below.

## Claim a worktree you create

Claim it in the same step as `git worktree add`, not after the first build:

    git -C /home/swlynch/projects/mono worktree add \
        .claude/worktrees/<name> -b <branch> llvm18-tiered-jit
    "$S/claim-worktree.sh" /home/swlynch/projects/mono/.claude/worktrees/<name> \
        --agent <id> --task "<what the tree is for>"

An unclaimed tree is what `claim-any-worktree.sh` searches for, so a tree you are
building in is taken by the next agent that runs it.

## Claim a worktree the user named

    "$S/claim-worktree.sh" <worktree> --agent <id> --task "<what the tree is for>"

Exit 1 means a peer holds it. The command prints their claim; take another tree or ask
the user. Do not pass `--force`.

## Check who holds a tree

    "$S/claim-worktree.sh" <worktree> --status    # one tree's claim, or "unclaimed"
    "$S/claim-any-worktree.sh" --list             # every tree in the pool

Both are read-only.

## Refresh and release

    "$S/claim-worktree.sh" <worktree> --agent <id> --update   # new task text, new timestamp
    "$S/claim-worktree.sh" <worktree> --agent <id> --release  # done with the tree

Release when you finish with a tree. `--update` and `--release` refuse a claim held by
another agent unless `--force` is given; use `--force` only when the user tells you to
take a tree over.

## Before you write in the tree

A claim only covers agents that ran these scripts. Before `checkout -b`, an edit or a
build in a tree you did not create:

    git -C <tree> reflog -1 --date=iso        # a checkout minutes old means a peer is in it
    grep -rl <tree-name> .claude/scratch/     # another task's notes naming the tree

Either hit means release the claim and take a different tree.

## Before you commit

Run `git -C <tree> diff --stat` and read the **paths**. Files you never opened mean a
peer wrote in the tree after you claimed it. `git status --porcelain` does not answer
this.

If that happens:

1. Save your own files.
2. `git checkout --` only the paths that are yours to revert; diff them first, because
   not all of them are necessarily yours.
3. Leave the branch refs alone.
4. Rebuild the tree so its binary matches its source again.
5. Tell the user which ref now holds whose commit.
6. Void every measurement taken in that tree and re-run it in a clean one.
