"""Cross-mode cache carry for auto-align.

Previously, pressing a different auto-align key invalidated the cache
(the retry predicate required `retry.mode == auto_align_mode`). The
user's mental model is a progress bar on the follower axis that fills
in as they search — whatever key they press, the searched region should
only grow, never reset. This test verifies that `` ` `` → `[` preserves
the symmetric press's searched interval and extends it leftward rather
than starting over.
"""

from .harness import VideoCompareSession


def _parse_window(field: str) -> tuple[float, float]:
    """Turn e.g. '[-1.500,0.500]s' into (-1.5, 0.5)."""
    stripped = field.strip("[]s")
    low_s, high_s = stripped.split(",")
    return float(low_s), float(high_s)


def test_symmetric_then_backward_extends_not_resets(video_compare_binary, lg_daylight_pair):
    """Press `` ` `` (symmetric), then `[` (backward). The `[` press must
    extend the existing searched interval leftward, not start fresh with a
    [-1, 0] window. Evidence: press 2's searched_window high end is
    positive (inherited from the symmetric press) and its low end is
    further back than a fresh `[` would produce."""
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Play ~2 s so follower_current is well away from clip start,
        # giving room to grow the searched interval in both directions.
        vc.key("space")
        vc.sleep(2.0)
        vc.key("space")
        vc.seek_wait(timeout=5.0)

        # Press 1: `` ` `` — symmetric. searched_window = [-0.5, +0.5]s.
        vc.key("`")
        vc.seek_wait()

        # Press 2: `[` — backward. Under cross-mode carry, high end stays
        # at +0.5 (inherited from `` ` ``), low end extends by 1 s to about
        # -1.5 s (= -0.5 - 1.0). Under the OLD behavior, cache would reset
        # and window would be [-1, 0]s.
        vc.key("[")
        vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 2, f"expected 2+ summaries, got {len(summaries)}"

        s1_low, s1_high = _parse_window(summaries[0]["searched_window"])
        s2_low, s2_high = _parse_window(summaries[1]["searched_window"])

        # Press 1: ~[-0.5, +0.5]s on the follower axis.
        assert abs(s1_low - (-0.5)) < 0.1, f"press 1 low end: got {s1_low}, expected ~-0.5"
        assert abs(s1_high - 0.5) < 0.1, f"press 1 high end: got {s1_high}, expected ~0.5"

        # Press 2: high end must still be positive (cache carried). A
        # fresh [ press would have high end = 0.
        assert s2_high > 0.1, (
            f"press 2 high end should be ~+0.5 (inherited from `), got {s2_high}. "
            f"This indicates the cache was reset on mode change instead of "
            f"carrying forward.\nsummary: {summaries[1]}"
        )
        # Press 2: low end must be further back than press 1's low end
        # (the [ press extended leftward by ~1 s).
        assert s2_low < s1_low - 0.5, (
            f"press 2 low end should extend ~1 s past press 1's low end. "
            f"press 1 low={s1_low}, press 2 low={s2_low}\nsummary: {summaries[1]}"
        )


def test_mode_change_does_not_redecode_overlap(video_compare_binary, lg_daylight_pair):
    """When `` ` `` → `[` carries cache forward, the backward press only
    decodes its net-new strip (the 1 s that lies beyond the symmetric
    window's low edge). The candidate pool grows but the ring contribution
    stays at 0 because candidates are keyed by PTS and deduped."""
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        vc.key("space")
        vc.sleep(2.0)
        vc.key("space")
        vc.seek_wait(timeout=5.0)

        vc.key("`")
        vc.seek_wait()
        vc.key("[")
        vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 2

        # Press 2 should report ring=0: the follower ring contribution was
        # already consumed by press 1, and the second press only decodes
        # net-new PTS from the backward extension.
        assert summaries[1]["ring"] == "0", (
            f"press 2 (after `) expected ring=0 (cache carried), "
            f"got ring={summaries[1]['ring']}\nsummary: {summaries[1]}"
        )
        # Candidate count must grow (decoded strip added frames), not reset
        # back to a small value. Press 1 typically has 60-80 candidates;
        # press 2 should have more after adding the backward strip.
        c1 = int(summaries[0]["candidates"])
        c2 = int(summaries[1]["candidates"])
        assert c2 > c1, (
            f"press 2 candidate pool should include press 1's + new backward "
            f"strip. Got press 1={c1}, press 2={c2}."
        )
