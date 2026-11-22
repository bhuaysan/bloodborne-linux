import tempfile
import unittest
from pathlib import Path

import rig
import thread_cpu as tc

S = 1_000_000_000


def stat_line(tid: int, name: str, utime: int, stime: int) -> str:
    # Fields after the name: state, ppid, pgrp, session, tty, tpgid, flags, minflt, cminflt, majflt,
    # cmajflt, utime, stime, ...
    return f"{tid} ({name}) S 1 1 1 0 -1 0 0 0 0 0 {utime} {stime} 0 0 20 0 1 0 0\n"


class ThreadTimesTest(unittest.TestCase):
    def test_reads_every_thread_of_every_process(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            proc = Path(d)
            for pid, threads in {"10": [(10, "bb-run", 5, 1), (11, "shadPS4:GpuComm", 900, 70)],
                                 "20": [(20, "odd) name, x", 3, 0)]}.items():
                for tid, name, u, s in threads:
                    task = proc / pid / "task" / str(tid)
                    task.mkdir(parents=True)
                    (task / "stat").write_text(stat_line(tid, name, u, s))
            for tid, raw in ((12, b"12 (bad\xff) S 1"), (13, b"")):  # cut short, gone
                (proc / "10" / "task" / str(tid)).mkdir()
                (proc / "10" / "task" / str(tid) / "stat").write_bytes(raw)
            times = sorted(rig.read_thread_times(["10", "20", "30"], proc))
        self.assertEqual(times, [(10, "bb-run", 5, 1), (11, "shadPS4:GpuComm", 900, 70),
                                 (20, "odd) name; x", 3, 0)])


class ThreadCpuTest(unittest.TestCase):
    def samples(self) -> dict:
        # GPU thread: 2 ticks a second user, 1 system; two workers 1 tick a second each.
        return {t * S: {1: ("shadPS4:GpuComm", 2 * t, t), 2: ("GXWorker:1", t, 0),
                        3: ("GXWorker:2", t, 0)} for t in range(0, 11)}

    def test_cpu_per_present_between_the_samples_at_the_marks(self) -> None:
        presents = [int(i * S / 30) for i in range(0, 300)]  # 30 a second for 10 s
        span, frames, threads = tc.segment_cpu(self.samples(), presents, 2 * S + 5, 6 * S + 5)
        self.assertEqual((span, frames), (4.0, 120))
        self.assertEqual(threads[0].name, "shadPS4:GpuComm")
        self.assertAlmostEqual(threads[0].cpu_ms, 12 * 10 / 120)  # 12 ticks of 10 ms
        self.assertAlmostEqual(threads[0].sys_ms, 4 * 10 / 120)
        self.assertEqual((threads[1].name, threads[1].threads), ("GXWorker", 2))
        self.assertAlmostEqual(threads[1].cpu_ms, 8 * 10 / 120)

    def test_threads_started_inside_the_span_and_empty_spans_are_left_out(self) -> None:
        samples = self.samples()
        samples[6 * S][9] = ("short", 50, 0)
        _, _, threads = tc.segment_cpu(samples, [3 * S], 2 * S, 6 * S)
        self.assertNotIn("short", [t.name for t in threads])
        self.assertEqual(tc.segment_cpu(samples, [], 2 * S, 6 * S)[1:], (0, []))
        self.assertEqual(tc.segment_cpu({S: {}}, [S], 0, 2 * S), (0.0, 0, []))

    def test_group_names(self) -> None:
        self.assertEqual(tc.group_name("GXWorker:3"), "GXWorker")
        self.assertEqual(tc.group_name("CSChrThread4"), "CSChrThread")
        self.assertEqual(tc.group_name("shadPS4:GpuComm"), "shadPS4:GpuComm")

    def test_report_of_a_run(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / "events.csv").write_text(
                "mono_ns,utc,log_bytes,kind,text\n"
                f"{2 * S},x,0,mark,play-stand\n{6 * S},x,0,mark,play-walk\n"
                f"{int(9.5 * S)},x,0,end,scenario finished\n")
            (run / "frames.csv").write_text(
                "frame,present_ns,return_ns,result,width,height,captured\n"
                + "".join(f"{i},{int(i * S / 30)},0,0,1,1,0\n" for i in range(300)))
            rows = ["mono_ns,tid,name,utime,stime"]
            samples = self.samples()
            samples[10 * S] = {}  # after the end: the target is gone
            for t, sample in samples.items():
                rows += [f"{t},{tid},{n},{u},{s}" for tid, (n, u, s) in sample.items()]
            (run / "threads.csv").write_text("\n".join(rows) + "\n12,trunc")
            text = tc.run_report(run, "play-*", "shadPS4:*")
        self.assertIn("play-stand: 4.0 s, 120 presents", text)
        self.assertIn("play-walk: 3.0 s, 90 presents", text)  # to the sample before the end
        self.assertIn("1.00 ( 0.33)  shadPS4:GpuComm", text)
        self.assertNotIn("GXWorker", text)

    def test_gpu_time_per_present(self) -> None:
        gpu = [(int(t * S / 10), 50 if t < 40 else 100) for t in range(100)]  # 10 a second
        presents = [int(i * S / 30) for i in range(300)]
        busy, ms = tc.segment_gpu(gpu, presents, 0, 4 * S)
        self.assertAlmostEqual(busy, 50)
        self.assertAlmostEqual(ms, 0.5 * 4000 / 120)
        busy, ms = tc.segment_gpu(gpu, presents, 2 * S, 6 * S)
        self.assertAlmostEqual(busy, 75)
        self.assertIsNone(tc.segment_gpu(gpu, [], 0, 4 * S))
        self.assertIsNone(tc.segment_gpu([], presents, 0, 4 * S))

    def test_gpu_line_in_the_report(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / "events.csv").write_text(
                "mono_ns,utc,log_bytes,kind,text\n"
                f"{2 * S},x,0,mark,play-stand\n{6 * S},x,0,end,scenario finished\n")
            (run / "frames.csv").write_text(
                "frame,present_ns,return_ns,result,width,height,captured\n"
                + "".join(f"{i},{int(i * S / 30)},0,0,1,1,0\n" for i in range(300)))
            (run / "threads.csv").write_text(
                "mono_ns,tid,name,utime,stime\n"
                + "".join(f"{t * S},1,shadPS4:GpuComm,{t},0\n" for t in range(8)))
            (run / "gpu.csv").write_text(
                "mono_ns,busy\n" + "".join(f"{int(t * S / 10)},60\n" for t in range(80))
                + "x,y\n")
            text = tc.run_report(run)
        self.assertIn("20.00          GPU (busy 60 %, whole GPU)", text)  # 0.6 * 4000 ms / 120

    def test_gpu_sampler_writes_the_busiest_card(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            drm = Path(d)
            for card, busy in (("card0", "7"), ("card1", "42")):
                (drm / card / "device").mkdir(parents=True)
                (drm / card / "device" / "gpu_busy_percent").write_text(busy + "\n")
            files = rig.gpu_busy_files(drm)
            self.assertEqual([f.parent.parent.name for f in files], ["card0", "card1"])
            sampler = rig.GpuSampler(drm / "gpu.csv", files, period=0.01)
            sampler.start()
            while not (drm / "gpu.csv").exists() or (drm / "gpu.csv").read_text().count("\n") < 3:
                sampler.stopping.wait(0.01)
            sampler.stopping.set()
            sampler.join()
            rows = tc.read_gpu(drm / "gpu.csv")
        self.assertGreaterEqual(len(rows), 2)
        self.assertEqual({b for _, b in rows}, {42})

    def test_runs_without_samples(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            self.assertIn("no threads.csv", tc.run_report(Path(d)))


if __name__ == "__main__":
    unittest.main()
