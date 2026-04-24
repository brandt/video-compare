"""Directional iteration through equal-confidence candidates.

Under the per-mode decision rules, `[` / `]` use "at-or-above current -
eps" and the landed position updates follower_current. Pressing the
same directional key again from the new position finds the next
candidate in the searched region (the previous landing is now "current"
and excluded from selection). The effect is step-through: repeated
presses walk forward/backward through near-equal-confidence matches.

The lg-daylight forward pair converges on its +2069 ms 1.0 peak in two
presses. A third `]` iterates one frame forward to the next at-or-above
candidate (score ~0.999, one frame ahead of the peak) — verified here.
"""

from .harness import VideoCompareSession


def test_press_3_iterates_one_frame_past_converged_target(video_compare_binary, lg_daylight_pair):
    """Two `]` presses converge on the peak; the 3rd steps forward one
    frame. Effective time shift must increase by approximately one
    right-side frame duration (17 ms on the 30 fps right stream)."""
    pair = lg_daylight_pair

    with VideoCompareSession(pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Press 1 + 2: converge on the 1.0 peak.
        for _ in range(2):
            vc.key("]")
            vc.seek_wait()
        shift_after_convergence = vc.get("effective_time_shift")

        # Press 3: iterate forward one frame.
        vc.key("]")
        vc.seek_wait()
        shift_after_iteration = vc.get("effective_time_shift")

        # Right-side delta is 1/30 s ≈ 33 ms per frame; the iteration step
        # should advance the shift by roughly one frame in that direction.
        # Generous bounds accommodate variable-duration frames and the
        # 1-ms time_base quantization.
        delta = shift_after_iteration - shift_after_convergence
        assert 0.005 < delta < 0.050, (
            f"press 3 should step forward by ~one frame; got delta={delta:.4f}s. "
            f"convergence={shift_after_convergence:.4f}s, iteration={shift_after_iteration:.4f}s.\n"
            "auto-align summaries:\n"
            + "\n".join(f"  {s}" for s in vc.auto_align_summaries())
        )

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 3
        # Press 3's decision must be seek (iteration), not already or
        # no_stronger_in_direction — there's a near-peer ahead of the peak
        # that still satisfies the at-or-above gate.
        assert summaries[2]["decision"] == "seek", (
            f"press 3 expected decision=seek (iteration), got {summaries[2]['decision']}\n"
            f"summary: {summaries[2]}"
        )
        # The iteration seek is tiny — one frame.
        assert summaries[2]["shift_frames"] in ("1", "-1"), (
            f"press 3 expected 1-frame shift, got shift_frames={summaries[2]['shift_frames']}\n"
            f"summary: {summaries[2]}"
        )
