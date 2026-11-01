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


def falling_object(extension, blocks=None):
    """One object (channel 8) whose gain into the left row falls from 1 to 0 over 4,000 samples,
    eight access units: the request the C++ test of the trajectory makes."""
    request = {
        "objects": 9,
        "noise": 0,
        "bypass_bits": 0,
        "mix_step": 0,
        "extension": extension,
        "update_units": 1,
        "drop_idle": False,
        "split": False,
        "nominal": 40,
        "interval_units": 8,
        "paths": {8: [(0, [65536] + [0] * 7), (4000, [0] * 8)]},
    }
    return writer.motion_blocks(request, 0, blocks or writer.interval_bounds(request)), request


class MovingObjects(unittest.TestCase):
    """The reference writer's plan of moving objects, held to values worked out by hand. The C++
    tests (libs/truehd/tests/encoder/test_encoder_object_motion.cpp) hold the same numbers."""

    def test_halves_round_away_from_zero(self):
        got = [writer.round_div(n, d) for n, d in ((5, 2), (-5, 2), (7, 3), (-7, 3))]
        self.assertEqual(got, [3, -3, 2, -2])

    def test_a_path_is_linear_between_knots_holds_outside_and_steps_at_a_repeated_sample(self):
        row = [0] * 7
        knots = [(10, [1000, *row]), (110, [3000, *row]), (110, [6000, *row]), (310, [2000, *row])]

        def gain(sample):
            n, d = writer.path_gain(knots, 0, sample)
            return n / d

        self.assertEqual(
            [gain(s) for s in (0, 10, 60, 109, 110, 210, 400)],
            [1000, 1000, 2000, 2980, 6000, 4000, 2000],
        )

    def test_a_coefficient_slides_on_its_slope_and_a_section_is_sent_where_it_stops_reaching(self):
        blocks, _request = falling_object("slopes")
        sent = [place for place, block in enumerate(blocks) if block["section"] is not None]
        self.assertEqual(sent, [0, 7])
        first, later = blocks[0]["section"], blocks[7]["section"]
        writer.resolve_sections(
            {blocks[place]["start"]: blocks[place]["section"] for place in sent}, {0}
        )
        # the restart: A, B, A2, B2 and C2, the raw value -4096 and a delta of 328 (2,624 units a
        # unit of 2^-18 at S = 15), the width 9 and scale 3
        self.assertEqual((first.a, first.b, first.a2, first.b2, first.c2), (True,) * 5)
        self.assertEqual(first.resolved[1]["raw"][8], -4096)
        self.assertEqual(first.resolved[1]["delta"][8], 328)
        self.assertEqual((first.resolved[1]["width"], first.resolved[1]["field2"]), (9, 3))
        # the running coefficient at the start of units 1 and 2
        self.assertEqual(blocks[2]["runs"][1]["coeff"][8], -262144 + 2624)
        self.assertEqual(blocks[3]["runs"][1]["coeff"][8], -262144 + 2 * 2624)
        # the second section: the slope alone (A clear, A2, B2, no C2), a delta of 326
        self.assertEqual((later.a, later.a2, later.b2, later.c2), (False, True, True, False))
        self.assertEqual(later.deltas[1][8], 326)

    def test_without_slopes_a_coefficient_holds_and_a_held_block_sends_nothing(self):
        blocks, request = falling_object("none", [(0, 8), (8, 32), (40, 40), (80, 40), (120, 40)])
        request["update_units"] = 2
        blocks = writer.motion_blocks(request, 0, [(0, 8), (8, 32), (40, 40), (80, 40), (120, 40)])
        self.assertEqual(
            [b["section"] is not None for b in blocks], [True, False, False, True, False]
        )
        raw = [b["runs"][1]["coeff"][8] // 64 for b in blocks]
        self.assertEqual(raw, [-4096, -4096, -4096, -4014, -4014])

    def test_a_slope_adds_its_share_across_the_unit_and_moves_the_coefficient_at_its_end(self):
        head = writer.moving_head(0, 12, 0, 0, 0, {0, 8}, 6, 3)
        run = {
            "coeff": {0: 4096 << 6, 8: 100 << 6},
            "slope": {8: 40},
            "extension": True,
            "scale": 15,
            "ramp": 0,
        }
        step = writer.ramp_step(40)
        self.assertEqual(step, 6552)
        row = [0] * 9
        row[8] = 1 << 20
        values = [writer.moving_value(head, run, row, i, step, False, 0) for i in (0, 1, 2, 10, 39)]
        self.assertEqual(values, [25600, 25631, 25663, 25919, 26847])
        writer.advance_run(head, run, 40, True, step)
        self.assertEqual(run["coeff"][8], 105 << 6)

    def sections_bits(self, first, second):
        sections = {0: first, 40: second}
        writer.resolve_sections(sections, {0})
        return writer.section_bits(second, 12)

    def restart(self):
        return writer.ObjectSection(
            coeffs=[{0: 5, 8: 1}],
            headers=[(9, 12, 0, 0)],
            a2=True,
            b2=True,
            c2=True,
            widths=[3],
            field2=[0],
            deltas=[{0: 0, 8: 0}],
        )

    def test_new_slopes_on_the_set_that_stands(self):
        second = writer.ObjectSection(a=False, a2=True, b2=True, deltas=[{0: -2, 8: 5}])
        self.assertEqual(self.sections_bits(self.restart(), second), "011011100101")

    def test_new_coefficients_on_the_headers_that_stand_with_the_extension_off(self):
        second = writer.ObjectSection(coeffs=[{0: 5, 8: -3}], b=False)
        self.assertEqual(
            self.sections_bits(self.restart(), second), "1000000000000101111111111111010"
        )

    def test_new_coefficients_with_the_standing_slopes(self):
        second = writer.ObjectSection(coeffs=[{0: 5, 8: -3}], b=False, a2=True)
        self.assertEqual(
            self.sections_bits(self.restart(), second),
            "10000000000001011111111111110110",
        )

    def test_new_widths_and_scales_with_their_slopes(self):
        second = writer.ObjectSection(
            a=False, a2=True, b2=True, c2=True, widths=[3], field2=[2], deltas=[{0: -2, 8: 5}]
        )
        self.assertEqual(self.sections_bits(self.restart(), second), "011100111011100101")

    def test_the_writer_and_the_plan_agree_on_every_case(self):
        for name in writer.MOTION_CASES:
            # case_moving asserts that the decoder session's model of the running state is the
            # one kept here, sample for sample
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
