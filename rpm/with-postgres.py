#!/usr/bin/python3
# Copyright (C) 2026 Qore Technologies, s.r.o.
# SPDX-License-Identifier: MIT
"""Run a command against a private PostgreSQL cluster."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


def run(command, bindir):
    bindir = Path(bindir).resolve()
    if not command:
        raise ValueError("A test command is required")
    if os.geteuid() == 0:
        raise ValueError("Run PostgreSQL tests as an unprivileged user")
    for name in ("initdb", "pg_ctl"):
        if not (bindir / name).is_file() or not os.access(bindir / name, os.X_OK):
            raise ValueError("Missing PostgreSQL executable: " + str(bindir / name))

    root = Path(tempfile.mkdtemp(prefix="qore-pgsql-", dir="/tmp"))
    data, socket = root / "data", root / "socket"
    env = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
    try:
        socket.mkdir()
        subprocess.run([str(bindir / "initdb"), "-D", str(data), "--username=qore_test",
                        "--auth=trust", "--encoding=UTF8", "--no-locale"], check=True, env=env)
        # Only a private Unix socket is opened: no TCP listener or shared cluster.
        options = shlex.join(["-k", str(socket), "-h", "", "-p", "5432"])
        subprocess.run([str(bindir / "pg_ctl"), "-D", str(data), "-w", "-t", "30",
                        "-l", str(root / "postgres.log"), "-o", options, "start"], check=True, env=env)
        env.update(PGHOST=str(socket), PGPORT="5432", PGDATABASE="postgres", PGUSER="qore_test", PGPASSWORD="")
        env["QORE_DB_CONNSTR_PGSQL"] = "pgsql:qore_test/@postgres%" + str(socket) + ":5432"
        return subprocess.run(command, env=env).returncode
    finally:
        if (data / "postmaster.pid").exists():
            stopped = subprocess.run([str(bindir / "pg_ctl"), "-D", str(data),
                                      "-w", "-t", "30", "-m", "fast", "stop"], env=env)
            if stopped.returncode:
                raise RuntimeError("PostgreSQL did not stop; preserving its cluster at " + str(root))
        shutil.rmtree(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bindir", type=Path, default=Path("/usr/bin"))
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    raise SystemExit(run(command, args.bindir))


if __name__ == "__main__":
    main()
