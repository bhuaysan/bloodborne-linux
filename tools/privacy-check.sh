#!/bin/sh
# Checks the commits on a branch before they are pushed or merged:
# authorship, agent and game files, file sizes, personal data in added
# lines and commit messages, AI attribution, and commit subject length.
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
check_identity
bad="$(git log --format='%an <%ae>%n%cn <%ce>' "$range" | sort -u | grep -vxF "$want_name <$want_email>" || true)"
[ -z "$bad" ] || fail "commits with an unexpected identity: $(printf '%s' "$bad" | tr '\n' ' ')"

# 2. files: forbidden paths in HEAD or added by any commit of the range
# (a file deleted again is still in history), size of every new blob
# -z: git quotes unusual names otherwise, and "d\303\274r/x.bin" would
# not match *.bin
problems="$( { git ls-tree -r -z --name-only HEAD
               git log -z --format= --name-only --diff-filter=AR "$range"; } |
    tr '\0' '\n' | sort -u | while IFS= read -r f; do path_problem "$f"; done)"
[ -z "$problems" ] || fail "$problems"
big="$(git rev-list --objects "$range" |
    git cat-file --batch-check='%(objecttype) %(objectsize) %(rest)' |
    # split by hand: read would strip trailing spaces from the path, and
    # "symbols/a.csv " would get the larger symbols/*.csv limit
    while IFS= read -r line; do
        type="${line%% *}"; line="${line#* }"
        size="${line%% *}"; path="${line#* }"
        [ "$type" = blob ] || continue
        [ "$size" -le "$(size_limit "$path")" ] || echo "file too large ($size bytes): $path"
    done)"
[ -z "$big" ] || fail "$big"

# 3. added lines and new file names of every commit, as
#    "<commit> <file>: <line>" so a hit points at what to fix
pat="$(privacy_pattern)"
# --text and no textconv: a .gitattributes must not hide content as binary
hits="$(git -c core.quotePath=false log -p -U0 --text --no-textconv --no-ext-diff \
        --no-color --format='commit %h' "$range" | awk '
    /^commit [0-9a-f]+$/ { c = $2; next }
    /^diff --git / { h = 0; f = $0; sub(/^diff --git a\/.* b\//, "", f); print c " " f ": (file name)"; next }
    /^@@/ { h = 1; next }
    h && /^\+/ { print c " " f ": " substr($0, 2) }' |
    grep -a -E -i -- "$pat" || true)"
if [ -n "$hits" ]; then
    fail "added lines or file names contain personal data or absolute paths:"
    printf '%s\n' "$hits" | head -20 >&2
fi

# 4. commit messages
# an assignment, so set -e stops the check if git log can't walk the range
msgs="$(git log --format=%B "$range")"
check_message "$msgs" "a commit message"
for c in $(git rev-list "$range"); do
    check_subject "$(git log -1 --format=%s "$c")" final
done

[ "$FAILED" -eq 0 ] || exit 1
echo "privacy check clean: $range"
