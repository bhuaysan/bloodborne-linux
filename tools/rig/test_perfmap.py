import re
import tempfile
import unittest
from pathlib import Path

import perfmap


class PerfMapTest(unittest.TestCase):
    def test_lines_are_at_the_load_base_and_end_at_the_next_function(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            csv_path = Path(d) / "f.csv"
            csv_path.write_text("address,name,signature,status,file\n"
                                "0x00001100,second,\"int second()\",replaced,x.cpp\n"
                                "0x00001000,first,\"int first()\",replaced,x.cpp\n")
            self.assertEqual(perfmap.perf_map_lines(csv_path),
                             ["10001000 100 first", "10001100 10000 second"])
            self.assertEqual(perfmap.perf_map_lines(csv_path, 0), ["1000 100 first",
                                                                     "1100 10000 second"])

    def test_load_base_matches_the_loader(self) -> None:
        header = Path(__file__).resolve().parents[2] / "src/loader/image.hpp"
        m = re.search(r"default_base\s*=\s*(0x[0-9a-fA-F]+)", header.read_text())
        self.assertIsNotNone(m)
        self.assertEqual(perfmap.LOAD_BASE, int(m.group(1), 16))

    def test_the_real_symbols_parse(self) -> None:
        lines = perfmap.perf_map_lines(perfmap.ROOT / "symbols" / "functions.csv")
        self.assertTrue(lines)
        self.assertTrue(all(len(line.split(" ", 2)) == 3 for line in lines))


if __name__ == "__main__":
    unittest.main()
