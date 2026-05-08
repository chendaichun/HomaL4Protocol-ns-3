from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Sim1PaperWindowTest(unittest.TestCase):
    def test_source_exposes_separate_traffic_and_trace_windows(self):
        sim1 = (ROOT / "scratch" / "sim1.cc").read_text(encoding="utf-8")
        self.assertIn("trafficStartSec", sim1)
        self.assertIn("trafficDurationSec", sim1)
        self.assertIn("traceStartSec", sim1)
        self.assertIn("traceDurationSec", sim1)
        self.assertIn("torQueueIncludeDevice", sim1)

    def test_paper_window_helper_matches_public_formula_for_load50(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            out_json = Path(tmpdir) / "window.json"
            result = subprocess.run(
                [
                    "python3",
                    str(ROOT / "scripts" / "sim1_paper_window.py"),
                    "--workload",
                    "google_rpc",
                    "--offered-load",
                    "0.5",
                    "--start-at",
                    "10.0",
                    "--trace-last-ratio",
                    "0.1",
                    "--json-out",
                    str(out_json),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            payload = out_json.read_text(encoding="utf-8")
            self.assertIn('"traffic_duration_sec": 0.18', payload)
            self.assertIn('"trace_start_sec": 10.162', payload)
            self.assertIn('"trace_duration_sec": 0.018', payload)


if __name__ == "__main__":
    unittest.main()
