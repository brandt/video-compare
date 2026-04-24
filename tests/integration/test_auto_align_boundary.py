"""Clip-boundary detection for auto-align.

Prior behavior would happily let `[` / `]` expand the reported search
window past the clip's actual duration (the "reached end of packet
buffer" short-circuit only fired when PacketRing::keyframe_at_or_before
returned nullopt, which is rare on clips the ring fully covers). Users
saw the HUD's reported window grow to e.g. -80 s on a 30 s clip with no
indication that further expansion couldn't possibly help.

The new behavior clamps the searched interval to the follower clip's
[start_time, start_time + duration) and emits a dedicated boundary
decision (`boundary_start` / `boundary_end` / `boundary_both`) when the
pressed direction is saturated and no stronger candidate is found.
"""

from .harness import VideoCompareSession


def test_backward_press_at_clip_start_emits_boundary(video_compare_binary, lg_daylight_pair):
    """Press `[` immediately while paused at clip start — follower_current is
    ~0 s, so the Backward mode's base window has nowhere to grow. Expect
    `boundary_start` on the first press and on any subsequent same-key
    presses (searched_window stays at [0, 0]s, low_sat=1)."""
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)
        assert vc.get("play_state") == "PAUSE"

        for _ in range(3):
            vc.key("[")
            vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 3, f"expected 3+ summaries, got {len(summaries)}"

        for i, s in enumerate(summaries[:3]):
            assert s["decision"] == "boundary_start", (
                f"press {i+1}: expected decision=boundary_start at clip start, "
                f"got {s['decision']}\nsummary: {s}"
            )
            assert s["low_sat"] == "1", (
                f"press {i+1}: expected low_sat=1 at clip start, got {s['low_sat']}"
            )
            # searched_window's low end must be 0 or very close (modulo the
            # display-relative-to-follower_current formatting), because the
            # clamp prevents growth below clip start.
            low_rel = float(s["searched_window"].strip("[]s").split(",")[0])
            # follower_current is at/near 0, so low_rel should be >= -0.001s
            # (floating-point slop on ms rounding).
            assert low_rel >= -0.002, (
                f"press {i+1}: searched_window low end should not extend past "
                f"clip start; got low_rel={low_rel}s\nsummary: {s}"
            )


def test_searched_window_does_not_grow_past_clip_start(video_compare_binary, lg_daylight_pair):
    """Fires `[` many times from clip start and asserts the reported
    searched_window low end stops expanding once it hits the clip boundary.
    Guards the primary regression the boundary feature fixes."""
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        for _ in range(8):
            vc.key("[")
            vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 8

        low_ends = []
        for s in summaries[:8]:
            low_rel = float(s["searched_window"].strip("[]s").split(",")[0])
            low_ends.append(low_rel)

        # After saturation, the low end must not decrease further across
        # subsequent presses. All 8 presses from clip start should report
        # the same low_rel (bounded by the clip at 0).
        min_low = min(low_ends)
        max_low = max(low_ends)
        assert abs(max_low - min_low) < 0.01, (
            f"searched_window low end grew across {len(low_ends)} presses at "
            f"clip start — boundary clamp not working. low_ends={low_ends}"
        )
