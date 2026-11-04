#!/bin/sh
# Checks the commits on a branch before they are pushed or merged:
# authorship, agent and game files, file sizes, personal data in added
# lines and commit messages, and AI attribution.
#
# Usage: tools/privacy-check.sh [<base>]   (default: origin/main)
# Checks <base>..HEAD and the file list of HEAD. Exit status 1 on any
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

# 2. forbidden files anywhere in HEAD, sizes of files the branch changed
problems="$(git ls-tree -r --name-only HEAD | while IFS= read -r f; do path_problem "$f"; done)"
[ -z "$problems" ] || fail "$problems"
git diff --name-only --diff-filter=ACMR "$base" HEAD | while IFS= read -r f; do
    size="$(git cat-file -s "HEAD:$f")"
    [ "$size" -le "$(size_limit "$f")" ] || echo "file too large ($size bytes): $f"
done > "${TMPDIR:-/tmp}/bbl-privacy.$$"
if [ -s "${TMPDIR:-/tmp}/bbl-privacy.$$" ]; then
    while IFS= read -r msg; do fail "$msg"; done < "${TMPDIR:-/tmp}/bbl-privacy.$$"
fi
rm -f "${TMPDIR:-/tmp}/bbl-privacy.$$"

# 3. added lines of every commit (a leak removed later is still in history)
pat="$(privacy_pattern)"
hits="$(git log -p -U0 --no-color --format='commit %h' "$range" | grep -E '^\+[^+]' | grep -nE -i -- "$pat" || true)"
if [ -n "$hits" ]; then
    fail "added lines contain personal data or absolute paths:"
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
