// Mixed-radix FFT for smooth sizes (factors 2, 3, 5).
//
// In-tree implementation (ROADMAP deviation from vendored KissFFT): the modem
// needs N = 320 = 2^6 * 5 at ~33 transforms/s plus power-of-two sizes for
// spectral analysis - correctness matters, speed does not. Verified against a
// naive double-precision DFT in test_dsp.
#pragma once

#include <complex>
#include <vector>

namespace skw {

using cfloat = std::complex<float>;

class Fft {
 public:
  // n must factor into {2, 3, 5}; throws std::invalid_argument otherwise.
  explicit Fft(int n);

  int size() const { return n_; }

  // out must not alias in. forward: X[k] = sum x[j] e^{-2pi i jk/n}.
  void forward(const cfloat* in, cfloat* out) const;
  // inverse includes the 1/n scale: x = ifft(fft(x)).
  void inverse(const cfloat* in, cfloat* out) const;

 private:
  void compute(const cfloat* in, cfloat* out, int n, int stride,
               int twiddleStep) const;

  int n_;
  std::vector<cfloat> twiddle_;        // e^{-2pi i k / n_}, k < n_
  mutable std::vector<cfloat> tmpA_;   // conjugate scratch for inverse
};

}  // namespace skw
