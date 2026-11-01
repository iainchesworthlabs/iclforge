"""Tests for tools/checks/verify_truehd_object_bed.py.

The render the check computes from the definition is held to values worked out by hand. The
integration test encodes with iclforge-truehd-encode-tool (ICLFORGE_TRUEHD_ENCODE_TOOL, or
build/bin) and is skipped without it."""

import os
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import verify_truehd_object_bed as check

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = pathlib.Path(
    os.environ.get("ICLFORGE_TRUEHD_ENCODE_TOOL")
    or REPO / "build" / "bin" / "iclforge-truehd-encode-tool"
)


class Render(unittest.TestCase):
    def test_a_silent_programme_has_a_silent_bed(self):
        self.assertEqual(check.expected_bed([[0] * 12]), [[0] * 8])

    def test_an_object_above_the_bed_is_taken_into_its_row_less_the_floored_gain(self):
        # Object 8 (index 0 of the extra objects) goes to row 0. The decoder adds
        # floor(-2896 * 4096 / 4096) = -2896 to the coded row, so the coded row is the object
        # channel plus 2896; the rounding of a value that does not divide is a floor.
        row = [100, 0, 0, 0, 0, 0, 0, 0, 4096, 0, 0, 0]
        self.assertEqual(check.expected_bed([row])[0][0], 100 + 2896)
        row = [0] * 8 + [1, 0, 0, 0]
        self.assertEqual(check.expected_bed([row])[0][0], 1)  # floor(-2896 / 4096) = -1
        row = [0] * 8 + [-1, 0, 0, 0]
        self.assertEqual(check.expected_bed([row])[0][0], 0)  # floor(2896 / 4096) = 0

    def test_the_objects_reach_the_rows_in_the_order_of_the_table(self):
        row = [0] * 8 + [4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096]
        bed = check.expected_bed([row])[0]
        self.assertEqual(bed, [2896] * 8)

    def test_the_audio_is_halved_to_leave_room_for_the_bed(self):
        rows = check.object_rows(12)
        self.assertEqual(len(rows[0]), 12)
        self.assertTrue(all(-(1 << 22) <= v < (1 << 22) for row in rows[:2000] for v in row))


writer = check.truehd_writer


class MovingObjects(unittest.TestCase):
    """The reference writer's plan of moving objects, held to values worked out by hand. The C++
    tests
    (libs/truehd/tests/encoder/test_encoder_object_motion.cpp) hold the same numbers."""

    def falling(self, ramp, mix_step=0):
        knots = [(0, [65536, 0, 0, 0, 0, 0, 0, 0]), (4000, [0] * 8)]
        return [
            writer.trajectory_coefficient(
                writer.path_gain(knots, 0, a), writer.path_gain(knots, 0, b), mix_step, True
            )
            for a, b in ramp
        ]

    def test_halves_round_away_from_zero(self):
        self.assertEqual(
            [writer.round_div(n, d) for n, d in ((5, 2), (-5, 2), (7, 3), (-7, 3))], [3, -3, 2, -2]
        )

    def test_a_path_is_linear_between_knots_holds_outside_and_steps_at_a_repeated_sample(self):
        knots = [
            (10, [1000] + [0] * 7),
            (110, [3000] + [0] * 7),
            (110, [6000] + [0] * 7),
            (310, [2000] + [0] * 7),
        ]

        def gain(sample):
            n, d = writer.path_gain(knots, 0, sample)
            return n / d

        self.assertEqual(
            [gain(s) for s in (0, 10, 60, 109, 110, 210, 400)],
            [1000, 1000, 2000, 2980, 6000, 4000, 2000],
        )

    def test_the_ramp_to_the_last_sample_anchors_a_sample_early(self):
        # blocks (0, 8), (8, 32), (40, 40): the anchors are -1 to 7, 7 to 39, 39 to 79
        got = self.falling([(-1, 7), (7, 39), (39, 79)])
        self.assertEqual(got, [(-4096, 57), (-4089, 263), (-4056, 327)])

    def test_the_ramp_from_the_first_sample_anchors_at_the_block(self):
        self.assertEqual(self.falling([(0, 8), (8, 40)]), [(-4096, 66), (-4088, 264)])

    def test_the_last_mix_step_mixes_nothing(self):
        self.assertEqual(self.falling([(0, 8)], mix_step=3), [(0, 0)])
        self.assertEqual(self.falling([(0, 8)], mix_step=1)[0][0], -2048)

    def test_a_width_holds_its_deltas_and_no_more(self):
        self.assertEqual(
            [writer.width_of(d) for d in ([0], [1], [-2], [2], [-3], [327], [-512], [512])],
            [0, 1, 1, 2, 2, 9, 9, 10],
        )

    def test_a_section_with_new_deltas_on_the_set_that_stands(self):
        before = [writer.moving_matrix(9, 12, 0, 0, 0, {0: 5, 8: 1}, 3, {0: 0, 8: 0})]
        after = [writer.moving_matrix(9, 12, 0, 0, 0, {0: 5, 8: 1}, 3, {0: -2, 8: 5})]
        sections = {
            0: writer.plan_object_section([], before),
            40: writer.plan_object_section(before, after),
        }
        writer.resolve_sections(sections, {0})
        # A clear, then A2 B2 and no C2 (the widths stand): 0 1 1 0, and the deltas of 4 bits
        self.assertEqual(writer.section_bits(sections[40], 12), "0110" + "1110" + "0101")

    def test_a_section_with_new_coefficients_on_the_headers_that_stand(self):
        before = [writer.moving_matrix(9, 12, 0, 0, 0, {0: 1, 8: 1})]
        after = [writer.moving_matrix(9, 12, 0, 0, 0, {0: 5, 8: -3})]
        sections = {
            0: writer.plan_object_section([], before),
            40: writer.plan_object_section(before, after),
        }
        writer.resolve_sections(sections, {0})
        # A set, B clear, the two coefficients in 14 bits, no extension
        self.assertEqual(
            writer.section_bits(sections[40], 12), "10" + "00000000000101" + "11111111111101" + "0"
        )

    def test_nothing_is_sent_when_nothing_changes(self):
        matrix = [writer.moving_matrix(9, 12, 0, 0, 0, {0: 5, 8: 1})]
        self.assertIsNone(writer.plan_object_section(matrix, [dict(m) for m in matrix]))

    def test_the_writer_and_the_plan_agree_on_every_case(self):
        for name in writer.MOTION_CASES:
            # case_moving asserts that the writer's state is the plan's
            plan = writer.case_moving(name)[3]
            self.assertGreater(len(plan["sent"]), 0, name)


@unittest.skipUnless(TOOL.exists(), "iclforge-truehd-encode-tool is not built")
class Integration(unittest.TestCase):
    def test_the_reference_decodes_the_streams_the_encoder_writes(self):
        original = sys.argv
        sys.argv = ["verify_truehd_object_bed.py", "--encoder", str(TOOL)]
        try:
            self.assertEqual(check.main(), 0)
        finally:
            sys.argv = original


if __name__ == "__main__":
    unittest.main()
