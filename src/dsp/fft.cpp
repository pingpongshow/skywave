#include "dsp/fft.hpp"

#include <cmath>
#include <stdexcept>

namespace skw {
namespace {
int smallestFactor(int n) {
  if (n % 2 == 0) return 2;
  if (n % 3 == 0) return 3;
  if (n % 5 == 0) return 5;
  return 0;
}
}  // namespace

Fft::Fft(int n) : n_(n) {
  int m = n;
  while (m > 1) {
    int f = smallestFactor(m);
    if (f == 0) throw std::invalid_argument("Fft: size must factor into 2,3,5");
    m /= f;
  }
  twiddle_.resize(size_t(n));
  for (int k = 0; k < n; ++k) {
    double a = -2.0 * M_PI * double(k) / double(n);
    twiddle_[size_t(k)] = cfloat(float(std::cos(a)), float(std::sin(a)));
  }
  tmpA_.resize(size_t(n));
}

// Recursive decimation-in-time. `in` is read with the given stride;
// `out[0..n)` receives the transform. twiddleStep = n_ / n maps local
// exponents onto the master twiddle table.
void Fft::compute(const cfloat* in, cfloat* out, int n, int stride,
                  int twiddleStep) const {
  if (n == 1) {
    out[0] = in[0];
    return;
  }
  const int r = smallestFactor(n);
  const int m = n / r;

  // Sub-transforms of the r interleaved sequences land contiguously in out.
  for (int q = 0; q < r; ++q) {
    compute(in + q * stride, out + q * m, m, stride * r, twiddleStep * r);
  }

  // Combine: for each k, the r values {out[q*m + k]} map onto the same
  // output slots {out[k + s*m]}, so a local buffer suffices (r <= 5).
  cfloat t[5];
  for (int k = 0; k < m; ++k) {
    for (int q = 0; q < r; ++q) {
      // e^{-2pi i qk/n} = master twiddle at qk * (n_/n)
      t[q] = out[q * m + k] * twiddle_[size_t(q * k * twiddleStep)];
    }
    for (int s = 0; s < r; ++s) {
      // Size-r DFT: X_s = sum_q t_q e^{-2pi i qs/r}; exponent qs*(n_/r) mod n_.
      cfloat acc = t[0];
      const int base = s * m * twiddleStep;  // = s * (n_/r)
      for (int q = 1; q < r; ++q) {
        acc += t[q] * twiddle_[size_t((q * base) % n_)];
      }
      out[k + s * m] = acc;
    }
  }
}

void Fft::forward(const cfloat* in, cfloat* out) const {
  compute(in, out, n_, 1, 1);
}

void Fft::inverse(const cfloat* in, cfloat* out) const {
  // ifft(x) = conj(fft(conj(x))) / n
  for (int i = 0; i < n_; ++i) tmpA_[size_t(i)] = std::conj(in[i]);
  compute(tmpA_.data(), out, n_, 1, 1);
  const float scale = 1.0f / float(n_);
  for (int i = 0; i < n_; ++i) out[i] = std::conj(out[i]) * scale;
}

}  // namespace skw
