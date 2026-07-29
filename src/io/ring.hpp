// Lock-free single-producer/single-consumer sample ring (ARCHITECTURE.md:
// audio callback <-> modem worker crossing; no locks, no allocation after
// construction).
#pragma once

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace skw {

class SampleRing {
 public:
  // Capacity rounded up to a power of two; usable capacity is cap-1.
  explicit SampleRing(size_t minCapacity) {
    size_t cap = 1;
    while (cap < minCapacity + 1) cap <<= 1;
    buf_.resize(cap);
    mask_ = cap - 1;
  }

  size_t readAvailable() const {
    return (head_.load(std::memory_order_acquire) -
            tail_.load(std::memory_order_relaxed)) & sizeMask();
  }
  size_t writeAvailable() const {
    return buf_.size() - 1 - ((head_.load(std::memory_order_relaxed) -
                               tail_.load(std::memory_order_acquire)) &
                              sizeMask());
  }

  // Producer side. Returns samples actually written (drops the rest).
  size_t write(std::span<const float> in) {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);
    size_t free = buf_.size() - 1 - ((head - tail) & sizeMask());
    size_t n = in.size() < free ? in.size() : free;
    for (size_t i = 0; i < n; ++i) buf_[(head + i) & mask_] = in[i];
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  // Consumer side. Returns samples actually read.
  size_t read(std::span<float> out) {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);
    size_t avail = (head - tail) & sizeMask();
    size_t n = out.size() < avail ? out.size() : avail;
    for (size_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & mask_];
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

 private:
  size_t sizeMask() const { return mask_; }
  std::vector<float> buf_;
  size_t mask_ = 0;
  std::atomic<size_t> head_{0};  // producer position
  std::atomic<size_t> tail_{0};  // consumer position
};

}  // namespace skw
