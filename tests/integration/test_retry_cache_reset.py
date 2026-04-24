"""Guards the auto-align retry-cache invalidation rule.

The cache of candidate fingerprints + master probes + searched interval
is kept across consecutive presses so each press only decodes net-new
territory. The cache MUST be invalidated by any intervening input that
moves the master or follower position outside what auto-align itself
produced — otherwise stale probes would score against the wrong ring state.

The invalidation is implicit: `can_reuse_cache` requires
`retry.master_pts_at_cache == master_current->pts` and
`retry.follower_pts_at_cache == follower_current->pts` (the latter is
updated post-seek to the landed PTS). Any `+`/`-`, manual seek, or
playback advance breaks those conditions.

This test verifies that `]` (low_confidence) → `+` (shifts follower) → `]`
resets the searched_window to [0, +1]s on the second press, confirming
the cache was invalidated rather than extending to [0, +2]s.
"""

from .harness import VideoCompareSession


def test_plus_keypress_invalidates_retry_cache(video_compare_binary, lg_daylight_pair):
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Press 1: `]` — fresh cache, searched_window=[0, +1]s, low_confidence
        # (window too narrow to reach the +2069 ms target).
        vc.key("]")
        vc.seek_wait()

        # Intervening input: single `+` shifts follower by one frame.
        vc.key("=")
        vc.seek_wait()

        # Press 2: `]` — the cache from press 1 should be invalidated because
        # the follower moved. Expect searched_window=[0, +1]s again (fresh),
        # not [0, +2]s (extension).
        vc.key("]")
        vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 2, (
            f"expected 2+ auto-align summaries, got {len(summaries)}:\n"
            + "\n".join(str(s) for s in summaries)
        )

        # Press 1: fresh cache, searched_window=[0, +1]s.
        assert summaries[0]["searched_window"] == "[0.000,1.000]s"
        assert summaries[0]["decision"] == "low_confidence"

        # Press 2 (after `+`): searched_window=[0, +1]s again — cache was
        # invalidated, not extended to [0, +2]s.
        assert summaries[1]["searched_window"] == "[0.000,1.000]s", (
            f"cache should have been invalidated by `+`, but press 2 shows "
            f"searched_window={summaries[1]['searched_window']} (would be "
            f"[0.000,2.000]s if the cache had been reused).\n"
            f"summary: {summaries[1]}"
        )
