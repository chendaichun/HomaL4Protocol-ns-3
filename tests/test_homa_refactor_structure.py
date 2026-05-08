from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class HomaRefactorStructureTest(unittest.TestCase):
    def test_homa_sources_split_into_core_send_recv_files(self):
        model_dir = ROOT / "src" / "internet" / "model"
        self.assertTrue((model_dir / "homa-l4-protocol-core.cc").exists())
        self.assertTrue((model_dir / "homa-l4-protocol-send.cc").exists())
        self.assertTrue((model_dir / "homa-l4-protocol-recv.cc").exists())

    def test_wscript_uses_split_homa_sources(self):
        wscript = (ROOT / "src" / "internet" / "wscript").read_text()
        self.assertIn("'model/homa-l4-protocol-core.cc'", wscript)
        self.assertIn("'model/homa-l4-protocol-send.cc'", wscript)
        self.assertIn("'model/homa-l4-protocol-recv.cc'", wscript)
        self.assertNotIn("'model/homa-l4-protocol.cc'", wscript)

    def test_send_scheduler_header_drops_old_two_step_packet_selection(self):
        header = (ROOT / "src" / "internet" / "model" / "homa-l4-protocol.h").read_text()
        self.assertNotIn("GetNextMsgId (", header)
        self.assertNotIn("GetNextPktOfMsg (", header)
        self.assertIn("GetNextPacket (", header)

    def test_homa_refactor_uses_clearer_api_names(self):
        model_dir = ROOT / "src" / "internet" / "model"
        combined = "\n".join(
            [
                (model_dir / "homa-l4-protocol.h").read_text(),
                (model_dir / "homa-l4-protocol-core.cc").read_text(),
                (model_dir / "homa-l4-protocol-send.cc").read_text(),
                (model_dir / "homa-l4-protocol-recv.cc").read_text(),
            ]
        )
        self.assertNotIn("MemIsOptimized", combined)
        self.assertNotIn("GetTimeToDrainTxQueue", combined)
        self.assertNotIn("CtrlPktRecvdForOutboundMsg", combined)
        self.assertIn("UsesOptimizedMemory", combined)
        self.assertIn("GetTxQueueDrainDelay", combined)
        self.assertIn("HandleControlPacketForOutboundMsg", combined)

    def test_recv_scheduler_uses_clearer_method_names(self):
        model_dir = ROOT / "src" / "internet" / "model"
        combined = "\n".join(
            [
                (model_dir / "homa-l4-protocol.h").read_text(),
                (model_dir / "homa-l4-protocol-recv.cc").read_text(),
            ]
        )
        self.assertNotIn("GetInboundMsg(", combined)
        self.assertNotIn("ScheduleMsgAtIdx(", combined)
        self.assertNotIn("ClearStateForMsg(Ptr<HomaInboundMsg>", combined)
        self.assertNotIn("SendAppropriateGrants(", combined)
        self.assertIn("FindInboundMsg", combined)
        self.assertIn("RescheduleInboundMsg", combined)
        self.assertIn("RemoveInboundMsg", combined)
        self.assertIn("IssuePendingGrants", combined)


if __name__ == "__main__":
    unittest.main()
