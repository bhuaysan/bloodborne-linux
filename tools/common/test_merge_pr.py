"""Tests for the CI gate in tools/merge-pr.sh.

Each test runs the real script in a throwaway clone of a local bare
repository, with a fake `gh` on PATH that serves fixed check runs and
commit statuses (filtered through the script's own --jq expressions by
the real jq) and records PR comments. run-tests.sh and privacy-check.sh
are stubs in the clone, so a merge that passes the CI gate goes through.
"""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "merge-pr.sh"
BRANCH = "feat/1-x"

FAKE_GH = r'''#!/usr/bin/env python3
import json, os, subprocess, sys
d = os.environ["FAKE_GH_DIR"]
a = sys.argv[1:]

def jq(data, expr):
    out = subprocess.run(["jq", "-r", expr], input=json.dumps(data),
                         capture_output=True, text=True)
    if out.returncode:
        sys.stderr.write(out.stderr)
        sys.exit(1)
    sys.stdout.write(out.stdout)

def load(name):
    with open(os.path.join(d, name)) as f:
        return json.load(f)

if a[:2] == ["pr", "view"]:
    fields = a[a.index("--json") + 1]
    if fields == "state":
        print(open(os.path.join(d, "state_after")).read().strip())
    else:
        print("OPEN\tmain\t%s\t%s\tfalse" % (os.environ["FAKE_HEAD_REF"],
                                            os.environ["FAKE_HEAD_OID"]))
elif a[:2] == ["pr", "comment"]:
    with open(os.path.join(d, "comments.log"), "a") as f:
        f.write(sys.stdin.read())
elif a[0] == "api":
    path = [x for x in a[1:] if x.startswith("repos/")][0]
    if os.environ["FAKE_HEAD_OID"] not in path:
        sys.stderr.write("unexpected commit in " + path + "\n")
        sys.exit(1)
    expr = a[a.index("--jq") + 1]
    if "/check-runs" in path:
        jq({"total_count": 0, "check_runs": load("check_runs.json")}, expr)
    elif path.split("?")[0].endswith("/status"):
        jq({"state": "pending", "statuses": load("statuses.json")}, expr)
    else:
        sys.exit(1)
else:
    sys.stderr.write("fake gh: unexpected call %r\n" % a)
    sys.exit(1)
'''


def run(*args, cwd, env=None, check=True):
    return subprocess.run(args, cwd=cwd, env=env, check=check,
                          capture_output=True, text=True)


def check_run(name, conclusion, status="completed"):
    return {"name": name, "status": status, "conclusion": conclusion}


@unittest.skipUnless(shutil.which("jq") and shutil.which("git"),
                     "needs jq and git")
