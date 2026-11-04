#!/bin/sh
# Merge a pull request locally so every commit keeps the repository-local
# identity. GitHub's merge button writes the account's display name into
# the history, so it must not be used.
#
# Usage: tools/merge-pr.sh <pr-number>
#
# Rebases the PR branch onto origin/main in a temporary worktree, checks
# authorship, updates the PR branch, fast-forwards main and, once GitHub
# shows the PR as merged, deletes the branch. The current checkout is
# never touched.
set -eu

[ $# -eq 1 ] || { echo "usage: $0 <pr-number>" >&2; exit 2; }
pr="$1"

repo_root="$(git rev-parse --show-toplevel)"
want_name="$(git -C "$repo_root" config user.name)"
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
cleanup() { git -C "$repo_root" worktree remove --force "$wt" 2>/dev/null || true; rm -rf "$wt"; }
trap cleanup EXIT

git -C "$repo_root" worktree add -q --detach "$wt" "origin/$head"
cd "$wt"
git rebase -q origin/main

# every commit on top of main must carry the local identity
bad="$(git log --format='%an <%ae>%n%cn <%ce>' origin/main..HEAD | sort -u | grep -vxF "$want_name <$want_email>" || true)"
if [ -n "$bad" ]; then
    echo "commits with an unexpected identity:" >&2
    printf '%s\n' "$bad" >&2
    exit 1
fi

count="$(git rev-list --count origin/main..HEAD)"
[ "$count" -gt 0 ] || { echo "nothing to merge" >&2; exit 1; }
echo "merging PR #$pr ($count commits from $head)"
git log --oneline origin/main..HEAD

new="$(git rev-parse HEAD)"
git push -q --force-with-lease="$head:origin/$head" origin "$new:refs/heads/$head"
git push -q origin "$new:refs/heads/main"

# GitHub marks the PR as merged only after it sees the head on main.
# Deleting the branch before that can leave the PR closed instead.
merged=no
for _ in $(seq 30); do
    if [ "$(gh pr view "$pr" --json state --jq .state)" = "MERGED" ]; then
        merged=yes
        break
    fi
    sleep 2
done
if [ "$merged" = yes ]; then
    git push -q origin --delete "$head"
else
    echo "warning: PR #$pr is not shown as merged yet; keeping branch $head" >&2
fi

git -C "$repo_root" fetch -q --prune origin
echo "done: main is now $(git rev-parse --short "$new")"
