from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Sim1PaperAnalysisTest(unittest.TestCase):
    def test_sim1_source_emits_paper_aligned_goodput_fields(self):
        sim1 = (ROOT / "scratch" / "sim1.cc").read_text()
        self.assertIn("perHostGoodputGbps", sim1)
        self.assertIn("aggregateGoodputGbps", sim1)
        self.assertIn("numHosts=", sim1)
        self.assertIn("peakBytes=", sim1)
        self.assertIn("deviceBytes=", sim1)

    def test_sim1_analyze_extracts_paper_goodput_and_queue(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            trace_dir = root / "traces"
            out_csv = root / "summary.csv"
            trace_dir.mkdir()

            (trace_dir / "sim1_balanced_google_rpc_load50.goodput.tr").write_text(
                textwrap.dedent(
                    """\
                    100000000 aggregateGoodputGbps=1000 perHostGoodputGbps=10 completedBytes=12500000 numHosts=100
                    200000000 aggregateGoodputGbps=2000 perHostGoodputGbps=20 completedBytes=37500000 numHosts=100
                    300000000 aggregateGoodputGbps=1500 perHostGoodputGbps=15 completedBytes=56250000 numHosts=100
                    """
                ),
                encoding="utf-8",
            )
            (trace_dir / "sim1_balanced_google_rpc_load50.tor-egress-queue.tr").write_text(
                textwrap.dedent(
                    """\
                    50000000 queue=tor0_to_host7 role=tor_to_host packets=99 bytes=999999
                    100000000 queue=tor0_to_host7 role=tor_to_host packets=1 bytes=500000 qdiscBytes=300000 deviceBytes=200000
                    100000000 queue=aggregate maxPackets=1 meanPackets=1 numQueues=1
                    200000000 queue=tor0_to_host7 role=tor_to_host packets=2 bytes=2500000 qdiscBytes=1500000 deviceBytes=1000000
                    200000000 queue=aggregate maxPackets=2 meanPackets=2 numQueues=1
                    400000000 queue=tor0_to_host7 role=tor_to_host peakBytes=3500000 peakPackets=3 peakTimeNs=210000000
                    """
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    "python3",
                    str(ROOT / "scripts" / "sim1_analyze.py"),
                    "--trace-dir",
                    str(trace_dir),
                    "--out-csv",
                    str(out_csv),
                    "--start-sec",
                    "0.1",
                    "--duration-sec",
                    "0.2",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            content = out_csv.read_text(encoding="utf-8")
            self.assertIn("balanced_google_rpc_load50", content)
            self.assertIn("20.000000", content)
            self.assertIn("0.012153", content)
            self.assertIn("3.500000", content)


if __name__ == "__main__":
    unittest.main()
