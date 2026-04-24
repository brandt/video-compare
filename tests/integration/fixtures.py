"""Fixture loader + downloader.

Reads sidecar JSONs checked into tests/fixtures/{sidecars,pairs}/, and
materializes the underlying video files in tests/fixtures/_cache/fixtures/
on demand — downloaded from S3 if not already present.

The generator at tests/fixtures/generate.py wrote the sidecars; this module
is strictly a read/download consumer. Test code only imports from here.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import subprocess
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SIDECAR_DIR = REPO_ROOT / "tests" / "fixtures" / "sidecars"
PAIR_DIR = REPO_ROOT / "tests" / "fixtures" / "pairs"
CACHE_DIR = REPO_ROOT / "tests" / "fixtures" / "_cache" / "fixtures"


def _sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dest: pathlib.Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    # Atomic: download to a .partial and rename. Keeps cache consistent if
    # the process is interrupted mid-download.
    partial = dest.with_suffix(dest.suffix + ".partial")
    print(f"[fixtures] downloading {url} -> {dest}", file=sys.stderr)
    # Use curl -f (fail on HTTP error) -L (follow redirects) -# (progress bar
    # to stderr) -o (output file).
    subprocess.run(
        ["curl", "-fL", "--retry", "3", "-o", str(partial), url],
        check=True,
    )
    partial.rename(dest)


@dataclasses.dataclass
class Fixture:
    """One encoded video + its generation metadata."""

    data: dict

    @property
    def fixture_id(self) -> str:
        return self.data["fixture_id"]

    @property
    def expected_sha256(self) -> str:
        return self.data["output"]["sha256"]

    @property
    def url(self) -> str:
        return self.data["output"]["url"]

    @property
    def filename(self) -> str:
        # The URL's last segment is authoritative for the on-disk filename.
        return self.url.rsplit("/", 1)[-1]

    @property
    def frame_pts_us(self) -> list[int]:
        return self.data["encoded"]["frame_pts_us"]

    @property
    def total_frames(self) -> int:
        return self.data["encoded"]["total_frames"]

    @property
    def first_source_frame(self) -> int:
        return self.data["content_mapping"]["first_source_frame_shown"]

    @property
    def intro_frames(self) -> int:
        """Number of encoded frames belonging to a synthetic pre-content
        intro (e.g., lavfi testsrc), if any. Zero for plain recipes."""
        return self.data["content_mapping"].get("intro_frames", 0)

    def local_path(self) -> pathlib.Path:
        return CACHE_DIR / self.filename

    def ensure_local(self, verify_sha256: bool = True) -> pathlib.Path:
        """Return a local path to the fixture, downloading if not cached.

        If a local file exists, the user has asked us to prefer it (see the
        discussion in the plan — local development is expected to run faster
        than always re-downloading). We hash-verify by default; pass
        `verify_sha256=False` to skip that cost on CI loops where the cache
        is trusted.
        """
        dst = self.local_path()
        if dst.exists():
            if verify_sha256:
                actual = _sha256(dst)
                if actual != self.expected_sha256:
                    raise RuntimeError(
                        f"Cached {dst} sha256 mismatch: expected {self.expected_sha256}, "
                        f"got {actual}. Delete the file to force a fresh download."
                    )
            return dst
        _download(self.url, dst)
        if verify_sha256:
            actual = _sha256(dst)
            if actual != self.expected_sha256:
                raise RuntimeError(
                    f"Downloaded {dst} sha256 mismatch: expected {self.expected_sha256}, got {actual}"
                )
        return dst

    def raw_pts_us_for_source_frame(self, source_frame: int) -> int | None:
        """Return the raw PTS (in microseconds) of the given source frame in
        this fixture, or None if that source frame isn't present.

        `first_source_frame_shown` names the source-frame index at the
        first CONTENT frame of this fixture. For a plain recipe that's at
        local_idx=0. For a recipe with a synthetic intro, source content
        starts at local_idx=intro_frames.
        """
        rel = source_frame - self.first_source_frame
        if rel < 0:
            return None
        local_idx = self.intro_frames + rel
        if local_idx >= self.total_frames:
            return None
        return self.frame_pts_us[local_idx]


@dataclasses.dataclass
class Pair:
    """A LEFT/RIGHT alignment pair."""

    data: dict
    left: Fixture
    right: Fixture

    @property
    def pair_id(self) -> str:
        return self.data["pair_id"]

    @property
    def check_points(self) -> list[dict]:
        return self.data["check_points"]

    def expected_effective_time_shift_us(self, source_frame: int) -> int:
        """For the given source frame, compute the target
        effective_time_shift in microseconds: right_raw_pts - left_raw_pts
        when both sides display that source_frame.

        Mirrors the app's internal definition of effective_time_shift
        (see TimeShifter::effective_shift — static_shift in the 1:1 case
        equals the raw pts delta).
        """
        l = self.left.raw_pts_us_for_source_frame(source_frame)
        r = self.right.raw_pts_us_for_source_frame(source_frame)
        if l is None or r is None:
            raise ValueError(
                f"Pair {self.pair_id}: source_frame={source_frame} not present on "
                f"both sides (left={l}, right={r})"
            )
        return r - l


def load_fixture(fixture_id: str) -> Fixture:
    path = SIDECAR_DIR / f"{fixture_id}.json"
    if not path.exists():
        raise FileNotFoundError(f"Sidecar not found: {path}")
    with path.open() as f:
        return Fixture(json.load(f))


def load_pair(pair_id: str) -> Pair:
    path = PAIR_DIR / f"{pair_id}.json"
    if not path.exists():
        raise FileNotFoundError(f"Pair sidecar not found: {path}")
    with path.open() as f:
        data = json.load(f)
    return Pair(
        data=data,
        left=load_fixture(data["left"]),
        right=load_fixture(data["right"]),
    )


def list_pairs() -> list[str]:
    return sorted(p.stem for p in PAIR_DIR.glob("*.json"))
