"""Tests for the changes import_shadps4.py makes to imported files."""

import tempfile
import unittest
from pathlib import Path

import import_shadps4 as tool


class AddNotice(unittest.TestCase):
    def test_goes_after_the_last_spdx_line(self):
        text = "// SPDX-FileCopyrightText: A\n// SPDX-License-Identifier: MIT\n\nint x;\n"
        lines = tool.add_notice(text, "common/a.h").split("\n")
        self.assertEqual(lines[0:2], text.split("\n")[0:2])
        self.assertEqual(lines[2], f"// Source: shadPS4 src/common/a.h @{tool.SHADPS4_COMMIT}")
        self.assertEqual(lines[3], f"// Changed for bloodborne-linux, {tool.NOTICE_DATE}")
        self.assertEqual(lines[4], "// License texts: third_party/shadps4/LICENSES/")
        self.assertEqual(lines[5:], ["", "int x;", ""])

    def test_keeps_crlf_line_endings(self):
        text = "// SPDX-License-Identifier: MIT\r\nint x;\r\n"
        result = tool.add_notice(text, "a.h")
        self.assertEqual(result.count("\r\n"), 5)
        self.assertEqual(result.count("\n"), 5)

    def test_refuses_a_file_without_spdx_lines(self):
        with self.assertRaises(tool.ImportError_):
            tool.add_notice("int x;\n", "a.h")


class NoticeMarkers(unittest.TestCase):
    def test_cmake_files_get_hash_comments(self):
        text = "# SPDX-FileCopyrightText: A\n# SPDX-License-Identifier: GPL-2.0-or-later\n\nset(a b)\n"
        lines = tool.add_notice(text, "video_core/x.txt").split("\n")
        self.assertEqual(lines[2], f"# Source: shadPS4 src/video_core/x.txt @{tool.SHADPS4_COMMIT}")
        self.assertEqual(lines[3], f"# Changed for bloodborne-linux, {tool.NOTICE_DATE}")

    def test_spdx_lines_with_two_spaces_are_found(self):
        text = "//  SPDX-License-Identifier: MIT\n\nint x;\n"
        self.assertTrue(tool.add_notice(text, "a.h").split("\n")[1].startswith("// Source:"))


class LicenseInFile(unittest.TestCase):
    def test_is_copied_byte_for_byte(self):
        name = next(iter(tool.LICENSE_IN_FILE))
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "a.h"
            source.write_bytes(b"// clang-format off\r\n// MIT text\n")
            tool.import_shadps4_file(source, name, Path(temp) / "out")
            self.assertEqual((Path(temp) / "out/src/shadps4" / name).read_bytes(), source.read_bytes())

    def test_a_tagged_file_makes_the_entry_stale(self):
        name = next(iter(tool.LICENSE_IN_FILE))
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "a.h"
            source.write_text("// SPDX-License-Identifier: MIT\n")
            with self.assertRaises(tool.ImportError_):
                tool.import_shadps4_file(source, name, Path(temp) / "out")

    def test_entries_are_allowed_licenses(self):
        for name, expression in tool.LICENSE_IN_FILE.items():
            self.assertTrue(tool.license_allowed(expression), name)


class CommonClosure(unittest.TestCase):
    def make(self, temp, files):
        for name, text in files.items():
            path = Path(temp) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def test_follows_includes_through_common_files(self):
        with tempfile.TemporaryDirectory() as temp:
            self.make(temp, {"video_core/a.cpp": '#include "common/b.h"\n#include "video_core/a.h"\n',
                             "common/b.h": '#include "c.h"\n', "common/c.h": "", "common/unused.h": "",
                             "video_core/a.h": ""})
            self.assertEqual(tool.common_closure(Path(temp), ["video_core/a.cpp", "video_core/a.h"]),
                             ["common/b.h", "common/c.h"])

    def test_skips_files_already_listed_and_host_provided_ones(self):
        with tempfile.TemporaryDirectory() as temp:
            host = "common/alignment.h"
            self.assertIn(host, tool.HOST_PROVIDED)
            self.make(temp, {"video_core/a.cpp": f'#include "common/b.h"\n#include "{host}"\n',
                             "common/b.h": "", host: ""})
            self.assertEqual(tool.common_closure(Path(temp), ["video_core/a.cpp", "common/b.h"]), [])


