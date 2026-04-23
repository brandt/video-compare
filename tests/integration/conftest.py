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
    return fx.load_pair("lg-daylight-auto-align")
