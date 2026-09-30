#!/usr/bin/env python3
"""Persist a job-bound OTA health decision after restart or rollback."""

import json
import fcntl
import os
import sys
import time


def active_job(staging, job, token, cancel=False):
    if not job or not token or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for c in job + token):
        raise ValueError("invalid active job identity")
    lock_fd = os.open(os.path.join(staging, "ota_state.lock"), os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock_fd, fcntl.LOCK_EX)
    active_path = os.path.join(staging, "ota_active_job.txt")
    expected = ("jobId=" + job + "\ntoken=" + token + "\n").encode("ascii")
    try:
        with open(active_path, "rb") as source:
            current = source.read(257)
    except FileNotFoundError:
        current = b""
    if cancel:
        if current == expected and not os.path.exists(os.path.join(staging, "ota_job_" + job + ".json")):
            os.unlink(active_path)
            sync_dir(staging)
        return
    if current == expected:
        return
    if current:
        lines = current.decode("ascii").splitlines()
        if len(lines) != 2 or not lines[0].startswith("jobId=") or not lines[1].startswith("token="):
            raise ValueError("invalid active job marker")
        previous_job, previous_token = lines[0][6:], lines[1][6:]
        if (not previous_job or not previous_token or
            any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for c in previous_job + previous_token) or
            current != ("jobId=" + previous_job + "\ntoken=" + previous_token + "\n").encode("ascii")):
            raise ValueError("invalid active job marker")
        final = os.path.join(staging, "ota_final_" + previous_job + ".txt")
        health = os.path.join(staging, "health_" + previous_job + ".txt")
        if os.path.exists(os.path.join(staging, "ota_job_" + previous_job + ".json")):
            with open(os.path.join(staging, "ota_job_" + previous_job + ".json"), encoding="utf-8") as source:
                intent = json.load(source)
            if (intent.get("format") != "ota-job-v1" or intent.get("jobId") != previous_job or
                intent.get("token") != previous_token or not intent.get("version")):
                raise ValueError("previous OTA intent is invalid")
            with open(final, "rb") as source:
                terminal = source.read(257)
            completed = ("jobId=" + previous_job + "\ntoken=" + previous_token + "\nstatus=completed\n").encode("ascii")
            failed = ("jobId=" + previous_job + "\ntoken=" + previous_token + "\nstatus=failed\n").encode("ascii")
            if terminal == failed:
                with open(health, "rb") as source:
                    result = json.loads(source.read(4097))
                if (result.get("jobId") != previous_job or result.get("version") != intent["version"] or
                    result.get("token") != previous_token or
                    result.get("status") != "failed" or result.get("rollback") not in ("succeeded", "unchanged")):
                    raise ValueError("previous OTA rollback is unresolved")
            elif terminal != completed:
                raise ValueError("previous OTA job is unresolved")
        else:
            with open(health, "rb") as source:
                terminal = source.read(4097)
            if len(terminal) > 4096 or not terminal.endswith(b"\n"):
                raise ValueError("previous OTA health is incomplete")
            result = json.loads(terminal)
            if (result.get("jobId") != previous_job or result.get("token") != previous_token or
                not (result.get("status") == "healthy" or
                     (result.get("status") == "failed" and result.get("rollback") == "succeeded"))):
                raise ValueError("previous OTA job is unresolved")
    temporary = active_path + ".tmp." + str(os.getpid())
    try:
        with open(temporary, "xb") as output:
            output.write(expected)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, active_path)
        sync_dir(staging)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def sync_dir(path):
    directory = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


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
    intent_path = os.path.join(staging, "ota_job_" + job + ".json")
    if os.path.exists(intent_path):
        with open(intent_path, encoding="utf-8") as source:
            intent = json.load(source)
        if (intent.get("format") != "ota-job-v1" or intent.get("jobId") != job or
            intent.get("version") != version or intent.get("token") != fields["token"]):
            raise ValueError("health does not match OTA intent")
    active_path = os.path.join(staging, "ota_active_job.txt")
    if os.path.exists(active_path):
        with open(active_path, "rb") as source:
            active = source.read(257)
        expected = ("jobId=" + job + "\ntoken=" + fields["token"] + "\n").encode("ascii")
        if active != expected:
            raise ValueError("health result is not for active OTA job")
    else:
        raise ValueError("OTA active job marker is missing")
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
    if len(sys.argv) == 5 and sys.argv[1] in ("--activate", "--cancel"):
        active_job(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[1] == "--cancel")
    else:
        main()