class MergePrCiGate(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="mergepr"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        git_env = {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith("GIT_")}
        self.env.update(git_env, TMPDIR=str(self.tmp))
        origin = self.tmp / "origin.git"
        self.clone = self.tmp / "clone"
        run("git", "init", "-q", "--bare", "-b", "main", str(origin),
            cwd=self.tmp, env=self.env)
        run("git", "init", "-q", "-b", "main", str(self.clone),
            cwd=self.tmp, env=self.env)
        c = self.clone
        for k, v in [("user.email", "1+x@users.noreply.github.com"),
                     ("user.name", "x"), ("commit.gpgsign", "false"),
                     ("core.hooksPath", "/dev/null")]:
            run("git", "config", k, v, cwd=c, env=self.env)
        run("git", "remote", "add", "origin", str(origin), cwd=c, env=self.env)
        (c / "tools").mkdir()
        shutil.copy(SCRIPT, c / "tools" / "merge-pr.sh")
        for stub in ("run-tests.sh", "privacy-check.sh"):
            (c / "tools" / stub).write_text("#!/bin/sh\nexit 0\n")
            (c / "tools" / stub).chmod(0o755)
        run("git", "add", "-A", cwd=c, env=self.env)
        run("git", "commit", "-qm", "base", cwd=c, env=self.env)
        run("git", "push", "-q", "origin", "main", cwd=c, env=self.env)
        run("git", "checkout", "-qb", BRANCH, cwd=c, env=self.env)
        (c / "docs").mkdir()
        (c / "docs" / "x.md").write_text("x\n")
        run("git", "add", "-A", cwd=c, env=self.env)
        run("git", "commit", "-qm", "Add x", cwd=c, env=self.env)
        run("git", "push", "-q", "origin", BRANCH, cwd=c, env=self.env)
        run("git", "checkout", "-q", "main", cwd=c, env=self.env)
        self.head = run("git", "rev-parse", BRANCH, cwd=c,
                        env=self.env).stdout.strip()
        self.origin = origin

        fake = self.tmp / "bin"
        fake.mkdir()
        (fake / "gh").write_text(FAKE_GH)
        (fake / "gh").chmod(0o755)
        self.data = self.tmp / "gh"
        self.data.mkdir()
        (self.data / "state_after").write_text("MERGED\n")
        self.env.update(PATH=f"{fake}:{self.env['PATH']}",
                        FAKE_GH_DIR=str(self.data), FAKE_HEAD_REF=BRANCH,
                        FAKE_HEAD_OID=self.head)
        self.set_ci([check_run("build (release)", "success")], [])

    def set_ci(self, check_runs, statuses):
        (self.data / "check_runs.json").write_text(json.dumps(check_runs))
        (self.data / "statuses.json").write_text(json.dumps(statuses))

    def merge(self, *flags):
        return run("sh", "tools/merge-pr.sh", *flags, "1", cwd=self.clone,
                   env=self.env, check=False)

    def origin_main(self):
        return run("git", "rev-parse", "main", cwd=self.origin,
                   env=self.env).stdout.strip()

    def comments(self):
        p = self.data / "comments.log"
        return p.read_text() if p.exists() else ""

    def assert_refused(self, r, *needles):
        self.assertEqual(r.returncode, 1, r.stderr)
        for n in needles:
            self.assertIn(n, r.stderr)
        self.assertNotEqual(self.origin_main(), self.head)
        self.assertEqual(self.comments(), "")

    def test_green_merges(self):
        self.set_ci([check_run("build (release)", "success"),
                     check_run("package", "skipped"),
                     check_run("lint", "neutral"),
                     # GitHub can leave a finished run in_progress
                     check_run("privacy", "success", status="in_progress")],
                    [{"context": "ext", "state": "success"}])
        r = self.merge("--sha", self.head[:12])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.origin_main(), self.head)
        self.assertEqual(self.comments(), "")

    def test_failed_check_refused(self):
        self.set_ci([check_run("build (debug)", "success"),
                     check_run("build (release)", "failure")], [])
        r = self.merge("--sha", self.head)
        self.assert_refused(r, "not green", "fail\tbuild (release)")
        self.assertNotIn("build (debug)", r.stderr)

    def test_cancelled_check_refused(self):
        self.set_ci([check_run("build (debug)", "cancelled")], [])
        self.assert_refused(self.merge("--sha", self.head),
                            "fail\tbuild (debug)")

    def test_pending_check_refused(self):
        self.set_ci([check_run("build (debug)", None, status="queued")], [])
        self.assert_refused(self.merge("--sha", self.head),
                            "pending\tbuild (debug)")

    def test_no_checks_refused(self):
        self.set_ci([], [])
        self.assert_refused(self.merge("--sha", self.head), "no CI checks")

    def test_failed_status_refused(self):
        self.set_ci([check_run("build (debug)", "success")],
                    [{"context": "ext", "state": "error"}])
        self.assert_refused(self.merge("--sha", self.head), "fail\text")

    def test_maintainer_alone_does_not_skip_ci(self):
        self.set_ci([check_run("build (release)", "failure")], [])
        self.assert_refused(self.merge("--maintainer"), "not green")

    def test_allow_red_ci_needs_maintainer(self):
        r = self.merge("--allow-red-ci", "why", "--sha", self.head)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("needs --maintainer", r.stderr)

    def test_allow_red_ci_needs_reason(self):
        r = self.merge("--maintainer", "--allow-red-ci", "", "--sha", self.head)
        self.assertEqual(r.returncode, 2, r.stderr)

    def test_allow_red_ci_records_decision(self):
        self.set_ci([check_run("build (release)", "failure")], [])
        r = self.merge("--maintainer", "--allow-red-ci",
                       "release -Werror in an old test, see #9",
                       "--sha", self.head)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.origin_main(), self.head)
        c = self.comments()
        self.assertIn(self.head, c)
        self.assertIn("release -Werror in an old test, see #9", c)
        self.assertIn("fail\tbuild (release)", c)

    def test_comment_failure_keeps_main(self):
        self.set_ci([check_run("build (release)", "failure")], [])
        (self.data / "comments.log").mkdir()  # makes the fake's write fail
        r = self.merge("--maintainer", "--allow-red-ci", "why",
                       "--sha", self.head)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("could not record", r.stderr)
        self.assertNotEqual(self.origin_main(), self.head)


if __name__ == "__main__":
    unittest.main()
