#!/usr/bin/env python3
"""One-off fixture generator for video-compare integration tests.

Not run by the test pipeline. Call this manually when:
  - Adding a new recipe.
  - Updating an existing recipe (rare — sidecars are checked in).
  - Re-seeding S3 after its contents are wiped.

Usage:
  # Generate all recipes locally (writes to tests/fixtures/_cache/fixtures/).
  python3 tests/fixtures/generate.py

  # Generate a specific recipe.
  python3 tests/fixtures/generate.py --recipe lg-daylight-sdr-h264-2160p-trim124

  # Generate + upload to S3 (assumes `aws` is configured with the
  # public-test-fixtures profile; see the bucket URL in the sidecar schema).
  python3 tests/fixtures/generate.py --upload

Sources live in testdata/source/ locally and are (also) published to the
same S3 bucket; this script uses the local copies when present.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Callable, Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SOURCE_DIR = REPO_ROOT / "testdata" / "source"
FIXTURE_CACHE_DIR = REPO_ROOT / "tests" / "fixtures" / "_cache" / "fixtures"
SIDECAR_DIR = REPO_ROOT / "tests" / "fixtures" / "sidecars"
PAIR_DIR = REPO_ROOT / "tests" / "fixtures" / "pairs"

S3_BUCKET = "public-test-fixtures"
S3_PREFIX = "video-compare"  # so uploaded objects live at s3://<bucket>/video-compare/...
S3_AWS_PROFILE = "public-test-fixtures"
S3_PUBLIC_URL_BASE = f"https://s3.amazonaws.com/{S3_BUCKET}/{S3_PREFIX}"


# ---------------------------------------------------------------------------
# Source registry: logical id -> (local filename, S3 object key under prefix)
# ---------------------------------------------------------------------------
@dataclass
class Source:
    id: str
    local_filename: str
    s3_key: str  # relative to S3_PREFIX

    def local_path(self) -> pathlib.Path:
        return SOURCE_DIR / self.local_filename

    def public_url(self) -> str:
        return f"{S3_PUBLIC_URL_BASE}/{self.s3_key}"


SOURCES: dict[str, Source] = {
    "lg-daylight-hdr-vp9-2160p-59.94": Source(
        id="lg-daylight-hdr-vp9-2160p-59.94",
        local_filename="LG - Daylight - [youtube FRE0Q-hBNcU] - (HDR VP9 yuv420p10le BT.2020 59.940fps).webm",
        s3_key="sources/lg-daylight-hdr-vp9-2160p-59.94.webm",
    ),
    "lg-daylight-sdr-vp9-2160p-59.94": Source(
        id="lg-daylight-sdr-vp9-2160p-59.94",
        local_filename="LG - Daylight - [youtube FRE0Q-hBNcU] - (SDR VP9 yuv420p BT.709 59.940fps).webm",
        s3_key="sources/lg-daylight-sdr-vp9-2160p-59.94.webm",
    ),
}


# ---------------------------------------------------------------------------
# Recipe definitions.
#
# A recipe is a function that takes (source_path, output_path) and returns
# the ffmpeg argv to run plus sidecar metadata. The generator invokes ffmpeg,
# probes the output, and writes the sidecar.
# ---------------------------------------------------------------------------
@dataclass
class Recipe:
    fixture_id: str
    description: str
    source_id: str
    extension: str  # "mp4" | "webm" | ...
    # Number of source frames skipped from the start. 0 means "no intro".
    # A nonzero value puts the source frame at this index at the fixture's
    # raw_pts=0.
    first_source_frame: int
    # Declared encoded properties — the generator verifies the real output
    # matches these via ffprobe after encoding.
    encoded_fps_num: int = 60000
    encoded_fps_den: int = 1001
    encoded_time_base_num: int = 1
    encoded_time_base_den: int = 1000
    encoded_codec: str = "h264"
    encoded_pixel_format: str = "yuv420p"
    encoded_keyframe_interval_frames: int = 250
    # Total duration to encode (seconds).
    duration_seconds: float = 15.0
    # Extra ffmpeg argv appended after -i and before the output path.
    extra_args: list[str] = field(default_factory=list)

    def output_filename(self) -> str:
        return f"{self.fixture_id}.{self.extension}"


RECIPES: list[Recipe] = [
    # Pair for lg-daylight-auto-align:
    #   LEFT=trim124 (source from frame 124, h264 SDR)
    #   RIGHT=base (source from frame 0, VP9 HDR, re-encoded with fixed GOP)
    # At both sides raw=0: LEFT shows source[124], RIGHT shows source[0].
    # To align, RIGHT must seek forward by 124 frames ≈ 2.069 s.
    Recipe(
        fixture_id="lg-daylight-sdr-h264-2160p-trim124",
        description=(
            "SDR x264 2160p, trimmed to start at source frame 124. Plays source[124..] "
            "from raw_pts=0. Used as LEFT side in lg-daylight-auto-align."
        ),
        source_id="lg-daylight-sdr-vp9-2160p-59.94",
        extension="mp4",
        first_source_frame=124,
        encoded_codec="h264",
        encoded_pixel_format="yuv420p",
        encoded_keyframe_interval_frames=250,
        duration_seconds=15.0,
        extra_args=[
            "-r", "60000/1001",  # pin frame rate explicitly (source is 19001/317 ≈ same)
            "-c:v", "libx264",
            "-preset", "medium",
            "-crf", "20",
            "-pix_fmt", "yuv420p",
            "-g", "250",      # keyframe interval
            "-keyint_min", "250",
            "-bf", "2",
            "-sc_threshold", "0",  # disable scenecut so GOP is uniform
            "-video_track_timescale", "1000",  # force 1/1000 time_base
            "-an",            # drop audio
        ],
    ),
    # 480p variants — same shape as the 2160p pair above, downscaled for
    # fast integration-test iteration (L2 drain drops from ~1.3 s to ~100 ms,
    # fixture download from 75 MB to a few MB). Used by default for logic
    # tests; the 2160p pair above is reserved for resolution-mismatch and
    # performance-regression coverage.
    Recipe(
        fixture_id="lg-daylight-sdr-h264-480p-trim124",
        description=(
            "SDR x264 480p, trimmed to start at source frame 124. 480p downscale of "
            "lg-daylight-sdr-h264-2160p-trim124 for lightweight logic tests."
        ),
        source_id="lg-daylight-sdr-vp9-2160p-59.94",
        extension="mp4",
        first_source_frame=124,
        encoded_codec="h264",
        encoded_pixel_format="yuv420p",
        encoded_keyframe_interval_frames=250,
        duration_seconds=15.0,
        extra_args=[
            "-r", "60000/1001",
            "-vf", "scale=854:480",
            "-c:v", "libx264",
            "-preset", "medium",
            "-crf", "20",
            "-pix_fmt", "yuv420p",
            "-g", "250",
            "-keyint_min", "250",
            "-bf", "2",
            "-sc_threshold", "0",
            "-video_track_timescale", "1000",
            "-an",
        ],
    ),
    Recipe(
        fixture_id="lg-daylight-sdr-vp9-480p-base",
        description=(
            "SDR VP9 480p 8-bit, plays source[0..] from raw_pts=0. 480p downscale "
            "of lg-daylight-sdr-vp9-2160p-base for lightweight logic tests."
        ),
        source_id="lg-daylight-sdr-vp9-2160p-59.94",
        extension="webm",
        first_source_frame=0,
        encoded_codec="vp9",
        encoded_pixel_format="yuv420p",
        encoded_keyframe_interval_frames=250,
        duration_seconds=15.0,
        extra_args=[
            "-r", "60000/1001",
            "-vf", "scale=854:480",
            "-c:v", "libvpx-vp9",
            "-b:v", "0",
            "-crf", "30",
            "-pix_fmt", "yuv420p",
            "-g", "250",
            "-keyint_min", "250",
            "-auto-alt-ref", "0",
            "-deadline", "good",
            "-cpu-used", "4",
            "-video_track_timescale", "1000",
            "-an",
        ],
    ),
    Recipe(
        fixture_id="lg-daylight-sdr-vp9-2160p-base",
        description=(
            "SDR VP9 2160p 8-bit, plays source[0..] from raw_pts=0. Used as RIGHT "
            "side in lg-daylight-auto-align — contains 124 source-frames of "
            "\"intro\" content before the aligned region. 8-bit (yuv420p) to "
            "match the fingerprint path's ring-frame format; a 10-bit variant "
            "is a separate recipe."
        ),
        source_id="lg-daylight-sdr-vp9-2160p-59.94",
        extension="webm",
        first_source_frame=0,
        encoded_codec="vp9",
        encoded_pixel_format="yuv420p",
        encoded_keyframe_interval_frames=250,
        duration_seconds=15.0,
        extra_args=[
            "-r", "60000/1001",  # pin frame rate explicitly (source is 19001/317 ≈ same)
            "-c:v", "libvpx-vp9",
            "-b:v", "0",
            "-crf", "30",
            "-pix_fmt", "yuv420p",
            "-g", "250",
            "-keyint_min", "250",
            "-auto-alt-ref", "0",  # disable alt-ref for predictable ordering
            "-deadline", "good",
            "-cpu-used", "4",
            "-video_track_timescale", "1000",
            "-an",
        ],
    ),
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def sha256_file(path: pathlib.Path, chunk_size: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(chunk_size), b""):
            h.update(chunk)
    return h.hexdigest()


def ffmpeg_version() -> str:
    out = subprocess.run(
        ["ffmpeg", "-version"], check=True, capture_output=True, text=True
    ).stdout
    first_line = out.splitlines()[0]
    return first_line.strip()


def ffprobe_stream_info(path: pathlib.Path) -> dict:
    """Return a dict of video-stream properties from the encoded file."""
    res = subprocess.run(
        [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries",
            "stream=codec_name,pix_fmt,width,height,r_frame_rate,time_base,has_b_frames",
            "-of", "json",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    data = json.loads(res.stdout)
    stream = data["streams"][0]
    return stream


def ffprobe_frame_pts(path: pathlib.Path) -> list[int]:
    """Return the sorted list of frame PTSes (in stream_tb units)."""
    res = subprocess.run(
        [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "packet=pts",
            "-of", "csv=p=0",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    ptses = [int(line) for line in res.stdout.splitlines() if line.strip()]
    ptses.sort()
    return ptses


def build_ffmpeg_argv(recipe: Recipe, source_path: pathlib.Path, output_path: pathlib.Path) -> list[str]:
    """Construct the ffmpeg argv for a recipe.

    We use input-side seeking to skip frames cheaply, then output-side -t to cap
    duration. For frame-exactness we rely on the encoder producing sequential
    PTSes from 0 (which is the default with no -copyts).
    """
    # Convert first_source_frame to a start time using the source's source fps.
    # Our known sources are 60000/1001 ≈ 59.94 fps.
    start_seconds = recipe.first_source_frame * recipe.encoded_fps_den / recipe.encoded_fps_num
    argv = [
        "ffmpeg", "-y", "-hide_banner",
        "-ss", f"{start_seconds:.9f}",  # seek before -i for speed (keyframe-aligned)
        "-i", str(source_path),
        "-t", f"{recipe.duration_seconds:.6f}",
    ] + list(recipe.extra_args) + [
        str(output_path),
    ]
    return argv


def run_ffmpeg(argv: list[str]) -> None:
    print(f"  $ {' '.join(argv)}", file=sys.stderr)
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"ffmpeg failed with exit {result.returncode}")


def parse_frame_rate(r_frame_rate: str) -> tuple[int, int]:
    num_s, den_s = r_frame_rate.split("/")
    return int(num_s), int(den_s)


def parse_time_base(time_base: str) -> tuple[int, int]:
    return parse_frame_rate(time_base)


def sidecar_path(fixture_id: str) -> pathlib.Path:
    return SIDECAR_DIR / f"{fixture_id}.json"


def output_path(fixture_id: str, extension: str) -> pathlib.Path:
    FIXTURE_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    return FIXTURE_CACHE_DIR / f"{fixture_id}.{extension}"


# ---------------------------------------------------------------------------
# Main generation loop
# ---------------------------------------------------------------------------
def generate_one(recipe: Recipe) -> pathlib.Path:
    source = SOURCES[recipe.source_id]
    src_path = source.local_path()
    if not src_path.exists():
        raise SystemExit(
            f"Source not found locally: {src_path}\n"
            f"Download it first (or run `aws --profile {S3_AWS_PROFILE} s3 cp "
            f"s3://{S3_BUCKET}/{S3_PREFIX}/{source.s3_key} {src_path}`)."
        )

    out_path = output_path(recipe.fixture_id, recipe.extension)
    argv = build_ffmpeg_argv(recipe, src_path, out_path)

    print(f"[generate] {recipe.fixture_id}", file=sys.stderr)
    run_ffmpeg(argv)

    # Probe the encoded output.
    stream = ffprobe_stream_info(out_path)
    ptses = ffprobe_frame_pts(out_path)
    fps_num, fps_den = parse_frame_rate(stream["r_frame_rate"])
    tb_num, tb_den = parse_time_base(stream["time_base"])

    # Verify key declared properties match reality. The codec and pixel
    # format are strict; the frame-rate representation is advisory because
    # different container/codec combos normalize the r_frame_rate fraction
    # differently (e.g., 60000/1001 vs 19001/317 are both ≈59.94). The
    # measured rate goes into the sidecar as-is; tests use per-frame pts
    # values, not derived-from-fps math.
    strict_checks = {
        "codec": (stream["codec_name"], recipe.encoded_codec),
        "pixel_format": (stream["pix_fmt"], recipe.encoded_pixel_format),
    }
    for name, (got, expected) in strict_checks.items():
        if got != expected:
            raise SystemExit(
                f"Recipe {recipe.fixture_id}: {name} mismatch — "
                f"declared {expected}, ffprobe reports {got}. "
                f"Fix the recipe declarations or adjust encoder args."
            )

    # Advisory: if the measured rate differs from declared by more than
    # ~0.1 fps, something's off.
    declared_rate = recipe.encoded_fps_num / recipe.encoded_fps_den
    measured_rate = fps_num / fps_den
    if abs(declared_rate - measured_rate) > 0.1:
        raise SystemExit(
            f"Recipe {recipe.fixture_id}: fps diverged — declared "
            f"{declared_rate:.4f}, measured {measured_rate:.4f}."
        )

    sidecar = {
        "fixture_id": recipe.fixture_id,
        "description": recipe.description,
        "source": {
            "id": source.id,
            "url": source.public_url(),
            "sha256": sha256_file(src_path),
        },
        "generation": {
            "ffmpeg_version": ffmpeg_version(),
            "argv": argv,
        },
        "output": {
            "url": f"{S3_PUBLIC_URL_BASE}/fixtures/{recipe.output_filename()}",
            "sha256": sha256_file(out_path),
            "byte_size": out_path.stat().st_size,
        },
        "encoded": {
            "codec": stream["codec_name"],
            "pixel_format": stream["pix_fmt"],
            "resolution": [int(stream["width"]), int(stream["height"])],
            "fps_num": fps_num,
            "fps_den": fps_den,
            "time_base_num": tb_num,
            "time_base_den": tb_den,
            "keyframe_interval_frames": recipe.encoded_keyframe_interval_frames,
            "has_b_frames": bool(stream.get("has_b_frames", 0)),
            "total_frames": len(ptses),
            "first_frame_pts_us": ptses[0] * 1_000_000 * tb_num // tb_den if ptses else 0,
            "last_frame_pts_us": ptses[-1] * 1_000_000 * tb_num // tb_den if ptses else 0,
            # Per-frame PTSes in microseconds, indexed by the fixture-local
            # frame number (0-indexed, display order, ascending pts). This is
            # the load-bearing ground truth — tests use this array directly
            # to compute expected raw-pts values for specific frame checks.
            "frame_pts_us": [p * 1_000_000 * tb_num // tb_den for p in ptses],
        },
        "content_mapping": {
            "first_source_frame_shown": recipe.first_source_frame,
            "description": (
                f"At this fixture's raw_pts=0, the frame shown corresponds to "
                f"source_frame={recipe.first_source_frame}."
            ),
        },
    }

    SIDECAR_DIR.mkdir(parents=True, exist_ok=True)
    scp = sidecar_path(recipe.fixture_id)
    with scp.open("w") as f:
        json.dump(sidecar, f, indent=2, sort_keys=False)
        f.write("\n")
    print(f"[generate]   -> {scp.relative_to(REPO_ROOT)}", file=sys.stderr)
    print(f"[generate]   -> {out_path.relative_to(REPO_ROOT)} ({out_path.stat().st_size} bytes)", file=sys.stderr)
    return out_path


def upload_fixture(recipe: Recipe, out_path: pathlib.Path) -> None:
    s3_uri = f"s3://{S3_BUCKET}/{S3_PREFIX}/fixtures/{recipe.output_filename()}"
    argv = ["aws", "--profile", S3_AWS_PROFILE, "s3", "cp", str(out_path), s3_uri]
    print(f"[upload] {' '.join(argv)}", file=sys.stderr)
    subprocess.run(argv, check=True)


def upload_source(source: Source) -> None:
    s3_uri = f"s3://{S3_BUCKET}/{S3_PREFIX}/{source.s3_key}"
    argv = ["aws", "--profile", S3_AWS_PROFILE, "s3", "cp", str(source.local_path()), s3_uri]
    print(f"[upload] {' '.join(argv)}", file=sys.stderr)
    subprocess.run(argv, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recipe", help="Generate only this recipe (fixture_id).")
    parser.add_argument("--upload", action="store_true", help="Upload generated fixtures (and sources) to S3.")
    parser.add_argument("--upload-sources-only", action="store_true", help="Upload just the sources, skip fixture generation.")
    args = parser.parse_args()

    if args.upload_sources_only:
        for source in SOURCES.values():
            if source.local_path().exists():
                upload_source(source)
            else:
                print(f"[skip] source not local: {source.local_path()}", file=sys.stderr)
        return

    recipes_to_run = RECIPES
    if args.recipe:
        recipes_to_run = [r for r in RECIPES if r.fixture_id == args.recipe]
        if not recipes_to_run:
            raise SystemExit(f"No recipe matches --recipe={args.recipe!r}")

    for recipe in recipes_to_run:
        out_path = generate_one(recipe)
        if args.upload:
            upload_fixture(recipe, out_path)

    if args.upload:
        # Also refresh source uploads if any are local.
        for source in SOURCES.values():
            if source.local_path().exists():
                upload_source(source)


if __name__ == "__main__":
    main()
