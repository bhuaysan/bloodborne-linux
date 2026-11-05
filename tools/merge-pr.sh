#!/bin/sh
# Merge a pull request locally so every commit keeps the repository-local
# identity. GitHub's merge button writes the account's display name into
# the history, so it must not be used.
#
# Usage: tools/merge-pr.sh [--maintainer] [--sha <commit>] <pr-number>
#
# --sha is the PR head commit the independent review saw. The merge is
# refused if the PR branch points anywhere else, e.g. after a late push.
# It is required unless --maintainer is given.
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
#   3  main was pushed (or may have been, if origin could not be
#      checked), but the merge was not confirmed or the branch could not
#      be deleted. The message says what to do next. Do not re-run the
#      script before checking origin/main.
#   any other non-zero status: main was not updated (for example 2 for
#      a usage error, 129/130/143 for a hangup, Ctrl-C or kill). Only
#      SIGKILL can't be handled: after a kill -9, check origin/main and
#      the PR branch before re-running.
set -eu
# No pathname expansion: the unquoted $PROTECTED below would otherwise be
# expanded against files in the PR's worktree, and a directory named like
# ":(glob)x" would replace the .gitattributes pathspec.
set -f
# These would make git ignore the :(glob) and :! magic in the pathspecs.
unset GIT_LITERAL_PATHSPECS GIT_GLOB_PATHSPECS GIT_NOGLOB_PATHSPECS GIT_ICASE_PATHSPECS

usage() { echo "usage: $0 [--maintainer] [--sha <commit>] <pr-number>" >&2; exit 2; }
maintainer=0
want_sha=
while [ $# -gt 1 ]; do
    case "$1" in
        --maintainer) maintainer=1; shift ;;
        --sha) [ $# -gt 2 ] || usage; want_sha="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[ $# -eq 1 ] || usage
pr="$1"
[ -n "$want_sha" ] || [ "$maintainer" -eq 1 ] ||
    { echo "--sha <commit> is required: the PR head the review saw" >&2; exit 2; }
# a commit id, not a ref name: "--sha origin/<branch>" would always match
case "$want_sha" in
    *[!0-9a-f]*) echo "--sha takes a commit id in hex" >&2; exit 2 ;;
esac
[ -z "$want_sha" ] || [ "${#want_sha}" -ge 7 ] ||
    { echo "--sha needs at least 7 hex digits" >&2; exit 2; }

# Paths whose changes always go to the maintainer (pathspecs).
# A .gitattributes can hide files from diffs, so any of them is protected.
PROTECTED="AGENTS.md docs/clean-room.md .gitignore :(glob)**/.gitattributes .github tools/githooks tools/merge-pr.sh tools/privacy-check.sh tools/run-tests.sh tools/common/toolenv.py"
MAX_CODE_LINES=500

repo_root="$(git rev-parse --show-toplevel)"
want_email="$(git -C "$repo_root" config user.email)"
case "$want_email" in
    *@users.noreply.github.com) ;;
    *) echo "user.email is not a GitHub noreply address" >&2; exit 1 ;;
esac

# One call, so all fields describe the same moment. Ref names can't contain
# tabs. An empty field would shift the ones after it, which leaves $fork
# not "false", so the merge is refused.
pr_info="$(gh pr view "$pr" --json state,baseRefName,headRefName,headRefOid,isCrossRepository \
    --jq '[.state, .baseRefName, .headRefName, .headRefOid, .isCrossRepository] | @tsv')"
tab="$(printf '\t')"
IFS="$tab" read -r state base head head_oid fork <<EOF
$pr_info
EOF
[ "$state" = "OPEN" ] || { echo "PR #$pr is $state" >&2; exit 1; }
[ "$base" = "main" ] || { echo "PR #$pr targets $base, expected main" >&2; exit 1; }
# for a fork, headRefName names the fork's branch, not one in origin
[ "$fork" = "false" ] || { echo "PR #$pr comes from a fork, the maintainer merges it" >&2; exit 1; }

git -C "$repo_root" fetch -q origin main "$head"
# Pin both commits now. Other sessions share the remote-tracking refs, and
# a fetch of theirs could move origin/* while the tests run.
fetched="$(git -C "$repo_root" rev-parse "origin/$head")"
main_base="$(git -C "$repo_root" rev-parse origin/main)"
[ "$fetched" = "$head_oid" ] ||
    { echo "origin/$head is $fetched, but GitHub shows $head_oid; try again" >&2; exit 1; }
# compared as a prefix: rev-parse would prefer a ref with a hex name
case "$fetched" in
    "$want_sha"*) ;;
    *) echo "PR #$pr head is $fetched, not the reviewed $want_sha; review the new commits first" >&2
       exit 1 ;;
