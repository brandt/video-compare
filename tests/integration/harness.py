"""VideoCompareSession — launches ./video-compare and drives it via the
debug input socket. One session per test; pytest fixtures in conftest.py
wrap this.

Usage:
    with VideoCompareSession(pair) as vc:
        vc.wait_for(lambda: vc.get("play_state") == "PAUSE", timeout=5.0)
        vc.key("]")
        vc.sleep(2.5)
        assert vc.get("effective_time_shift") == pytest.approx(2.069, abs=0.002)
"""

from __future__ import annotations

import contextlib
import json
import os
import pathlib
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Optional

from .fixtures import Fixture, Pair

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent


def _default_binary() -> pathlib.Path:
    override = os.environ.get("VIDEO_COMPARE_BINARY")
    if override:
        return pathlib.Path(override).resolve()
    return REPO_ROOT / "video-compare"


class SocketClient:
    """JSON-Lines client against the debug input socket.

    Each request is a single JSON object terminated by '\\n'; each reply is
    the same. Connection is opened lazily and kept open for the session
    lifetime.
    """

    def __init__(self, sock_path: pathlib.Path):
        self._sock_path = sock_path
        self._sock: Optional[socket.socket] = None
        self._reader = None
        self._writer = None

    def connect(self, retries: int = 100, delay: float = 0.1) -> None:
        last_err: Optional[Exception] = None
        for _ in range(retries):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(str(self._sock_path))
                self._sock = s
                self._reader = s.makefile("r")
                self._writer = s.makefile("w")
                return
            except (FileNotFoundError, ConnectionRefusedError) as e:
                last_err = e
                time.sleep(delay)
        raise RuntimeError(f"could not connect to {self._sock_path}: {last_err}")

    def call(self, msg: dict) -> dict:
        assert self._writer is not None and self._reader is not None
        self._writer.write(json.dumps(msg) + "\n")
        self._writer.flush()
        line = self._reader.readline()
        if not line:
            raise RuntimeError("socket closed unexpectedly")
        return json.loads(line)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
                self._reader = None
                self._writer = None


