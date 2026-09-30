#!/usr/bin/env python3
"""Persist a job-bound OTA health decision after restart or rollback."""

import json
import fcntl
import os
import sys
import time


def main():
    if len(sys.argv) != 6:
        raise ValueError("expected staging, job, version, status, rollback")
    staging, job, version, status, rollback = sys.argv[1:]
    lock_fd = os.open(os.path.join(staging, "ota_state.lock"), os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    if not job or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for c in job):
        raise ValueError("invalid job id")
    if status not in ("healthy", "failed") or rollback not in ("none", "succeeded", "failed", "skipped"):
        raise ValueError("invalid health result")
    if (status == "healthy") != (rollback == "none"):
        raise ValueError("invalid health combination")
    pending = os.path.join(staging, job, "health_pending.txt")
    fields = {}
    with open(pending, encoding="utf-8") as source:
        for line in source:
            key, sep, value = line.rstrip("\n").partition("=")
            if not sep or key in fields:
                raise ValueError("invalid pending health identity")
            fields[key] = value
    if fields.get("jobId") != job or fields.get("version") != version or not fields.get("token"):
        raise ValueError("health identity mismatch")
    state = None
    if status == "healthy":
        with open(os.path.join(staging, job, "applied_state_pending.txt"), encoding="utf-8") as source:
            state = source.read()
        if not state.startswith("jobId=" + job + "\nversion=" + version + "\n") or not state.endswith("\n"):
            raise ValueError("invalid pending version state")
    path = os.path.join(staging, "health_" + job + ".txt")
    if os.path.exists(path):
        raise FileExistsError("OTA health decision already exists")
    record = dict(format="ota-health-v1", jobId=job, version=version,
                  token=fields["token"], status=status, rollback=rollback,
                  ts=str(int(time.time() * 1000)))
    temporary = path + ".tmp." + str(os.getpid())
    try:
        with open(temporary, "x", encoding="utf-8") as output:
            output.write(json.dumps(record, separators=(",", ":")) + "\n")
            output.flush()
            os.fsync(output.fileno())
        os.link(temporary, path)
        directory = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    if state is not None:
        for name, contents in (("current_version.txt", state), ("applied_version.txt", version + "\n")):
            target = os.path.join(staging, name)
            temporary = target + ".tmp." + str(os.getpid())
            try:
                with open(temporary, "x", encoding="utf-8") as output:
                    output.write(contents)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, target)
                directory = os.open(staging, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)


if __name__ == "__main__":
    main()
