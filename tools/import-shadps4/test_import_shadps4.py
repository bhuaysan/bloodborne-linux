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
