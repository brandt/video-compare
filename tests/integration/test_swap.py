"""Swap-aware input routing — Phases 1/2/3 of the swap rollout.

Phase 1: `+` / `-` frame shifting. Under swap, the sign flips at the input
layer so visual-right still means the underlying-LEFT pipeline.

Phase 2: shift-click on the timeline scopes its seek to the visually-right
side (underlying LEFT under swap).

Phase 3: auto-align (``` ` ```, `[`, `]`) follows the visual right as well —
the log's `follower=` field flips to LEFT when swap is active.
"""

from .harness import VideoCompareSession


def test_plus_minus_swap_sign_flip(video_compare_binary, lg_daylight_pair):
    """Phase 1: `+`/`-` signs flip at the input layer under swap.

    Three `+` with no swap, then three `+` under swap — the swap presses
    should undo the no-swap ones so effective_time_shift returns to baseline.
    Then three `-` under swap should increase it again.
    """
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)
        baseline = vc.get("effective_time_shift")

        # Three + (no swap) — should increase shift.
        for _ in range(3):
            vc.key("=")
            vc.seek_wait()
        after_plus = vc.get("effective_time_shift")
        assert after_plus > baseline, (
            f"expected shift to increase after 3x + / no-swap; got {after_plus:.4f} vs baseline {baseline:.4f}"
        )

        # Toggle swap, three + under swap — should undo the first three.
        vc.key("s")
        vc.seek_wait()
        assert vc.get("swap") is True

        for _ in range(3):
            vc.key("=")
            vc.seek_wait()
        after_swap_plus = vc.get("effective_time_shift")
        assert after_swap_plus < after_plus, (
            f"+ under swap should decrease shift (sign flip); "
            f"got {after_swap_plus:.4f} vs {after_plus:.4f}"
        )
        assert abs(after_swap_plus - baseline) < 0.01, (
            f"three + no-swap then three + swap should cancel; "
            f"got {after_swap_plus:.4f} vs baseline {baseline:.4f}"
        )

        # Symmetric check: three - under swap should increase the shift again.
        for _ in range(3):
            vc.key("-")
            vc.seek_wait()
        after_swap_minus = vc.get("effective_time_shift")
        assert after_swap_minus > after_swap_plus, (
            f"- under swap should increase shift (sign flip); "
            f"got {after_swap_minus:.4f} vs {after_swap_plus:.4f}"
        )


# Phase 2 (shift-click scopes to visual right) is covered by the standalone
# script at tmp/swap_click_test.py, which depends on window-layout coordinates
# tuned to the lg1 / -t2.0 launch. Porting here would need a robust way to
# locate the timeline strip on arbitrary fixture sizes; deferred.


def test_autoalign_follower_follows_swap(video_compare_binary, lg_daylight_pair):
    """Phase 3: auto-align's follower side follows the visual right.

    Press `]` with and without swap. The `[auto-align] mode=...` log line
    carries a `follower=RIGHT|LEFT` field that must match the display-side
    logic: no swap → RIGHT, swap active → LEFT.
    """
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Case A: no swap, press `]`.
        assert vc.get("swap") is False
        vc.key("]")
        vc.seek_wait()

        # Case B: enable swap, press `]` again.
        vc.key("s")
        vc.seek_wait()
        assert vc.get("swap") is True
        vc.key("]")
        vc.seek_wait()

        followers = [s.get("follower") for s in vc.auto_align_summaries()]
        assert len(followers) >= 2, (
            f"expected 2+ auto-align summary lines, got {len(followers)}"
        )
        assert followers[0] == "RIGHT", (
            f"no-swap press should route to follower=RIGHT, got {followers[0]}"
        )
        assert followers[1] == "LEFT", (
            f"swap-active press should route to follower=LEFT, got {followers[1]}"
        )
