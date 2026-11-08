#!/usr/bin/env python3
"""Check selector wiring and output admission for the read-only PCIe query."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/cx3_abi.h").read_text()
CLIENT = (ROOT / "src/Cx3Mlx4UserClient.cpp").read_text()
PROVIDER = (ROOT / "src/Cx3Mlx4.cpp").read_text()
CLI = (ROOT / "tools/cx3ctl.c").read_text()

class PcieExportTests(unittest.TestCase):
    def test_abi_appends_selector_and_exact_fixed_layout(self):
        self.assertRegex(ABI, r"kCx3SelPcieLink\s*=\s*48")
        count = re.search(r"kCx3SelCount\s*=\s*(\d+)", ABI)
        self.assertIsNotNone(count)
        self.assertGreater(int(count[1]), 48)
        self.assertIn("sizeof(struct Cx3PcieLinkResult) == 368", ABI)
        self.assertIn("offsetof(struct Cx3PcieLinkResult, raw) == 80", ABI)

    def test_dispatch_has_zero_scalars_and_exact_structure_output(self):
        self.assertRegex(CLIENT, r"sPcieLink,\s*0,\s*0,\s*0,\s*\(uint32_t\)sizeof\(Cx3PcieLinkResult\)")
        self.assertIn("/* 48 pcielink */ kXactTryLock", CLIENT)
        body = CLIENT.split("IOReturn Cx3Mlx4UserClient::doPcieLink", 1)[1].split("\n}", 1)[0]
        for required in ("scalarInputCount != 0", "scalarOutputCount != 0",
                         "structureInput", "structureInputSize != 0",
                         "structureOutputSize != sizeof(Cx3PcieLinkResult)"):
            self.assertIn(required, body)

    def test_provider_refuses_reset_retention_before_hardware(self):
        body = PROVIDER.split("IOReturn Cx3Mlx4::exportPcieLink", 1)[1].split("\n}", 1)[0]
        self.assertLess(body.index("fResetCommandsBlocked"), body.index("fHw.queryPcieLink"))
        self.assertLess(body.index("retentionExhausted()"), body.index("fHw.queryPcieLink"))
        self.assertLess(body.index("if (!fPci)"), body.index("fHw.queryPcieLink"))

    def test_cli_command_rejects_extra_arguments_and_reports_sample_limits(self):
        self.assertIn('strcmp(cmd, "pcielink")', CLI)
        self.assertIn('if (argc != 2)', CLI)
        self.assertIn("non-atomic", CLI)
        self.assertIn("meaning unverified", CLI)
        self.assertIn("provider-local bookkeeping only", CLI)

if __name__ == "__main__": unittest.main(verbosity=2)
