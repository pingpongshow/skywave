// Selective-repeat ARQ engine (PROTOCOL.md "Connected Transport"):
// per-direction 16-bit sequence space, window from the MAC profile,
// cumulative base + 16-bit gap bitmap feedback, adaptive airtime-derived
// timeout, retransmit-first priority, max-retries teardown signal.
// Pure logic; time injected in seconds.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "link/frame.hpp"

namespace skw {

// Serial-number arithmetic on the 16-bit space.
inline int16_t seqDiff(uint16_t a, uint16_t b) {
  return int16_t(uint16_t(a - b));
}

// ACK convention: ackBase = next-expected sequence (everything below it
// received); gapBitmap bit k = 1 means seq (ackBase + 1 + k) HAS been
// received out of order. Any bit set implies a hole at ackBase -> NAK.

class ArqTx {
 public:
  ArqTx(int window, int maxRetries) : window_(window), maxRetries_(maxRetries) {}
  // MAC profile changes mid-session resize the window (mode ladder
  // moves); shrinking only limits NEW sends, in-flight frames drain.
  void setWindow(int w) { window_ = w; }

  // ACK timeout = burst_airtime + expected_ack_airtime + 3 s, [4, 45].
  void setTimeout(double sec);
  double timeoutSec() const { return timeoutSec_; }
  // Un-aired audio queued AHEAD of any frame sent now (worker TX ring +
  // staging). A frame cannot possibly be ACKed before its own queue
  // delay has passed - arming deadlines without it made every timer
  // fire early under sustained TX and snowballed retransmissions.
  void setExtraDelay(double sec) { extraSec_ = sec < 0.0 ? 0.0 : sec; }

  bool canSend() const { return int(pending_.size()) < window_; }
  size_t inFlight() const { return pending_.size(); }
  bool failed() const { return failed_; }  // max retries exceeded -> teardown

  // Assign the next sequence number and retain the payload for
  // retransmission. Returns nullopt when the window is full.
  std::optional<uint16_t> send(std::vector<uint8_t> payload, double now);

  // Process peer feedback; releases acked frames and queues retransmits
  // for the reported holes (original sequence number, rv + 1).
  void onAck(const AckBody& ack, double now);

  // Retransmissions drain BEFORE new data (arbiter priority).
  struct Retransmit {
    uint16_t seq;
    uint8_t rv;
    std::vector<uint8_t> payload;
  };
  std::optional<Retransmit> nextRetransmit(double now);

  // Fires the ACK timeout: all in-flight unacked frames requeue.
  void tick(double now);

  // Session resume (PROTOCOL.md): drop frames the peer confirmed
  // (< peerNextSeq), requeue everything still pending with fresh retry
  // budgets, keep the sequence counter - the sequence space continues.
  void resumeFrom(uint16_t peerNextSeq, double now);
  uint16_t nextSeq() const { return nextSeq_; }

 private:
  struct Pending {
    uint16_t seq;
    uint8_t rv = 0;
    int retries = 0;
    bool inRetxQueue = false;
    std::vector<uint8_t> payload;
  };
  Pending* find(uint16_t seq);
  void queueRetx(Pending& p);

  int window_;
  int maxRetries_;
  double timeoutSec_ = 10.0;
  double extraSec_ = 0.0;  // TX-queue delay ahead of newly armed timers
  double deadline_ = 0.0;  // 0 = no timer armed
  uint16_t nextSeq_ = 0;
  bool failed_ = false;
  std::deque<Pending> pending_;      // oldest first
  std::deque<uint16_t> retxQueue_;   // seqs awaiting retransmit
};

class ArqRx {
 public:
  explicit ArqRx(int window) : window_(window) {}

  enum class Accept { InOrder, OutOfOrder, Duplicate, OutsideWindow };
  Accept onData(uint16_t seq, std::vector<uint8_t> payload);

  // In-order payload stream (drained by the proto layer).
  std::vector<std::vector<uint8_t>> takeDelivered();

  uint16_t ackBase() const { return base_; }
  uint16_t gapBitmap() const;
  AckBody makeAck(int8_t snrHalfDb, uint8_t rxFlags, int8_t cfoHalfHz) const;

 private:
  int window_;
  uint16_t base_ = 0;  // next expected
  // Out-of-order stash: received frames in (base, base+16].
  struct Stashed {
    uint16_t seq;
    std::vector<uint8_t> payload;
  };
  std::vector<Stashed> stash_;
  std::vector<std::vector<uint8_t>> delivered_;
};

}  // namespace skw
