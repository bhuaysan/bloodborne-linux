#!/bin/sh
# Merge a pull request locally so every commit keeps the repository-local
# identity. GitHub's merge button writes the account's display name into
# the history, so it must not be used.
#
# Usage: tools/merge-pr.sh [--maintainer] <pr-number>
#
# Rebases the PR branch onto origin/main in a temporary worktree, runs
# tools/run-tests.sh and tools/privacy-check.sh (which also checks
# authorship) on the rebased commits, updates the PR branch, fast-forwards main and, once
# GitHub shows the PR as merged, deletes the branch. The current
# checkout is never touched.
#
# PRs that change the guardrails (see PROTECTED below) or exceed the size
# limit in AGENTS.md are refused unless --maintainer is given. Only the
# maintainer passes that flag.
#
# Exit status:
#   0  merged and branch deleted
#   3  main was pushed, but the merge was not confirmed or the branch
#      could not be deleted. The message says what to do next. Do not
#      re-run the script: main already has the commits.
#   any other non-zero status: main was not updated (2 for a usage error)
set -eu

maintainer=0
if [ "${1:-}" = "--maintainer" ]; then maintainer=1; shift; fi
[ $# -eq 1 ] || { echo "usage: $0 [--maintainer] <pr-number>" >&2; exit 2; }
pr="$1"

# Paths whose changes always go to the maintainer (pathspecs).
PROTECTED="AGENTS.md docs/clean-room.md .gitignore .github tools/githooks tools/merge-pr.sh tools/privacy-check.sh tools/run-tests.sh tools/common/toolenv.py"
MAX_CODE_LINES=500

repo_root="$(git rev-parse --show-toplevel)"
want_email="$(git -C "$repo_root" config user.email)"
case "$want_email" in
    *@users.noreply.github.com) ;;
    *) echo "user.email is not a GitHub noreply address" >&2; exit 1 ;;
esac

state="$(gh pr view "$pr" --json state --jq .state)"
base="$(gh pr view "$pr" --json baseRefName --jq .baseRefName)"
head="$(gh pr view "$pr" --json headRefName --jq .headRefName)"
[ "$state" = "OPEN" ] || { echo "PR #$pr is $state" >&2; exit 1; }
[ "$base" = "main" ] || { echo "PR #$pr targets $base, expected main" >&2; exit 1; }

git -C "$repo_root" fetch -q origin main "$head"

wt="$(mktemp -d)"
gh_err="$(mktemp)"
cleanup() {
    git -C "$repo_root" worktree remove --force "$wt" 2>/dev/null || true
    rm -rf "$wt" "$gh_err"
}
trap cleanup EXIT

git -C "$repo_root" worktree add -q --detach "$wt" "origin/$head"
cd "$wt"
git rebase -q origin/main

count="$(git rev-list --count origin/main..HEAD)"
[ "$count" -gt 0 ] || { echo "nothing to merge" >&2; exit 1; }

# shellcheck disable=SC2086  # PROTECTED is a list of pathspecs
touched="$(git diff --name-only origin/main HEAD -- $PROTECTED)"
# changed lines of code: everything except docs/ and symbols/
lines="$(git diff --numstat origin/main HEAD -- . ':!docs' ':!symbols' |
    awk '$1 != "-" { n += $1 + $2 } END { print n + 0 }')"
if [ "$maintainer" -eq 0 ]; then
    [ -z "$touched" ] || {
        echo "PR #$pr changes protected paths, the maintainer merges it:" >&2
        printf '%s\n' "$touched" >&2
        exit 1
    }
    [ "$lines" -le "$MAX_CODE_LINES" ] || {
        echo "PR #$pr changes $lines lines of code (limit $MAX_CODE_LINES), the maintainer merges it" >&2
        exit 1
    }
fi

# check the commits exactly as they will land on main
tools/run-tests.sh || { echo "tests fail on the rebased branch" >&2; exit 1; }
tools/privacy-check.sh origin/main ||
    { echo "privacy check fails on the rebased branch" >&2; exit 1; }
echo "merging PR #$pr ($count commits from $head)"
git log --oneline origin/main..HEAD

new="$(git rev-parse HEAD)"
short="$(git rev-parse --short "$new")"
git push -q --force-with-lease="$head:origin/$head" origin "$new:refs/heads/$head"
git push -q origin "$new:refs/heads/main"

# From here on main is updated, so every failure exits 3.
unconfirmed() {
    echo "error: $1" >&2
    echo "main is now $short; branch $head kept" >&2
    echo "$2" >&2
    exit 3
}
delete_hint="once PR #$pr shows as merged, run: git push origin --delete $head"
git -C "$repo_root" fetch -q origin main ||
    echo "warning: could not fetch origin/main" >&2

# GitHub marks the PR as merged only after it sees the head on main.
# Deleting the branch before that can leave the PR closed instead.
# gh errors are retried until the timeout, since they may be transient.
timeout=60
interval=2
waited=0
while :; do
    if now="$(gh pr view "$pr" --json state --jq .state 2>"$gh_err")"; then
        case "$now" in
            MERGED) break ;;
            OPEN) ;;
            *) unconfirmed "PR #$pr is $now after pushing main" "this needs the maintainer" ;;
        esac
    fi
    if [ "$waited" -ge "$timeout" ]; then
        [ ! -s "$gh_err" ] || { echo "last gh error:" >&2; cat "$gh_err" >&2; }
        unconfirmed "PR #$pr is still not shown as merged after $timeout s" "$delete_hint"
    fi
    sleep "$interval"
    waited=$((waited + interval))
done
git push -q origin --delete "$head" ||
    unconfirmed "PR #$pr is merged, but deleting branch $head failed" \
        "run: git push origin --delete $head"

git -C "$repo_root" fetch -q --prune origin ||
    echo "warning: could not fetch origin; run git fetch --prune" >&2
echo "done: PR #$pr merged, main is now $short"
