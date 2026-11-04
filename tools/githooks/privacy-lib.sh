# Shared privacy checks for the git hooks. POSIX sh.
#
# Generic patterns live here. Personal patterns (real names, email
# addresses) belong in .git/info/privacy-patterns, one extended regex
# per line. That file lives inside .git and is never committed.

# Brackets keep these patterns from matching this file itself.
GENERIC_PATTERNS='/hom[e]/[A-Za-z0-9_.-]+|/User[s]/[A-Za-z0-9_.-]+|[A-Za-z]:\\User[s]\\'

privacy_pattern() {
    local_file="$(git rev-parse --git-dir)/info/privacy-patterns"
    pat="$GENERIC_PATTERNS"
    if [ -f "$local_file" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in ''|'#'*) continue ;; esac
            pat="$pat|$line"
        done < "$local_file"
    fi
    printf '%s' "$pat"
}

fail() {
    printf 'privacy check: %s\n' "$1" >&2
    FAILED=1
}