class Licenses(unittest.TestCase):
    def test_allowed_expressions(self):
        for expression in ("GPL-2.0-or-later", "MIT", "BSD-3-Clause AND GPL-2.0-or-later", "MIT OR Apache-2.0"):
            self.assertTrue(tool.license_allowed(expression), expression)

    def test_refused_expressions(self):
        for expression in ("CC0-1.0", "GPL-2.0-only", "MIT AND Apache-2.0", "OFL-1.1",
                           "MIT OR GPL-3.0 AND ISC"):
            self.assertFalse(tool.license_allowed(expression), expression)

    def test_a_file_under_a_refused_license_is_not_written(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "a.h"
            source.write_text("// SPDX-License-Identifier: CC0-1.0\n")
            with self.assertRaises(tool.ImportError_):
                tool.import_shadps4_file(source, "common/a.h", Path(temp) / "out")
            self.assertFalse((Path(temp) / "out").exists())

    def test_reads_the_expression_without_a_carriage_return(self):
        self.assertEqual(tool.spdx_expression("// SPDX-License-Identifier: MIT\r\n"), "MIT")


class Includes(unittest.TestCase):
    def test_rewrites_only_the_src_prefixed_include(self):
        text = '#include "src/common/types.h"\n#include "common/assert.h"\n'
        self.assertEqual(tool.rewrite_includes(text), '#include "common/types.h"\n#include "common/assert.h"\n')


class Scope(unittest.TestCase):
    def test_host_provided_names_are_not_imported(self):
        imported = set(tool.EXTRA_FILES) | set(tool.LICENSE_IN_FILE)
        self.assertEqual(imported & (tool.HOST_PROVIDED | tool.GENERATED_SHADER_HEADERS), set())

    def test_the_imgui_overlay_layers_are_host_provided(self):
        for name in ("imgui/friends_layer.h", "imgui/notifications_layer.h",
                     "imgui/invitation_prompt_layer.h", "imgui/shadnet_notifications_layer.h"):
            self.assertIn(name, tool.HOST_PROVIDED)

    def test_the_imgui_layer_base_class_is_imported(self):
        self.assertIn("imgui/imgui_layer.h", tool.IMGUI_FILES)
        self.assertNotIn("imgui/imgui_layer.h", tool.HOST_PROVIDED)

    def test_generated_shader_headers_are_listed_once_per_shader(self):
        self.assertEqual(len(tool.GENERATED_SHADER_HEADERS), 20)


class Dependencies(unittest.TestCase):
    def test_video_core_libraries_are_listed_with_their_license_files(self):
        found = {d.name: d for d in tool.dependencies()}
        expected = {"vma": "LICENSE.txt", "xxhash": "LICENSE", "robin-map": "LICENSE",
                    "stb": None}
        for name, license_file in expected.items():
            self.assertEqual(found[name].license_file, license_file, name)

    def test_only_stb_comes_from_shadps4_itself(self):
        self.assertEqual([d.name for d in tool.dependencies() if d.in_shadps4], ["stb"])

    def test_stb_takes_only_the_header_the_presenter_includes(self):
        stb = next(d for d in tool.dependencies() if d.name == "stb")
        self.assertEqual(stb.files, ("stb_image_write.h",))

    def test_every_dependency_directory_is_owned(self):
        for dep in tool.dependencies():
            self.assertIn(f"third_party/{dep.name}", tool.owned_roots())


class Compare(unittest.TestCase):
    def test_reports_missing_extra_and_changed_files(self):
        with tempfile.TemporaryDirectory() as temp:
            generated, checkout = Path(temp) / "g", Path(temp) / "c"
            for root, files in ((generated, {"same": "1", "changed": "a", "gone": "x"}),
                                (checkout, {"same": "1", "changed": "b", "extra": "y"})):
                (root / "src/shadps4").mkdir(parents=True)
                for name, content in files.items():
                    (root / "src/shadps4" / name).write_text(content)
            self.assertEqual(tool.compare(generated, checkout),
                             ["missing: src/shadps4/gone", "not generated: src/shadps4/extra",
                              "differs: src/shadps4/changed"])


if __name__ == "__main__":
    unittest.main()
