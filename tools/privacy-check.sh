#!/bin/sh
# Checks the commits on a branch before they are pushed or merged:
# authorship, agent and game files, file sizes, personal data in added
# lines and commit messages, and AI attribution.
#
# Usage: tools/privacy-check.sh [<base>]   (default: origin/main)
# Checks every commit in <base>..HEAD and the file list of HEAD. Exit status 1 on any
# finding.
set -eu
. "$(dirname "$0")/githooks/privacy-lib.sh"
FAILED=0
base="${1:-origin/main}"
git rev-parse -q --verify "$base^{commit}" >/dev/null ||
    { echo "unknown base: $base" >&2; exit 2; }
range="$base..HEAD"

# 1. identity: author and committer of every commit
want_name="$(git config user.name)"
want_email="$(git config user.email)"
case "$want_email" in
    *@users.noreply.github.com) ;;
    *) fail "user.email must be a GitHub noreply address" ;;
esac
bad="$(git log --format='%an <%ae>%n%cn <%ce>' "$range" | sort -u | grep -vxF "$want_name <$want_email>" || true)"
[ -z "$bad" ] || fail "commits with an unexpected identity: $(printf '%s' "$bad" | tr '\n' ' ')"

# 2. files: forbidden paths in HEAD or added by any commit of the range
# (a file deleted again is still in history), size of every new blob
problems="$( { git ls-tree -r --name-only HEAD
               git log --format= --name-only --diff-filter=AR "$range"; } |
    sort -u | while IFS= read -r f; do path_problem "$f"; done)"
[ -z "$problems" ] || fail "$problems"
big="$(git rev-list --objects "$range" |
    git cat-file --batch-check='%(objecttype) %(objectsize) %(rest)' |
    while read -r type size path; do
        [ "$type" = blob ] || continue
        [ "$size" -le "$(size_limit "$path")" ] || echo "file too large ($size bytes): $path"
    done)"
[ -z "$big" ] || fail "$big"

# 3. added lines and new file names of every commit, as
#    "<commit> <file>: <line>" so a hit points at what to fix
pat="$(privacy_pattern)"
hits="$(git log -p -U0 --no-color --format='commit %h' "$range" | awk '
    /^commit [0-9a-f]+$/ { c = $2; next }
    /^diff --git / { h = 0; f = $0; sub(/^diff --git a\/.* b\//, "", f); print c " " f ": (file name)"; next }
    /^@@/ { h = 1; next }
    h && /^\+/ { print c " " f ": " substr($0, 2) }' |
    grep -E -i -- "$pat" || true)"
if [ -n "$hits" ]; then
    fail "added lines or file names contain personal data or absolute paths:"
    printf '%s\n' "$hits" | head -20 >&2
fi

# 4. commit messages
msgs="$(git log --format=%B "$range")"
printf '%s\n' "$msgs" | grep -qE -i -- "$pat" &&
    fail "a commit message contains personal data or absolute paths"
printf '%s\n' "$msgs" | grep -qE -i -- "$AI_ATTRIBUTION_PATTERN" &&
    fail "a commit message contains AI attribution"

[ "$FAILED" -eq 0 ] || exit 1
echo "privacy check clean: $range"
