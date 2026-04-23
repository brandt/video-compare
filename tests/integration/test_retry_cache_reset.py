"""Guards the auto-align retry-cache invalidation rule.

When the previous press returned low_confidence, the retry cache (candidate
pool + master probes + retry_count) is kept so the next same-key press can
*extend* the search window. The cache MUST be invalidated by any intervening
input that moves the master or follower position — otherwise stale probes
would score against the new ring state.

The invalidation is implicit: `can_reuse_cache` requires
`retry.master_pts_at_cache == master_current->pts` and
`retry.follower_pts_at_cache == follower_current->pts`. Any `+`/`-`, seek,
or playback advance breaks those conditions.

This test verifies that `]` (low_confidence) → `+` (shifts follower) → `]`
shows retry=0 on the second press, confirming the cache was invalidated.
"""

from .harness import VideoCompareSession


def test_plus_keypress_invalidates_retry_cache(video_compare_binary, lg_daylight_pair):
    with VideoCompareSession(lg_daylight_pair) as vc:
        vc.seek_wait(timeout=15.0)

        # Press 1: `]` — expected retry=0, decision=low_confidence (window
        # [0, +1s] too narrow to reach the +2069 ms target).
        vc.key("]")
        vc.seek_wait()

        # Intervening input: single `+` shifts follower by one frame.
        vc.key("=")
        vc.seek_wait()

        # Press 2: `]` — the cache from press 1 should be invalidated because
        # the follower moved. Expect retry=0 again (fresh), not retry=1.
        vc.key("]")
        vc.seek_wait()

        summaries = vc.auto_align_summaries()
        assert len(summaries) >= 2, (
            f"expected 2+ auto-align summaries, got {len(summaries)}:\n"
            + "\n".join(str(s) for s in summaries)
        )

        # Press 1: retry=0, low_confidence.
        assert summaries[0]["retry"] == "0"
        assert summaries[0]["decision"] == "low_confidence"

        # Press 2 (after `+`): retry=0 — cache invalidated.
        assert summaries[1]["retry"] == "0", (
            f"cache should have been invalidated by `+`, but press 2 shows "
            f"retry={summaries[1]['retry']} (cache reused).\n"
            f"summary: {summaries[1]}"
        )
