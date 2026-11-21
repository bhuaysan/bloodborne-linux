# Releases

Players get the game from the [Releases page](https://github.com/bhuaysan/bloodborne-linux/releases): the portable package of `docs/design/launcher.md` ("One portable folder"), its `SHA256SUMS` and notes on what changed. The maintainer chose this on 2026-11-21, so that players don't have to build from source.

## Versions

Release tags are `vMAJOR.MINOR.PATCH` ([Semantic Versioning](https://semver.org/)):

- **MINOR** goes up with each completed milestone, or with a set of new features between milestones.
- **PATCH** goes up for a release that only fixes faults: a crash, a rendering fault, a broken option. Fixes that matter to players don't wait for the next milestone.
- **MAJOR** stays 0 until the game is playable from the start to an ending (R7). `v1.0.0` is that release.

The first release is `v0.1.0`, the state after R11. The milestone tags `m1` to `r11` stay as they are; from then on, the release replaces the milestone tag.

A release never breaks what a player keeps in `user/`: settings and saves of an older release load in a newer one. A config key that goes away is ignored, not an error.

Every build knows its version (`cmake/version.cmake`): `git describe` against the release tags, so `v0.2.0` on the tagged commit, `v0.2.0-3-gabc1234` three commits later and `dev-<commit>` before the first release, with `-dirty` for uncommitted changes. `bb-run --version` and `bb-launcher --version` print it, the launcher shows it in its corner and `bb-run` logs it first, so a bug report names the build. A build without git takes it from `-DBB_VERSION=`.

## Making a release

Releases come from `main`, which always passes its tests. There are no release branches.

1. **Highlights (agent, PR).** A PR `docs/release-vX.Y.Z` adds `docs/releases/vX.Y.Z.md`: a `## Highlights` heading and a few lines for players, written so they can follow them. It covers what's new, the fixes they would notice and known problems that remain. `tools/release/notes.py vX.Y.Z --at origin/main` previews the notes. The PR is Small and goes through the normal gate.
2. **Tag (maintainer).** After that PR is merged, the maintainer tags the commit on `main` and pushes the tag:

   ```sh
   git fetch origin
   git tag vX.Y.Z origin/main
   git push origin vX.Y.Z
   ```

   The tag is lightweight: an annotated tag would carry the tagger's name and address.
3. **Release (workflow).** `.github/workflows/release.yml` checks that the tag has the right form, is on `main` and has its highlights. It then builds the package with `tools/package-portable.sh` and checks that the programs report the tag as their version. That fails for a build that is not exactly the tagged commit. Finally it writes `SHA256SUMS` and publishes the release with the package, the checksums and the notes.

The notes (`tools/release/notes.py`) are the highlights, then every PR merged since the previous release, grouped by branch: `feat/` under Features, `fix/` under Fixes, the rest under Other changes. PR titles therefore have to read well on their own. A release with a wrong text is fixed by editing it on GitHub. A release with a wrong package is deleted together with its tag, and the next patch version is released.

Agents never create or push tags. "Tag for me" from the maintainer covers only the named tag.