class VideoCompareSession:
    """One launched video-compare process plus a socket client.

    Use as a context manager; the process is cleaned up on exit. Stderr is
    captured to a log file which tests can read for [auto-align] and
    [seek-timing] events.
    """

    def __init__(
        self,
        pair: Pair,
        *,
        binary_path: Optional[pathlib.Path] = None,
        extra_args: Optional[list[str]] = None,
        extra_env: Optional[dict[str, str]] = None,
        start_paused: bool = True,
        log_auto_align: bool = True,
        log_seek_timing: bool = True,
    ):
        self.pair = pair
        self._binary = binary_path or _default_binary()
        self._extra_args = list(extra_args or [])
        self._extra_env = dict(extra_env or {})
        self._start_paused = start_paused
        self._log_auto_align = log_auto_align
        self._log_seek_timing = log_seek_timing

        # Per-session temp dir holds the Unix socket + stderr log.
        self._tmpdir: Optional[tempfile.TemporaryDirectory] = None
        self._sock_path: Optional[pathlib.Path] = None
        self._log_path: Optional[pathlib.Path] = None
        self._log_fh = None
        self._proc: Optional[subprocess.Popen] = None
        self._client: Optional[SocketClient] = None

    # ---- context manager ----
    def __enter__(self) -> "VideoCompareSession":
        self._tmpdir = tempfile.TemporaryDirectory(prefix="vc-session-")
        tmpdir = pathlib.Path(self._tmpdir.name)
        self._sock_path = tmpdir / "vc.sock"
        self._log_path = tmpdir / "vc.log"
        self._log_fh = self._log_path.open("w")

        # Download / verify fixture files.
        left_path = self.pair.left.ensure_local()
        right_path = self.pair.right.ensure_local()

        env = os.environ.copy()
        env["VIDEO_COMPARE_INPUT_SOCK"] = str(self._sock_path)
        if self._log_auto_align:
            env["VIDEO_COMPARE_LOG_AUTO_ALIGN"] = "1"
        if self._log_seek_timing:
            env["VIDEO_COMPARE_LOG_SEEK_TIMING"] = "1"
        env.update(self._extra_env)

        argv: list[str] = [str(self._binary)]
        if self._start_paused:
            argv.append("--start-paused")
        argv.extend(self._extra_args)
        argv.append(str(left_path))
        argv.append(str(right_path))

        self._proc = subprocess.Popen(
            argv,
            stdout=self._log_fh,
            stderr=self._log_fh,
            env=env,
            cwd=str(REPO_ROOT),
        )

        self._client = SocketClient(self._sock_path)
        self._client.connect()
        return self

    def __exit__(self, exc_type, exc, tb):
        # When VIDEO_COMPARE_TEST_HOLD is set, pause before teardown so the
        # operator can manually inspect the video-compare window + final
        # state. Blocks on stdin; press Enter (or Ctrl-C) to continue and
        # close the session. Test actions have already completed by this
        # point, so what you see is the final landed state.
        if os.environ.get("VIDEO_COMPARE_TEST_HOLD"):
            status = "FAIL" if exc is not None else "pass"
            try:
                final_shift = self.get("effective_time_shift")
                final_state = self.get("play_state")
                sys.stderr.write(
                    f"\n[hold] test actions complete ({status}); "
                    f"play_state={final_state} effective_time_shift={final_shift:.4f} s. "
                    f"Log: {self._log_path}\n"
                    f"[hold] Press Enter (or Ctrl-C) to close the session.\n"
                )
                sys.stderr.flush()
                try:
                    input()
                except (KeyboardInterrupt, EOFError):
                    pass
            except Exception as inspect_err:
                sys.stderr.write(f"[hold] couldn't read state: {inspect_err}\n")

        # If an assertion failed, copy the log out before teardown so the
        # user can inspect it. Uses an artifacts dir under tests/integration/.
        if exc is not None and self._log_path is not None and self._log_path.exists():
            artifacts = REPO_ROOT / "tests" / "integration" / "_artifacts"
            artifacts.mkdir(parents=True, exist_ok=True)
            saved = artifacts / f"{self.pair.pair_id}-{int(time.time())}.log"
            try:
                import shutil
                shutil.copy(str(self._log_path), str(saved))
                sys.stderr.write(f"\n[harness] saved failing log -> {saved}\n")
            except Exception:
                pass

        try:
            if self._client is not None:
                try:
                    self._client.call({"cmd": "quit"})
                except Exception:
                    pass
                self._client.close()
        finally:
            if self._proc is not None:
                try:
                    self._proc.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    self._proc.send_signal(signal.SIGTERM)
                    try:
                        self._proc.wait(timeout=2.0)
                    except subprocess.TimeoutExpired:
                        self._proc.kill()
                        self._proc.wait()
            if self._log_fh is not None:
                self._log_fh.close()
            if self._tmpdir is not None:
                self._tmpdir.cleanup()
        return False  # don't suppress

    # ---- socket helpers ----
    def call(self, msg: dict) -> dict:
        assert self._client is not None
        resp = self._client.call(msg)
        if not resp.get("ok"):
            raise RuntimeError(f"call failed: {msg} -> {resp}")
        return resp

    def get(self, field: str) -> Any:
        return self.call({"cmd": "get", "field": field})["value"]

    def key(self, key: str) -> None:
        self.call({"cmd": "key", "action": "press", "key": key})

    def sleep(self, seconds: float) -> None:
        # Use the app's own sleep command so sleeps are synchronous against
        # the main-loop timeline rather than racing local wall-clock.
        self.call({"cmd": "sleep", "seconds": seconds})

    def wait_for(
        self,
        predicate: Callable[[], bool],
        *,
        timeout: float = 5.0,
        poll_interval: float = 0.05,
    ) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(poll_interval)
        raise TimeoutError(f"predicate didn't become true within {timeout} s")

    def seek_wait(self, timeout: float = 10.0, min_iterations: int = 2) -> None:
        """Block until the most recently dispatched input has been processed
        and any resulting seek has landed.

        Mechanism:
          - record `frame_number` (main-loop iteration counter published with
            every snapshot);
          - poll until it has advanced by >= `min_iterations`;
          - additionally require `play_state == "PAUSE"` so we don't exit
            while a multi-iteration seek is still in flight.

        Why 2 iterations? SDL key events are consumed one iteration, then
        the auto-align + seek dispatch block runs the NEXT iteration. Two
        increments guarantees both have happened. Under a multi-iteration
        L2 seek (4K/sparse keyframes) the play_state=PAUSE guard blocks
        further until the sync returns.

        Use this in place of `vc.sleep(N)` whenever N was a fudge tuned to
        the slowest expected fixture — `seek_wait` is deterministic and
        hardware-neutral for correctness tests.
        """
        start_frame = self.get("frame_number")

        def settled() -> bool:
            cur_frame = self.get("frame_number")
            if cur_frame - start_frame < min_iterations:
                return False
            return self.get("play_state") == "PAUSE"

        self.wait_for(settled, timeout=timeout, poll_interval=0.02)

    # ---- log access ----
    @property
    def log_path(self) -> pathlib.Path:
        assert self._log_path is not None
        return self._log_path

    def auto_align_summaries(self) -> list[dict[str, str]]:
        """Parse every `[auto-align] mode=...` summary line in the log.

        Returns a list of dicts, one per press, with the key=value pairs from
        the summary line.
        """
        out: list[dict[str, str]] = []
        if self._log_path is None or not self._log_path.exists():
            return out
        with self._log_path.open() as f:
            for line in f:
                if not line.startswith("[auto-align] mode="):
                    continue
                fields: dict[str, str] = {}
                for tok in line.rstrip("\n").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        fields[k] = v
                out.append(fields)
        return out

    def auto_align_landed_lines(self) -> list[dict[str, str]]:
        """Parse `[auto-align] landed_pts_delta_ms=...` verification lines."""
        out: list[dict[str, str]] = []
        if self._log_path is None or not self._log_path.exists():
            return out
        with self._log_path.open() as f:
            for line in f:
                if not line.startswith("[auto-align] landed_pts_delta_ms="):
                    continue
                fields: dict[str, str] = {}
                for tok in line.rstrip("\n").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        fields[k] = v
                out.append(fields)
        return out

    def seek_timing_lines(self) -> list[dict[str, str]]:
        out: list[dict[str, str]] = []
        if self._log_path is None or not self._log_path.exists():
            return out
        with self._log_path.open() as f:
            for line in f:
                if not line.startswith("[seek-timing]"):
                    continue
                fields: dict[str, str] = {}
                for tok in line.rstrip("\n").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        fields[k] = v
                out.append(fields)
        return out
