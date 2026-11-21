"""Checks that the distribution package lists, the docs and the CMake pins agree.

tools/distro-build.sh installs tools/distro-packages/<distro>.txt in a container, and
docs/distributions.md shows the same install line. The pins of cmake/headers.cmake are logged in
docs/references.md. Needs no container.
"""

import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DISTROS = ["fedora", "ubuntu", "debian", "arch"]


def read(relative: str) -> str:
    return (REPO / relative).read_text(encoding="utf-8")


def install_line(distro: str) -> str:
    out = subprocess.run(
        [str(REPO / "tools" / "distro-build.sh"), "--install-line", distro],
        check=True, capture_output=True, text=True)
    return out.stdout.strip()


class DistroPackages(unittest.TestCase):
    def test_every_distribution_has_a_list_and_a_script_entry(self):
        listed = sorted(p.stem for p in (REPO / "tools" / "distro-packages").glob("*.txt"))
        self.assertEqual(listed, sorted(DISTROS))

    def test_lists_have_no_duplicates_or_stray_words(self):
        for distro in DISTROS:
            names = [w for w in install_line(distro).split()[2:] if not w.startswith("-")]
            names = [w for w in names if w not in ("install", "pacman")]
            self.assertTrue(names, distro)
            self.assertEqual(len(names), len(set(names)), f"duplicate package in {distro}")

    def test_docs_show_exactly_the_installed_packages(self):
        docs = read("docs/distributions.md")
        for distro in DISTROS:
            self.assertIn(f"\n{install_line(distro)}\n", docs, distro)

    def test_the_readme_links_the_distribution_page(self):
        self.assertIn("docs/distributions.md", read("README.md"))

    def test_fetched_pins_are_logged_in_the_references(self):
        references = read("docs/references.md")
        for cmake in ("cmake/headers.cmake", "cmake/sdl3.cmake"):
            hashes = re.findall(r"URL_HASH SHA256=([0-9a-f]{64})", read(cmake))
            self.assertTrue(hashes, cmake)
            for digest in hashes:
                self.assertIn(digest, references, f"{cmake}: {digest} is not in docs/references.md")


if __name__ == "__main__":
    unittest.main()
