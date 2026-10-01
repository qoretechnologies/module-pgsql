#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Exercise fixture cleanup, failure propagation and environment isolation."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.util.spec_from_file_location("fixture", Path(__file__).with_name("with-postgres.py"))
fixture = importlib.util.module_from_spec(loader)
loader.loader.exec_module(fixture)


class FixtureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in ("initdb", "pg_ctl"):
            path = self.root / name
            path.touch()
            path.chmod(0o755)
        self.cluster = None
        self.stopped = False

    def simulate(self, args, **kwargs):
        name = Path(args[0]).name
        if name == "initdb":
            self.cluster = Path(args[args.index("-D") + 1])
            self.cluster.mkdir()
            return subprocess.CompletedProcess(args, 0)
        if name == "pg_ctl":
            if args[-1] == "start":
                options = shlex.split(args[args.index("-o") + 1])
                self.assertEqual(options[options.index("-h") + 1], "")
                self.assertEqual(options[options.index("-p") + 1], "5432")
                (self.cluster / "postmaster.pid").write_text("123\n")
            else:
                self.stopped = True
                (self.cluster / "postmaster.pid").unlink()
            return subprocess.CompletedProcess(args, 0)
        self.assertEqual(args, ["test-program"])
        env = kwargs["env"]
        self.assertNotIn("PGHOSTADDR", env)
        self.assertNotIn("PGSERVICE", env)
        self.assertEqual(env["PGDATABASE"], "postgres")
        self.assertIn("%" + env["PGHOST"] + ":5432", env["QORE_DB_CONNSTR_PGSQL"])
        return subprocess.CompletedProcess(args, self.test_status)

    def test_success_and_test_failure_stop_and_remove_private_cluster(self):
        for status in (0, 19):
            self.stopped = False
            self.test_status = status
            with self.subTest(status=status), patch.object(fixture.os, "geteuid", return_value=1000), \
                    patch.dict(os.environ, PGHOSTADDR="203.0.113.1", PGSERVICE="external"), \
                    patch.object(fixture.subprocess, "run", side_effect=self.simulate):
                self.assertEqual(fixture.run(["test-program"], self.root), status)
            self.assertTrue(self.stopped)
            self.assertFalse(self.cluster.parent.exists())

    def test_start_failure_still_stops_a_started_server(self):
        def start_failure(args, **kwargs):
            result = self.simulate(args, **kwargs)
            if args[-1] == "start":
                raise subprocess.CalledProcessError(1, args)
            return result

        with patch.object(fixture.os, "geteuid", return_value=1000), \
                patch.object(fixture.subprocess, "run", side_effect=start_failure):
            with self.assertRaises(subprocess.CalledProcessError):
                fixture.run(["test-program"], self.root)
        self.assertTrue(self.stopped)
        self.assertFalse(self.cluster.parent.exists())

    def test_command_launch_error_still_stops_and_removes_cluster(self):
        def launch_error(args, **kwargs):
            if args == ["test-program"]:
                raise FileNotFoundError("test executable disappeared")
            return self.simulate(args, **kwargs)

        with patch.object(fixture.os, "geteuid", return_value=1000), \
                patch.object(fixture.subprocess, "run", side_effect=launch_error):
            with self.assertRaises(FileNotFoundError):
                fixture.run(["test-program"], self.root)
        self.assertTrue(self.stopped)
        self.assertFalse(self.cluster.parent.exists())

    def test_failed_stop_preserves_cluster_and_fails_the_run(self):
        self.test_status = 0

        def failed_stop(args, **kwargs):
            if args[-1] == "stop":
                return subprocess.CompletedProcess(args, 1)
            return self.simulate(args, **kwargs)

        with patch.object(fixture.os, "geteuid", return_value=1000), \
                patch.object(fixture.subprocess, "run", side_effect=failed_stop):
            try:
                with self.assertRaisesRegex(RuntimeError, "preserving its cluster"):
                    fixture.run(["test-program"], self.root)
                self.assertTrue((self.cluster / "postmaster.pid").is_file())
            finally:
                # The simulated server has no process; remove this test's files.
                if self.cluster is not None:
                    fixture.shutil.rmtree(self.cluster.parent)

    def test_invalid_inputs_fail_before_creating_a_cluster(self):
        with patch.object(fixture.tempfile, "mkdtemp", side_effect=AssertionError("created cluster")):
            with self.assertRaisesRegex(ValueError, "test command"):
                fixture.run([], self.root)
            with patch.object(fixture.os, "geteuid", return_value=0):
                with self.assertRaisesRegex(ValueError, "unprivileged"):
                    fixture.run(["test-program"], self.root)
            with patch.object(fixture.os, "geteuid", return_value=1000):
                with self.assertRaisesRegex(ValueError, "Missing PostgreSQL executable"):
                    fixture.run(["test-program"], self.root / "missing")


if __name__ == "__main__":
    unittest.main()
