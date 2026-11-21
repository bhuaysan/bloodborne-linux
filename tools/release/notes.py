#!/usr/bin/env python3
"""Writes the notes of a GitHub release (docs/releasing.md).

    tools/release/notes.py vX.Y.Z [--at REF] [--repo OWNER/NAME] > notes.md

The notes are the release's highlights, docs/releases/<tag>.md (written by hand before the tag),
then the PRs merged since the previous release tag, grouped by their branch: feat/ under
"Features", fix/ under "Fixes", the rest under "Other changes". The first release has no list.

--at previews the notes before the tag exists, for the commit REF (for example origin/main).
Needs git with the whole history and, after the first release, gh (GH_TOKEN in CI).
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections.abc import Callable, Iterable
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
GROUPS = (("feat/", "Features"), ("fix/", "Fixes"), ("", "Other changes"))


class NotesError(Exception):
    pass


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout.strip()


def is_ancestor(commit: str, of: str) -> bool:
    """Whether commit is in the history of of; False for a commit git doesn't have."""
    return subprocess.run(["git", "merge-base", "--is-ancestor", commit, of], cwd=ROOT,
                          capture_output=True).returncode == 0


def previous_tag(at: str) -> str | None:
    """The newest release tag before the commit at, None for the first release."""
    try:
        return git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", f"{at}^")
    except subprocess.CalledProcessError:
        return None


def merged_prs(repo: str, since: str) -> list[dict]:
    """PRs into main merged on or after the date since (YYYY-MM-DD)."""
    out = subprocess.run(
        ["gh", "pr", "list", "--repo", repo, "--state", "merged", "--base", "main",
         "--limit", "1000", "--search", f"merged:>={since}",
         "--json", "number,title,headRefName,headRefOid"],
        cwd=ROOT, check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def released_prs(prs: Iterable[dict], at: str, previous: str,
                 ancestor: Callable[[str, str], bool] = is_ancestor) -> list[dict]:
    """The PRs whose head landed after previous and up to the commit at, by number.

    tools/merge-pr.sh moves the PR branch to the rebased head before it fast-forwards main, so
    the head a merged PR shows is the commit on main.
    """
    chosen = [pr for pr in prs
              if ancestor(pr["headRefOid"], at) and not ancestor(pr["headRefOid"], previous)]
    return sorted(chosen, key=lambda pr: pr["number"])


def render(highlights: str, prs: list[dict], repo: str, tag: str, previous: str | None) -> str:
    parts = [highlights.strip()]
    if previous is None:
        parts.append("The first release.")
    else:
        sections = []
        rest = list(prs)
        for prefix, title in GROUPS:
            group = [pr for pr in rest if pr["headRefName"].startswith(prefix)]
            rest = [pr for pr in rest if pr not in group]
            if group:
                lines = "\n".join(f"- {pr['title']} (#{pr['number']})" for pr in group)
                sections.append(f"### {title}\n\n{lines}")
        parts.append("## Changes\n\n" + ("\n\n".join(sections) if sections else "No merged PRs."))
        parts.append(f"**Full changelog**: https://github.com/{repo}/compare/{previous}...{tag}")
    return "\n\n".join(parts) + "\n"


def notes(tag: str, repo: str, at: str | None = None) -> str:
    if not TAG.fullmatch(tag):
        raise NotesError(f"{tag} is not a release tag vMAJOR.MINOR.PATCH")
    highlights_file = ROOT / "docs" / "releases" / f"{tag}.md"
    if not highlights_file.is_file():
        raise NotesError(f"docs/releases/{tag}.md is missing: write the highlights first")
    at = at or tag
    previous = previous_tag(at)
    prs: list[dict] = []
    if previous is not None:
        since = git("log", "-1", "--format=%cs", previous)
        prs = released_prs(merged_prs(repo, since), at, previous)
    return render(highlights_file.read_text(), prs, repo, tag, previous)


def default_repo() -> str:
    url = git("remote", "get-url", "origin")
    match = re.search(r"github\.com[:/]([^/]+/[^/]+?)(\.git)?$", url)
    if not match:
        raise NotesError("origin is not a GitHub repository: pass --repo")
    return match.group(1)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("tag")
    parser.add_argument("--at", help="the commit to release, before the tag exists (default: the tag)")
    parser.add_argument("--repo", help="OWNER/NAME (default: from the origin remote)")
    args = parser.parse_args(argv)
    try:
        sys.stdout.write(notes(args.tag, args.repo or default_repo(), args.at))
    except (NotesError, subprocess.CalledProcessError) as e:
        detail = getattr(e, "stderr", None) or ""
        print(f"notes.py: {e} {detail}".strip(), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
