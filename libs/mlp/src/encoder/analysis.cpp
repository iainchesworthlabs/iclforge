#include "encoder/analysis.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#include "iclforge/mlp/core/stream_info.hpp"

namespace iclforge::mlp::detail {

namespace {

// Solves the symmetric system a x = b in place by Gaussian elimination with partial pivoting.
// nullopt when a pivot vanishes.
std::optional<std::vector<double>> solve(std::vector<double> a, std::vector<double> b, int n) {
    const auto at = [n](int r, int c) { return static_cast<std::size_t>(r * n + c); };
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r) {
            if (std::abs(a[at(r, col)]) > std::abs(a[at(pivot, col)])) {
                pivot = r;
            }
        }
        if (std::abs(a[at(pivot, col)]) < 1e-9) {
            return std::nullopt;
        }
        if (pivot != col) {
            for (int c = 0; c < n; ++c) {
                std::swap(a[at(col, c)], a[at(pivot, c)]);
            }
            std::swap(b[static_cast<std::size_t>(col)], b[static_cast<std::size_t>(pivot)]);
        }
        for (int r = col + 1; r < n; ++r) {
            const double f = a[at(r, col)] / a[at(col, col)];
            for (int c = col; c < n; ++c) {
                a[at(r, c)] -= f * a[at(col, c)];
            }
            b[static_cast<std::size_t>(r)] -= f * b[static_cast<std::size_t>(col)];
        }
    }
    std::vector<double> x(static_cast<std::size_t>(n));
    for (int r = n - 1; r >= 0; --r) {
        double s = b[static_cast<std::size_t>(r)];
        for (int c = r + 1; c < n; ++c) {
            s -= a[at(r, c)] * x[static_cast<std::size_t>(c)];
        }
        x[static_cast<std::size_t>(r)] = s / a[at(r, r)];
    }
    return x;
}

// The coded form of one filter's coefficients at coeff_q `q`.
void code(std::span<const double> coeffs, int q, FilterParams& f) {
    f.order = static_cast<std::uint8_t>(coeffs.size());
    if (coeffs.empty()) {
        return;
    }
    f.coeff_q = static_cast<std::uint8_t>(q);
    std::array<std::int32_t, kMaxFirOrder> ints{};
    std::uint32_t any = 0;
    int width = 1;
    for (std::size_t k = 0; k < coeffs.size(); ++k) {
        ints[k] = static_cast<std::int32_t>(std::lround(coeffs[k] * std::ldexp(1.0, q)));
        any |= static_cast<std::uint32_t>(ints[k]);
    }
    // coeff_shift: the trailing zeros every coefficient shares, at most 7 (9.2).
    const int shift = any == 0 ? 0 : std::min(7, std::countr_zero(any));
    for (std::size_t k = 0; k < coeffs.size(); ++k) {
        const std::int32_t v = ints[k] >> shift;
        const auto mag = static_cast<std::uint32_t>(v < 0 ? ~v : v);
        width = std::max(width, std::bit_width(mag) + 1);
        f.coeffs[k] = v;
    }
    f.coeff_shift = static_cast<std::uint8_t>(shift);
    f.coeff_bits = static_cast<std::uint8_t>(width);
}

}  // namespace

std::optional<FilterDesign> quantise(std::span<const double> fir, std::span<const double> iir) {
    double peak = 0.0;
    for (const double c : fir) {
        peak = std::max(peak, std::abs(c));
    }
    for (const double c : iir) {
        peak = std::max(peak, std::abs(c));
    }
    // 9.3: coefficients in -32767 to 32767 after coeff_shift, coeff_q 8 to 15.
    int q = 15;
    while (q >= 8 && peak * std::ldexp(1.0, q) > 32767.0) {
        --q;
    }
    if (q < 8) {
        return std::nullopt;
    }
    FilterDesign d;
    code(fir, q, d.fir);
    code(iir, q, d.iir);
    for (const FilterParams* f : {&d.fir, &d.iir}) {
        if (f->order > 0 && f->coeff_bits + f->coeff_shift > 16) {
            return std::nullopt;
        }
    }
    return d;
}

