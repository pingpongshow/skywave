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

  // OPEN-LOOP timeout floor = burst_airtime + expected_ack_airtime + 3 s.
  // It budgets AIRTIME only, which is not the round trip: a receiver
  // also has to DECODE, and the decode ladder's coarse window rungs
  // (worker kRxWindowSec) add several seconds on top of the burst. The
  // one-way latency was MEASURED at a median 12.4 s in the ST2 world
  // against a 6.7 s open-loop timeout at M10 - so every data frame
  // retransmitted three times before its ACK could physically arrive,
  // each retransmission lengthened the peer's decode queue, and a clean
  // 30 dB one-way link delivered ZERO bytes. The airtime estimate is
  // therefore a FLOOR, superseded by the measured round trip below.
  void setTimeout(double sec);
  // Effective ACK timeout: max(open-loop floor, smoothed RTT estimate),
  // scaled by the timeout backoff, clamped [4, 90].
  double timeoutSec() const;
  // Seed the RTT estimator from an unambiguous non-data exchange (the
  // connect handshake). Without a seed the estimator cannot bootstrap:
  // the first data frames time out before any ACK returns, every one of
  // them is then a retransmission, and Karn's algorithm - correctly -
  // refuses to sample an ambiguous round trip. The handshake measures
  // the same decode-and-reply latency with no ambiguity at all.
  void seedRtt(double sec);
  double srttSec() const { return srtt_; }
  // When the peer last demonstrably received something from us (any
  // forward progress). -1 = never. Distinguishes "the peer is not
  // hearing us" from "an ACK went missing", which an ACK timeout alone
  // cannot tell apart.
  double lastProgressAt() const { return lastProgressAt_; }
  bool haveRtt() const { return srtt_ > 0.0; }
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

  // Explicit NAK (kRxFlagRxFail): the peer heard a burst it could not
  // decode. Requeue its next-expected frame - exactly the head-of-line
  // frame its window cannot advance without - for immediate
  // retransmission, instead of waiting for the ACK timer (50-90 s under
  // fading). Charged as a retry like any retransmission, so the retry
  // budget still bounds a dead link. Returns true if a frame was queued.
  bool nakAt(uint16_t seq, double now);
  // Oldest unacknowledged sequence (the head-of-line frame), if any:
  // what a turn-taking drain fast-retransmits when the window will not
  // clear on its own.
  std::optional<uint16_t> oldestInFlight() const {
    if (pending_.empty()) return std::nullopt;
    return pending_.front().seq;
  }

  // Cumulative-only feedback from the ENVELOPE's piggybacked ack_base
  // (PROTOCOL.md: "every data-bearing frame also piggybacks ack_base").
  // Releases everything below the reported floor and resets retry budgets
  // on forward progress, exactly like a dedicated ACK - but reports no
  // holes, so it never queues a retransmission and never re-arms the
  // timer on its own. Returns true when it released something.
  bool onPiggybackAck(uint16_t ackBase, double now);

  // Retransmissions drain BEFORE new data (TX-queue priority).
  struct Retransmit {
    uint16_t seq;
    uint8_t rv;
    std::vector<uint8_t> payload;
  };
  std::optional<Retransmit> nextRetransmit(double now);
  // Sequence at the head of the retransmit queue, WITHOUT consuming it
  // or charging a retry. The caller may already hold an unsent copy of
  // that frame; taking the retransmit anyway burned a retry (and
  // re-armed the timer) for a frame that never aired - with a queued
  // copy stuck behind the CCA gate this repeated until max-retries
  // tore down a live link.
  std::optional<uint16_t> peekRetransmit() const {
    for (uint16_t s : retxQueue_) {
      for (const auto& p : pending_) {
        if (p.seq == s) return s;
      }
    }
    return std::nullopt;
  }

  // Fires the ACK timeout: all in-flight unacked frames requeue.
  void tick(double now);
  // Turn-taking (PROTOCOL.md "Turn-Taking"): a station that has handed
  // the sending role away CANNOT retransmit, so firing its ACK timers
  // only burns retry budget on frames it is forbidden to resend.
  // Suspension freezes the timeout clock (in-flight frames are covered
  // by the new sender's piggybacked ack_base meanwhile); resuming
  // re-arms a fresh deadline for whatever is still outstanding.
  void setSuspended(bool s, double now);
  bool suspended() const { return suspended_; }
  // Frames requeued by ACK timeout since the last call (cleared). A
  // burst that is never acknowledged is the ONLY failure evidence a
  // sender gets when the peer heard nothing at all - on a half-duplex
  // link (deaf while keyed) or below the mode's floor, whole bursts
  // vanish and the receiver-side statistics stay empty.
  int takeTimeouts() {
    const int n = timeouts_;
    timeouts_ = 0;
    return n;
  }

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
    // -1 = unset. A zero sentinel would be WRONG: `now` is caller
    // supplied and legitimately starts at 0.0, which silently
    // disabled every RTT sample taken at the start of a session.
    double sentAt = -1.0;
    std::vector<uint8_t> payload;
  };
  Pending* find(uint16_t seq);
  void queueRetx(Pending& p, double now);
  // timeoutSec() WITHOUT the consecutive-timeout backoff: the unit the
  // retry budget's wall-clock equivalent is measured in (the backoff
  // grows during an RTO-paced retry run, so measuring against the
  // backed-off value would never let a dead peer time out).
  double baseTimeoutSec() const;
  // Release the front pending frame, sampling its RTT when unambiguous.
  void releaseFront(double now);
  void sampleRtt(double rttSec);

  int window_;
  int maxRetries_;
  double openLoopSec_ = 10.0;
  // Jacobson/Karels smoothed round trip. 0 = no measurement yet.
  double srtt_ = 0.0;
  double rttvar_ = 0.0;
  // Multiplicative backoff on consecutive timeouts, reset by a clean
  // RTT sample. Without it a persistently under-estimated timeout
  // retransmits forever at the same wrong cadence.
  double backoff_ = 1.0;
  double lastProgressAt_ = -1.0;
  double firstSendAt_ = -1.0;  // budget clock before any progress exists
  double extraSec_ = 0.0;  // TX-queue delay ahead of newly armed timers
  int timeouts_ = 0;       // ACK-timeout requeues (takeTimeouts)
  double deadline_ = 0.0;  // 0 = no timer armed
  bool suspended_ = false;  // turn-taking: sending role handed away
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
