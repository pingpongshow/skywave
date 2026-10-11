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
    if (n < in.size()) {
      dropped_.fetch_add(in.size() - n, std::memory_order_relaxed);
    }
    return n;
  }

  // Samples the producer could not write because the ring was full - the
  // consumer fell behind real time. Never silent: an RX overrun destroys
  // whatever frame was on air.
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  // Consumer-side LOOKAHEAD: copy up to out.size() samples starting
  // `offset` past the consumer position WITHOUT consuming them. Used by
  // the ST2 test world to channel-process a burst the round it is staged
  // while the ring keeps draining at real time (the modem's keying and
  // txq clock). Returns samples copied.
  size_t peek(std::span<float> out, size_t offset = 0) const {
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t head = head_.load(std::memory_order_acquire);
    size_t avail = (head - tail) & sizeMask();
    if (offset >= avail) return 0;
    size_t n = avail - offset;
    if (out.size() < n) n = out.size();
    for (size_t i = 0; i < n; ++i) out[i] = buf_[(tail + offset + i) & mask_];
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
  std::atomic<uint64_t> dropped_{0};  // producer-side overrun count
};

}  // namespace skw