std::vector<std::vector<double>> lpc(std::span<const std::int32_t> signal, int max_order) {
    std::vector<std::vector<double>> out(static_cast<std::size_t>(max_order));
    const std::size_t n = signal.size();
    if (n <= static_cast<std::size_t>(max_order)) {
        return out;
    }
    // A Welch window: the autocorrelation method needs a taper, and this one keeps most of the
    // block's energy.
    std::vector<double> x(n);
    const double half = (static_cast<double>(n) - 1.0) / 2.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = (static_cast<double>(i) - half) / (half + 1.0);
        x[i] = static_cast<double>(signal[i]) * (1.0 - t * t);
    }
    std::vector<double> r(static_cast<std::size_t>(max_order) + 1U);
    for (int lag = 0; lag <= max_order; ++lag) {
        double s = 0.0;
        for (std::size_t i = static_cast<std::size_t>(lag); i < n; ++i) {
            s += x[i] * x[i - static_cast<std::size_t>(lag)];
        }
        r[static_cast<std::size_t>(lag)] = s;
    }
    if (r[0] <= 0.0) {
        return out;
    }
    r[0] *= 1.0 + 1e-9;  // a little white noise: conditions a near-singular recursion
    std::vector<double> a(static_cast<std::size_t>(max_order) + 1U, 0.0);
    std::vector<double> prev;
    double err = r[0];
    for (int m = 1; m <= max_order; ++m) {
        double acc = r[static_cast<std::size_t>(m)];
        for (int k = 1; k < m; ++k) {
            acc -= a[static_cast<std::size_t>(k)] * r[static_cast<std::size_t>(m - k)];
        }
        const double k_m = acc / err;
        if (!std::isfinite(k_m) || std::abs(k_m) >= 1.0) {
            break;
        }
        prev = a;
        a[static_cast<std::size_t>(m)] = k_m;
        for (int k = 1; k < m; ++k) {
            a[static_cast<std::size_t>(k)] =
                prev[static_cast<std::size_t>(k)] - k_m * prev[static_cast<std::size_t>(m - k)];
        }
        err *= 1.0 - k_m * k_m;
        out[static_cast<std::size_t>(m - 1)].assign(a.begin() + 1, a.begin() + 1 + m);
    }
    return out;
}

std::optional<std::pair<std::vector<double>, std::vector<double>>> arma(
    std::span<const std::int32_t> signal, int fir_order, int iir_order) {
    const int long_order = 16;
    const std::size_t n = signal.size();
    if (n < static_cast<std::size_t>(4 * long_order)) {
        return std::nullopt;
    }
    const auto ar = lpc(signal, long_order);
    const std::vector<double>* longest = nullptr;
    for (const auto& c : ar) {
        if (!c.empty()) {
            longest = &c;
        }
    }
    if (longest == nullptr) {
        return std::nullopt;
    }
    // Stage 1: the residuals of the long autoregression stand in for the true innovations.
    std::vector<double> e(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        double p = 0.0;
        for (std::size_t k = 0; k < longest->size() && k < i; ++k) {
            p += (*longest)[k] * signal[i - k - 1];
        }
        e[i] = signal[i] - p;
    }
    // Stage 2: one least-squares fit of x[i] on x[i-1..i-p] and e[i-1..i-q].
    const int m = fir_order + iir_order;
    std::vector<double> ata(static_cast<std::size_t>(m * m), 0.0);
    std::vector<double> atb(static_cast<std::size_t>(m), 0.0);
    std::vector<double> row(static_cast<std::size_t>(m));
    const std::size_t start = static_cast<std::size_t>(long_order);
    for (std::size_t i = start; i < n; ++i) {
        for (int k = 0; k < fir_order; ++k) {
            row[static_cast<std::size_t>(k)] = signal[i - static_cast<std::size_t>(k) - 1];
        }
        for (int k = 0; k < iir_order; ++k) {
            row[static_cast<std::size_t>(fir_order + k)] = e[i - static_cast<std::size_t>(k) - 1];
        }
        for (int r = 0; r < m; ++r) {
            for (int c = 0; c < m; ++c) {
                ata[static_cast<std::size_t>(r * m + c)] +=
                    row[static_cast<std::size_t>(r)] * row[static_cast<std::size_t>(c)];
            }
            atb[static_cast<std::size_t>(r)] += row[static_cast<std::size_t>(r)] * signal[i];
        }
    }
    for (int d = 0; d < m; ++d) {
        ata[static_cast<std::size_t>(d * m + d)] *= 1.0 + 1e-9;
    }
    auto x = solve(std::move(ata), std::move(atb), m);
    if (!x) {
        return std::nullopt;
    }
    std::vector<double> a(x->begin(), x->begin() + fir_order);
    std::vector<double> b(x->begin() + fir_order, x->end());
    return std::make_pair(std::move(a), std::move(b));
}

std::optional<std::vector<double>> least_squares(
    std::span<const std::span<const std::int32_t>> inputs, std::span<const std::int32_t> target) {
    const int m = static_cast<int>(inputs.size());
    if (m == 0) {
        return std::vector<double>{};
    }
    std::vector<double> ata(static_cast<std::size_t>(m * m), 0.0);
    std::vector<double> atb(static_cast<std::size_t>(m), 0.0);
    for (std::size_t i = 0; i < target.size(); ++i) {
        for (int r = 0; r < m; ++r) {
            const double xr = inputs[static_cast<std::size_t>(r)][i];
            for (int c = 0; c < m; ++c) {
                ata[static_cast<std::size_t>(r * m + c)] +=
                    xr * static_cast<double>(inputs[static_cast<std::size_t>(c)][i]);
            }
            atb[static_cast<std::size_t>(r)] += xr * static_cast<double>(target[i]);
        }
    }
    for (int d = 0; d < m; ++d) {
        ata[static_cast<std::size_t>(d * m + d)] += 1e-6;
    }
    return solve(std::move(ata), std::move(atb), m);
}

}  // namespace iclforge::mlp::detail
