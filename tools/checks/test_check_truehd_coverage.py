"""Tests for tools/checks/check_truehd_coverage.py and truehd_census.py on small committed streams.

The full sweep of testdata/truehd is the check itself (about half a minute); these cases prove the
two halves it rests on: the census reads what a stream carries, and the check reports a feature
that no stream carries."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_truehd_coverage as coverage
import truehd_census

HANDBUILT = pathlib.Path(__file__).resolve().parents[2] / "testdata" / "truehd" / "handbuilt"


def census(name):
    return truehd_census.census_of((HANDBUILT / name).read_bytes())


class Census(unittest.TestCase):
    def test_reads_the_orders_of_a_channels_filters(self):
        record = census("hb_filter_orders.thd")
        self.assertEqual(record["iir_orders"], [1, 3, 4])
        self.assertEqual(record["fir_orders"], [4, 5, 8])
        self.assertIn((4, 4), record["combined_orders"])
        self.assertEqual(record["channels"], 6)

    def test_reads_the_framing_features(self):
        self.assertGreater(census("hb_extra_data.thd")["extra_data_units"], 0)
        self.assertGreater(census("hb_drc_word.thd")["drc_word_units"], 0)
        self.assertTrue(census("hb_data_check.thd")["data_check"])
        marker = census("hb_marker_last.thd")
        self.assertEqual((marker["marker_units"], marker["padding_samples"]), (1, 17))

    def test_reads_the_stream_shape(self):
        record = census("hb_eight_channels.thd")
        self.assertEqual((record["channels"], record["substreams"]), (8, 3))
        self.assertIn(0x31EB, record["restart_syncs"])
        self.assertEqual(census("hb_noise_generator_0.thd")["noise_coefficients"], True)

    def test_the_census_sees_noise_generator_1_and_the_v37_streams(self):
        noise = census("hb_noise_generator_1_96k.thd")
        self.assertTrue(noise["matrix_noise_shift"])
        self.assertEqual(noise["sample_rate"], 96000)
        self.assertTrue(census("hb_noise_type1_lsb_bypass.thd")["lsb_bypass_noise_type"])
        self.assertFalse(census("hb_noise_type1.thd")["matrix_noise_shift"])
        self.assertTrue(census("hb_iir_state.thd")["iir_state_shift"])
        self.assertFalse(census("hb_filters.thd")["iir_state"])
        self.assertTrue(census("hb_output_shift_negative.thd")["output_shift_negative"])
        self.assertTrue(census("hb_output_shift_permuted.thd")["output_shift_on_moved_channel"])
        self.assertFalse(census("hb_matrices.thd")["output_shift_on_moved_channel"])
        self.assertTrue(census("hb_ch_assign_changes.thd")["ch_assign_changes"])
        self.assertFalse(census("hb_output_shift_permuted.thd")["ch_assign_changes"])
        self.assertTrue(census("hb_mlp_word16.thd")["mlp_word16_low_bits"])
        self.assertFalse(census("hb_filters.thd")["mlp_word16_low_bits"])


class Evaluate(unittest.TestCase):
    def test_a_feature_no_stream_carries_is_missing(self):
        records = {"a.thd": census("hb_filters.thd")}
        missing, _closed, carriers = coverage.evaluate(records)
        self.assertIn("IIR order 4", missing)  # hb_filters has IIR order 2 alone
        self.assertEqual(carriers["IIR order 2"], ["a.thd"])
        self.assertIn("sample rate 88200 Hz", missing)

    def test_adding_the_stream_that_carries_it_clears_the_feature(self):
        records = {"a.thd": census("hb_filters.thd")}
        missing, _closed, _carriers = coverage.evaluate(records)
        self.assertIn("IIR order 3", missing)
        records["b.thd"] = census("hb_filter_orders.thd")
        missing, _closed, carriers = coverage.evaluate(records)
        self.assertNotIn("IIR order 3", missing)
        self.assertEqual(carriers["IIR order 3"], ["b.thd"])

    def test_a_known_gap_that_a_stream_closes_is_reported(self):
        record = dict(census("hb_filters.thd"), fir_state=True)
        _missing, closed, _carriers = coverage.evaluate({"a.thd": record})
        self.assertEqual(closed, ["FIR filter state"])


if __name__ == "__main__":
    unittest.main()