esac

wt="$(mktemp -d)"
gh_err="$(mktemp)"
cleanup() {
    git -C "$repo_root" worktree remove --force "$wt" 2>/dev/null || true
    rm -rf "$wt" "$gh_err"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

git -C "$repo_root" worktree add -q --detach "$wt" "$fetched"
cd "$wt"
git rebase -q "$main_base"

count="$(git rev-list --count "$main_base..HEAD")"
[ "$count" -gt 0 ] || { echo "nothing to merge" >&2; exit 1; }

# shellcheck disable=SC2086  # PROTECTED is a list of pathspecs
touched="$(git diff --name-only "$main_base" HEAD -- $PROTECTED)"
# changed lines of code: everything except docs/, symbols/ and copied third_party/ code
# Counted from the patch with --text: --numstat shows a file marked -diff
# in a .gitattributes as "-" and so would not count it. Real binaries
# count by their line breaks, which sends large ones to the maintainer.
lines="$(git diff -U0 --text --no-textconv --no-ext-diff --no-color "$main_base" HEAD -- . ':!docs' ':!symbols' ':!third_party' |
    awk '/^diff --git / { h = 0; next } /^@@/ { h = 1; next } h && /^[-+]/ { n++ } END { print n + 0 }')"
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
tools/privacy-check.sh "$main_base" ||
    { echo "privacy check fails on the rebased branch" >&2; exit 1; }
echo "merging PR #$pr ($count commits from $head)"
git log --oneline "$main_base..HEAD"

new="$(git rev-parse HEAD)"
short="$(git rev-parse --short "$new")"
# $1: what went wrong, $2: next step, $3: state of main if not $short
unconfirmed() {
    set +e   # a failing echo (stderr gone after a hangup) must not skip exit 3
    echo "error: $1" >&2
    echo "${3:-main is now $short}; branch $head kept" >&2
    echo "$2" >&2
    exit 3
}
delete_hint="once PR #$pr shows as merged, run: git push origin --delete $head"

# Put the PR branch back to the reviewed commit, so a retry with the same
# --sha still matches. The lease makes this a no-op unless the branch is
# at the rebased commit.
restore_branch() {
    git push -q --force-with-lease="refs/heads/$head:$new" origin "$fetched:refs/heads/$head" 2>/dev/null ||
        [ "$(git ls-remote origin "refs/heads/$head" 2>/dev/null | cut -f1)" = "$fetched" ] ||
        echo "warning: branch $head is not at $fetched; check it before re-running" >&2
}

# The push of main failed or was interrupted. The remote may have taken it
# before the connection broke, and another merge may have landed on top
# since, so check whether origin's main contains the new commit.
# $1: exit status if main was not updated, $2: what happened.
main_push_failed() {
    unknown_hint="check origin/main: if it contains $short, $delete_hint; otherwise reset branch $head to $fetched and re-run"
    remote="$(git ls-remote origin refs/heads/main)" ||
        unconfirmed "$2, and origin/main could not be checked" "$unknown_hint" "origin/main is unknown"
    remote="${remote%%[[:space:]]*}"
    if [ "$remote" != "$new" ]; then
        git fetch -q origin "$remote" 2>/dev/null ||
            unconfirmed "$2, and origin/main could not be checked" "$unknown_hint" "origin/main is unknown"
        if ! git merge-base --is-ancestor "$new" "$remote"; then
            set +e   # keep the exit status below even if stderr is gone
            restore_branch
            echo "$2; main was not updated" >&2
            exit "$1"
        fi
    fi
    [ "$1" -eq 1 ] || unconfirmed "$2 after main was pushed" "$delete_hint"
    echo "warning: $2, but origin/main contains $short" >&2
}

# The lease is the pinned commit, not origin/$head, which may have moved.
trap 'restore_branch; exit 130' INT
trap 'restore_branch; exit 143' TERM
trap 'restore_branch; exit 129' HUP
git push -q --force-with-lease="refs/heads/$head:$fetched" origin "$new:refs/heads/$head" ||
    { restore_branch; echo "pushing branch $head failed; main was not updated" >&2; exit 1; }
trap 'main_push_failed 130 "interrupted while pushing main"' INT
trap 'main_push_failed 143 "terminated while pushing main"' TERM
trap 'main_push_failed 129 "hangup while pushing main"' HUP
git push -q origin "$new:refs/heads/main" || main_push_failed 1 "pushing main failed"

# From here on main is updated, so every failure exits 3.
trap 'unconfirmed "interrupted after pushing main" "$delete_hint"' INT TERM HUP
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
