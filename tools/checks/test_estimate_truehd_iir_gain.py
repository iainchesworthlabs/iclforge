"""Unit tests for estimate_truehd_iir_gain.py: the pieces that its numbers rest on."""

import sys
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import estimate_truehd_iir_gain as E


class EstimateIirGainTest(unittest.TestCase):
    def test_levinson_recovers_the_coefficients_of_an_ar_signal(self):
        rng = np.random.default_rng(1)
        noise = rng.standard_normal(20000)
        x = np.zeros_like(noise)
        for n in range(2, len(x)):
            x[n] = 1.2 * x[n - 1] - 0.5 * x[n - 2] + noise[n]
        a = E.fit_fir(x, 2)
        self.assertAlmostEqual(a[0], 1.2, delta=0.02)
        self.assertAlmostEqual(a[1], -0.5, delta=0.02)

    def test_the_integer_recursion_is_undone_by_the_decoders(self):
        # The decoder keeps the past signal for filter A and the past residuals for filter B.
        rng = np.random.default_rng(2)
        x = np.rint(np.cumsum(rng.standard_normal(2000)) * 50).astype(np.int64)
        a = np.array([0.9, -0.2, 0.05])
        b = np.array([0.3, -0.1])
        q, ai, bi = E.quantise(a, b)
        r = E.residual(x, (q, ai, bi), context=E.CONTEXT)
        rebuilt = list(x[: E.CONTEXT])
        past_r = [0] * E.CONTEXT
        for value in r:
            acc = sum(int(ai[i - 1]) * rebuilt[-i] for i in range(1, len(ai) + 1))
            acc += sum(int(bi[j - 1]) * past_r[-j] for j in range(1, len(bi) + 1))
            rebuilt.append(int(value) + (acc >> q))
            past_r.append(int(value))
        self.assertEqual(rebuilt[E.CONTEXT :], x[E.CONTEXT :].tolist())

    def test_coefficients_the_format_cannot_hold_are_refused(self):
        self.assertIsNone(E.quantise(np.array([200.0]), np.zeros(0)))  # needs coeff_q below 8
        q, a, b = E.quantise(np.array([1.5, -0.5]), np.array([0.25]))
        self.assertEqual(q, 14)  # 32767 / 1.5 is just over 2^14
        self.assertEqual(a.tolist(), [24576, -8192])
        self.assertEqual(b.tolist(), [4096])

    def test_a_filter_b_with_a_pole_outside_the_unit_circle_is_not_stable(self):
        self.assertTrue(E.stable(np.array([0.5])))  # root at -0.5
        self.assertFalse(E.stable(np.array([-1.5])))  # root at 1.5
        self.assertTrue(E.stable(np.zeros(0)))

    def test_bits_grow_with_the_size_of_the_residual(self):
        small = np.full(800, 3, dtype=np.int64)
        large = np.full(800, 300, dtype=np.int64)
        self.assertLess(E.estimated_bits(small), E.estimated_bits(large))
        self.assertEqual(
            E.estimated_bits(np.zeros(800, dtype=np.int64)),
            E.estimated_bits(np.zeros(800, dtype=np.int64)),
        )


if __name__ == "__main__":
    unittest.main()
