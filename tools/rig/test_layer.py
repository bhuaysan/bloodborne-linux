#!/usr/bin/env python3
"""The Vulkan layer of the test rig builds without warnings and exports its entry point.

Run: python3 -m unittest discover -s tools/rig. It needs g++ and the Vulkan headers and skips
without them. How the layer behaves is checked by running it on the game through the rig.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


@unittest.skipUnless(shutil.which("g++") and Path("/usr/include/vulkan/vk_layer.h").exists(),
                     "needs g++ and the Vulkan headers")
class LayerBuildTests(unittest.TestCase):
    def test_builds_cleanly_and_exports_the_negotiate_call(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "layer.so"
            subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra",
                            "-Wno-missing-field-initializers", "-Werror", "-fPIC", "-shared",
                            "-fvisibility=hidden", "-o", str(binary),
                            str(HERE / "layer" / "bb_rig_layer.cpp")], check=True)
            symbols = subprocess.run(["nm", "-D", "--defined-only", str(binary)],
                                     capture_output=True, text=True, check=True).stdout
        self.assertIn("vkNegotiateLoaderLayerInterfaceVersion", symbols)


if __name__ == "__main__":
    unittest.main()
