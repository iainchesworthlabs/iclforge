"""crypto_bench_report.py: which pass is read, and what the tables say.

stdlib unittest, as the other suites of this directory are, so that the Oracle unit tests step finds
them. No board and no network: the logs are the lines the bench prints.

Run: python3 -m unittest discover -s tools/hearth -p 'test_crypto_bench_report.py'
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import crypto_bench_report as report

LOG = """\
BENCH,esp32p4,360,sha256.psa,internal,64,9999,9999,3,1.00
INFO,esp32p4,1,v6.1,360,300000,8000000,2,0
CHECK,sha256,64,1
CHECK,sha256.flash,4096,1
BENCH,esp32p4,360,sha256.base,internal,2304,100000,100100,50,8.28
BENCH,esp32p4,360,sha256.psa,internal,2304,10000,10050,500,82.80
BENCH,esp32p4,360,sha256.psa,psram,2304,20000,20050,250,41.40
BENCH,esp32p4,360,sha256.psa,flash,2304,40000,40050,125,20.70
BENCH,esp32p4,360,sha256.psa@xtal,internal,2304,25000,25050,200,33.12
LEFT,esp32p4,sha256.psa,4096,0.4
LEFT,esp32p4,aead.aes256gcm.psa,4096,71.5
FOOT,esp32p4,empty,0,0
FOOT,esp32p4,sha256.psa,300,128
DONE
INFO,esp32p4,1,v6.1,360,300000,8000000,2,0
CHECK,sha256,64,0
BENCH,esp32p4,360,sha256.psa,internal,64,1,1,1,1.00
"""

C6_LOG = (
    LOG.replace("esp32p4", "esp32c6").replace(",360,", ",160,").replace("8000000,2,0", "0,-1,0")
)


class ParsePassTest(unittest.TestCase):
    def test_last_whole_pass_is_the_one_read(self):
        run = report.parse_pass(LOG)
        self.assertIsNotNone(run)
        # The second pass never reached DONE, and the line before the first INFO has no pass to
        # join.
        self.assertEqual([ok for _, _, ok in run.checks], [True, True])
        self.assertEqual(run.mhz, 360)
        self.assertEqual(run.crypto_clock, 2)
        self.assertNotIn(("sha256.psa", "internal", 64), run.cases)

    def test_the_consoles_timestamps_are_ignored(self):
        stamped = "\n".join(f"[{i * 1.5:7.2f}] {line}" for i, line in enumerate(LOG.splitlines()))
        plain = report.parse_pass(LOG)
        run = report.parse_pass(stamped)
        self.assertIsNotNone(plain)
        self.assertIsNotNone(run)
        self.assertEqual(run.cases, plain.cases)
        self.assertEqual(run.left, plain.left)

    def test_a_log_with_no_whole_pass_reads_as_none(self):
        self.assertIsNone(report.parse_pass("INFO,esp32s3,2,v6.1,240,1,1,-1,0\nBENCH,x\n"))


class TablesTest(unittest.TestCase):
    def test_tables_carry_the_speedup_and_the_failures(self):
        run = report.parse_pass(LOG)
        self.assertIsNotNone(run)
        text = report.render({"esp32p4": run}, None)
        self.assertIn("10.0x", text)  # 100000 / 10000 cycles
        self.assertIn("all equal", text)
        run.checks.append(("hmac_sha256", 800, False))
        self.assertIn("hmac_sha256 800 B", report.render({"esp32p4": run}, None))

    def test_where_the_message_lives_and_the_crypto_clock(self):
        run = report.parse_pass(LOG)
        self.assertIsNotNone(run)
        memory = report.memory_table({"esp32p4": run})
        self.assertIsNotNone(memory)
        self.assertIn("| esp32p4 | 2304 | 82.80 | 41.40 | 20.70 |", memory)
        clock = report.clock_table({"esp32p4": run})
        self.assertIsNotNone(clock)
        self.assertIn("| esp32p4 | 2 | 2304 | 25,000 | 10,000 | 2.5x |", clock)

    def test_a_chip_without_the_clock_selector_has_no_clock_table(self):
        run = report.parse_pass(C6_LOG)
        self.assertIsNotNone(run)
        self.assertEqual(run.crypto_clock, -1)
        self.assertIsNone(report.clock_table({"esp32c6": run}))

    def test_cpu_left_is_laid_out_by_chip(self):
        p4 = report.parse_pass(LOG)
        c6 = report.parse_pass(C6_LOG)
        self.assertIsNotNone(p4)
        self.assertIsNotNone(c6)
        text = report.left_table({"esp32p4": p4, "esp32c6": c6})
        self.assertIsNotNone(text)
        self.assertIn("| aead.aes256gcm.psa | 4096 | 72 % | 72 % |", text)
        self.assertIn("| sha256.psa | 4096 | 0 % | 0 % |", text)

    def test_the_aead_is_priced_at_the_lanes_burst_sizes(self):
        run = report.Pass(info=["esp32p4", "1", "v6.1", "100"])
        # 100 MHz, so a cycle is 10 ns: ChaCha 1,000 cycles at 1,000 B and 5,000 at 5,000 B (1 cycle
        # a byte over the line), GCM 500 and 900.
        for size, chacha, gcm in ((1000, 1000, 500), (5000, 5000, 900)):
            run.cases[("aead.chacha20poly1305.psa", "internal", size)] = report.Case(
                chacha, 0.0, 1, 0.0
            )
            run.cases[("aead.aes256gcm.psa", "internal", size)] = report.Case(gcm, 0.0, 1, 0.0)
        self.assertEqual(report.cycles_at(run, "aead.chacha20poly1305.psa", 3000), 3000)
        # 900 + 0.1 a byte, past the end
        self.assertEqual(report.cycles_at(run, "aead.aes256gcm.psa", 7000), 1100)
        text = report.capacity_table({"esp32p4": run})
        self.assertIsNotNone(text)
        # 3,072 B: ChaCha 3,072 cycles = 30.72 us = 0.096 % of a core at 31.25 bursts a second.
        self.assertIn("| 768 kb/s lane (3072 B) | esp32p4 | 31 |", text)
        self.assertIn("0.1 %", text)
        self.assertIsNone(report.capacity_table({}))

    def test_pc_section_pairs_portable_with_hardware(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "pc.json"
            path.write_text(
                json.dumps(
                    {
                        "kernels": [
                            {
                                "name": "sha256_compress_portable_1024B",
                                "iters": 1,
                                "ns_per_call": 3000.0,
                            },
                            {
                                "name": "sha256_compress_hardware_1024B",
                                "iters": 1,
                                "ns_per_call": 500.0,
                            },
                            {"name": "hmac_sha256_2304B", "iters": 1, "ns_per_call": 1300.0},
                        ]
                    }
                ),
                encoding="utf-8",
            )
            text = report.pc_section(path)
        self.assertIn("6.0x", text)
        self.assertIn("HMAC-SHA-256 (dispatching), 2304 B", text)


if __name__ == "__main__":
    unittest.main()
