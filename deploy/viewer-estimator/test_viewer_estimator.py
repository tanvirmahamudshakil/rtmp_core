import json
import os
import tempfile
import unittest
from unittest import mock

import viewer_estimator as estimator


class ViewerEstimatorTest(unittest.TestCase):
    def setUp(self):
        estimator.seen.clear()
        estimator.delivered_bytes.clear()
        estimator.traffic_buckets.clear()

    def test_successful_cache_delivery_has_session_bytes_and_master_key(self):
        event = estimator.parse_event(
            "GET\t200\t188000\t/hls/live/demo_720p/segment-42.ts"
            "?viewer_session=0123456789abcdef0123456789abcdef&viewer_stream=demo"
        )
        self.assertEqual(event, ("live/demo", "0123456789abcdef0123456789abcdef", 188000))

    def test_non_delivery_and_malformed_sessions_are_ignored(self):
        self.assertIsNone(estimator.parse_event("GET\t404\t12\t/hls/live/demo/segment-1.ts"))
        self.assertIsNone(
            estimator.parse_event(
                "HEAD\t200\t188000\t/hls/live/demo/segment-1.ts"
                "?viewer_session=0123456789abcdef0123456789abcdef&viewer_stream=demo"
            )
        )
        self.assertIsNone(
            estimator.parse_event(
                "GET\t200\t188000\t/hls/live/demo/segment-1.ts?viewer_session=bad&viewer_stream=demo"
            )
        )

    def test_snapshot_deduplicates_viewers_and_reports_cache_bandwidth(self):
        now = 100.5
        estimator.seen["live/demo"].update({"session-a": now, "session-b": now - 1})
        estimator.seen["live/expired"]["old"] = now - estimator.WINDOW_SECONDS - 1
        estimator.delivered_bytes["live/demo"] = 3000
        estimator.traffic_buckets["live/demo"].update({99: 1000, 100: 2000})

        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, "viewer_estimate.json")
            with mock.patch.object(estimator, "OUTPUT_PATH", output), mock.patch.object(
                estimator.time, "time", return_value=1_700_000_000.0
            ):
                estimator.prune_and_write(now)
            with open(output, encoding="utf-8") as snapshot_file:
                snapshot = json.load(snapshot_file)

        self.assertEqual(snapshot["viewers"], {"live/demo": 2})
        self.assertEqual(snapshot["bytes_total"], {"live/demo": 3000})
        self.assertGreater(snapshot["bitrate_bps"]["live/demo"], 0)
        self.assertNotIn("live/expired", snapshot["viewers"])


if __name__ == "__main__":
    unittest.main()
