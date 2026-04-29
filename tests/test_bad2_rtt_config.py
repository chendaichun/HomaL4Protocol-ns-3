from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Bad2RttConfigTest(unittest.TestCase):
    def test_bad2_script_defaults_target_rtt_heterogeneity_experiment(self):
        script = (ROOT / "scripts" / "bad2.sh").read_text()

        self.assertIn('BOTTLENECK_RATE_GBPS="${BOTTLENECK_RATE_GBPS:-100}"', script)
        self.assertIn('BDP_PKTS="${BDP_PKTS:-33.32}"', script)
        self.assertIn('SHORT_PATH_DELAY_US="${SHORT_PATH_DELAY_US:-1}"', script)
        self.assertIn('LONG_PATH_EXTRA_DELAYS_US="${LONG_PATH_EXTRA_DELAYS_US:-0 4 8 16 32}"', script)
        self.assertIn('GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-1000}"', script)
        self.assertIn('CREDIT_SAMPLE_US="${CREDIT_SAMPLE_US:-500}"', script)
        self.assertIn('TRACE_SWITCH_QUEUE_SAMPLE_US="${TRACE_SWITCH_QUEUE_SAMPLE_US:-500}"', script)
        self.assertIn("scripts/bad2_analyze.py", script)

    def test_bad2_cc_emits_machine_readable_summary_with_sender_roles(self):
        source = (ROOT / "scratch" / "bad2.cc").read_text()

        self.assertIn('".summary.tr"', source)
        self.assertIn('"senderA="', source)
        self.assertIn('"senderB="', source)
        self.assertIn('"receiver="', source)
        self.assertIn('"longPathExtraDelayUs="', source)
        self.assertIn('"bottleneckRateGbps="', source)

    def test_bad2_analyzer_exists_and_reports_utilization_metrics(self):
        analyzer = ROOT / "scripts" / "bad2_analyze.py"
        text = analyzer.read_text()

        self.assertIn("aggregate_utilization", text)
        self.assertIn("sender_a_goodput_gbps", text)
        self.assertIn("sender_b_goodput_gbps", text)
        self.assertIn("receiver_avail_ratio_a_to_b", text)
        self.assertIn("receiver_queue_avg_pkts", text)
        self.assertIn("aggregate_utilization_vs_rtt.png", text)
        self.assertIn("aggregate_utilization_baseline_comparison.png", text)
        self.assertIn("sender_goodput_vs_rtt.png", text)
        self.assertIn("goodput_stack_with_unused_capacity.png", text)
        self.assertIn("normalized_goodput_vs_baseline.png", text)
        self.assertIn("sender_budget_ratio_vs_rtt.png", text)
        self.assertIn("receiver_queue_vs_rtt.png", text)


if __name__ == "__main__":
    unittest.main()
