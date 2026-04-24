"""Shared pytest fixtures for integration tests."""

import pathlib

import pytest

from . import fixtures as fx
from .harness import VideoCompareSession

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent


@pytest.fixture
def video_compare_binary() -> pathlib.Path:
    """Where the tests expect ./video-compare to live. Override via
    VIDEO_COMPARE_BINARY env var (handled inside the harness)."""
    path = REPO_ROOT / "video-compare"
    if not path.exists():
        pytest.skip(f"video-compare binary not found at {path}; `make` first")
    return path


@pytest.fixture
def lg_daylight_pair() -> fx.Pair:
    """Default pair for integration logic tests. 480p downscale — fast
    enough that the whole suite stays responsive. Scoring is on normalized
    64x64 fingerprints regardless of source resolution, so algorithm
    behavior matches the 2160p pair. Use `lg_daylight_2160p_pair` instead
    when a test specifically needs 4K (resolution-mismatch, performance
    regression)."""
    return fx.load_pair("lg-daylight-480p-auto-align")


@pytest.fixture
def lg_daylight_2160p_pair() -> fx.Pair:
    """Full-resolution pair for resolution-mismatch and performance-
    regression tests. Slower (1–3 s per L2 seek on 2160p h264/VP9 decode)
    so use sparingly."""
    return fx.load_pair("lg-daylight-auto-align")


@pytest.fixture
def lg_daylight_backward_pair() -> fx.Pair:
    """Pair where aligned target is a NEGATIVE shift (LEFT has synthetic
    intro, RIGHT has no intro). Used by backward-convergence tests."""
    return fx.load_pair("lg-daylight-480p-backward-align")
