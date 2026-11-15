import tempfile
import unittest
from pathlib import Path

import frame_profile as prof
import report


def line(at: int, interval: int, **parts: tuple[int, int]) -> str:
    text = "".join(f", {p}={us} us/{n}" for p, (us, n) in parts.items())
    return (f"13:00:00.000 debug [shadPS4:GpuCommandProcessor] profile: "
            f"Frame profile: at {at} ns, {interval} us since the last{text}\n")


class ProfileTest(unittest.TestCase):
    def test_reads_lines_with_and_without_parts(self) -> None:
        log = line(100, 0) + "noise\n" + line(200, 33000, upload=(2500, 3), fault=(7, 2))
        frames = prof.read_profile(log)
        self.assertEqual([f.at_ns for f in frames], [100, 200])
        self.assertEqual(frames[1].us, {"upload": 2500, "fault": 7})
        self.assertEqual(frames[1].calls["upload"], 3)

    def test_slow_frames_name_the_part_that_grew(self) -> None:
        frames = [prof.GpuFrame(i * 33_000_000, 33_333, {"upload": 100, "draw": 5000})
                  for i in range(1, 30)]
        frames += [prof.GpuFrame(10**9 + i, 50_000, {"upload": 9000, "draw": 5000})
                   for i in range(2)]
        result = prof.analyse(frames)
        self.assertEqual(result["slow"], 2)
        ranked = list(result["excess_us"])
        self.assertEqual(ranked[0], "interval")
        self.assertEqual(ranked[1], "upload")
        self.assertAlmostEqual(result["excess_us"]["draw"], 0)
        self.assertEqual(result["slowest"][0].interval_us, 50_000)

    def test_upload_bursts_say_how_many_images_were_new(self) -> None:
        frames = [prof.GpuFrame(i * 33_000_000, 33_333, {"upload": 100}, {"upload": 5})
                  for i in range(1, 30)]
        frames.append(prof.GpuFrame(10**9, 33_333, {"upload": 9000, "upload_new": 8000},
                                    {"upload": 176, "upload_new": 172}))
        result = prof.analyse(frames)
        self.assertEqual(result["upload_bursts"], [(176, 172)])
        self.assertIn("frames with over 50 image refreshes: 176 (172 new images)",
                      prof.format_segment("s", result))

    def test_builds_overlapping_a_frame_are_added(self) -> None:
        frame = prof.GpuFrame(100_000_000, 33_000)  # spans 67 ms .. 100 ms
        prof.add_builds([frame], [report.Build(90_000_000, 20_000_000),  # 10 ms inside
                                  report.Build(0, 1_000_000)])           # outside
        self.assertEqual(frame.us["build"], 10_000)

    def test_late_presents_are_matched_to_the_gpu_frame_before_them(self) -> None:
        ms = 1_000_000
        frames = [prof.GpuFrame(i * 33 * ms, 33_000, {"commands": 10_000}) for i in range(1, 12)]
        presents = [report.Frame(i * 33 * ms + 3 * ms, False) for i in range(1, 6)]
        presents += [report.Frame(6 * 33 * ms + 20 * ms + 33 * ms, False)]  # late by 20 ms
        presents += [report.Frame(i * 33 * ms + 3 * ms, False) for i in range(8, 12)]
        late = prof.late_presents(frames, presents)
        self.assertEqual(late["late"], 1)
        self.assertAlmostEqual(late["on_time_lag_ms"], 3, places=1)
        self.assertAlmostEqual(late["lag_ms"], 20, places=1)
        self.assertIsNone(prof.late_presents(frames, presents[:5]))

    def test_too_few_frames(self) -> None:
        self.assertIsNone(prof.analyse([prof.GpuFrame(1, 33_000)] * 3))

    def test_segments_split_at_marks_and_no_lines_is_said(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / "events.csv").write_text("mono_ns,utc,log_bytes,kind,text\n"
                                            "0,x,0,mark,a\n1000000000,x,0,mark,b\n")
            (run / "target.log").write_text("nothing\n")
            self.assertIn("no `Frame profile` lines", prof.profile_run(run))
            body = "".join(line(i * 33_000_000, 33_000, draw=(10, 1)) for i in range(1, 40))
            (run / "target.log").write_text(body)
            text = prof.profile_run(run, "a")
            self.assertTrue(text.startswith("a: 30 frames"))
            self.assertNotIn("b:", text)


if __name__ == "__main__":
    unittest.main()
