#include "link/arq.hpp"

#include <algorithm>

namespace skw {

// -------------------------------------------------------------------- ArqTx

void ArqTx::setTimeout(double sec) {
  // Ceiling 90 s: a runaway guard, NOT a tuning. The old 45 s ceiling
  // sat BELOW the physical data+ACK round trip of deep CSS modes, so
  // every first transmission timed out before its ACK could possibly
  // return and each frame cost a full retransmission cycle (measured
  // 0.83 B/s live at M1 against a 32 B/s theoretical net).
  openLoopSec_ = std::clamp(sec, 4.0, 90.0);
}

double ArqTx::timeoutSec() const {
  double t = openLoopSec_;
  if (srtt_ > 0.0) {
    // The full Jacobson/Karels variance term, deliberately UNCAPPED.
    // Capping it at one srtt was tried twice - a 68.8 s timeout against
    // a 20 s round trip looks obviously excessive - and MEASURED WORSE
    // both times. On properly isolated soaks (no inherited channel
    // memory) the cap froze churn at 17.9/21.7 KB where the uncapped
    // build reached 44.0/45.0 KB.
    //
    // The long tail is not slack. This link's latency is genuinely
    // heavy-tailed: the receiver's decode ladder resolves a burst at a
    // coarse window rung (2.5/5/10/20 s) and which rung it lands on
    // varies frame to frame. A timeout sized to the MEAN retransmits
    // into the tail, which is the exact failure this estimator exists
    // to prevent.
    t = std::max(t, srtt_ + 4.0 * rttvar_);
  }
  return std::clamp(t * backoff_, 4.0, 90.0);
}

void ArqTx::sampleRtt(double rttSec) {
  // Implausible samples (clock jumps, a resumed session's stale frame)
  // must not poison the estimator.
  if (rttSec <= 0.0 || rttSec > 300.0) return;
  if (srtt_ <= 0.0) {
    srtt_ = rttSec;
    rttvar_ = rttSec * 0.5;
  } else {
    rttvar_ = 0.75 * rttvar_ + 0.25 * std::abs(srtt_ - rttSec);
    srtt_ = 0.875 * srtt_ + 0.125 * rttSec;
  }
  // A frame acknowledged on its FIRST transmission proves the current
  // timeout is workable, so the backoff earned by past timeouts goes.
  backoff_ = 1.0;
}

void ArqTx::seedRtt(double sec) {
  if (sec <= 0.0 || sec > 300.0 || srtt_ > 0.0) return;
  srtt_ = sec;
  rttvar_ = sec * 0.5;  // => initial RTO of 3x the handshake round trip
}

void ArqTx::releaseFront(double now) {
  const Pending& p = pending_.front();
  // KARN'S ALGORITHM: a retransmitted frame's ACK cannot be attributed
  // to a particular transmission, so it never yields an RTT sample.
  // The backoff in tick() is what covers an under-estimate meanwhile.
  if (p.retries == 0 && p.sentAt >= 0.0) sampleRtt(now - p.sentAt);
  pending_.pop_front();
}

ArqTx::Pending* ArqTx::find(uint16_t seq) {
  for (auto& p : pending_) {
    if (p.seq == seq) return &p;
  }
  return nullptr;
}

void ArqTx::queueRetx(Pending& p) {
  if (p.inRetxQueue) return;
  if (p.retries >= maxRetries_) {
    failed_ = true;
    return;
  }
  p.inRetxQueue = true;
  retxQueue_.push_back(p.seq);
}

std::optional<uint16_t> ArqTx::send(std::vector<uint8_t> payload,
                                    double now) {
  if (!canSend() || failed_) return std::nullopt;
  Pending p;
  p.seq = nextSeq_++;
  p.sentAt = now;
  p.payload = std::move(payload);
  pending_.push_back(std::move(p));
  if (deadline_ == 0.0) deadline_ = now + timeoutSec() + extraSec_;
  return pending_.back().seq;
}

void ArqTx::onAck(const AckBody& ack, double now) {
  // Release everything below the cumulative base.
  bool progress = false;
  while (!pending_.empty() && seqDiff(pending_.front().seq, ack.ackBase) < 0) {
    releaseFront(now);
    progress = true;
  }
  // Drop released seqs from the retransmit queue.
  retxQueue_.erase(
      std::remove_if(retxQueue_.begin(), retxQueue_.end(),
                     [&](uint16_t s) { return find(s) == nullptr; }),
      retxQueue_.end());

  // Bitmap: bit k set => seq base+1+k received; those frames release too.
  for (int k = 0; k < 16; ++k) {
    if (!(ack.gapBitmap & (1u << k))) continue;
    uint16_t seq = uint16_t(ack.ackBase + 1 + k);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->seq == seq) {
        if (it->retries == 0 && it->sentAt >= 0.0) {
          sampleRtt(now - it->sentAt);
        }
        pending_.erase(it);
        progress = true;
        break;
      }
    }
    retxQueue_.erase(std::remove(retxQueue_.begin(), retxQueue_.end(), seq),
                     retxQueue_.end());
  }

  // A NAK (any gap bit) reports a hole at the base and possibly between
  // bitmap bits: every still-pending frame at or beyond the base that the
  // bitmap did NOT cover is presumed lost.
  if (ack.gapBitmap != 0) {
    for (auto& p : pending_) {
      const int16_t d = seqDiff(p.seq, ack.ackBase);
      if (d < 0) continue;                 // released above
      if (d >= 1 && d <= 16 && (ack.gapBitmap & (1u << (d - 1)))) continue;
      // Only frames the receiver has had a chance to see: anything up to
      // the highest bitmap-reported sequence.
      int highest = 0;
      for (int k = 15; k >= 0; --k) {
        if (ack.gapBitmap & (1u << k)) {
          highest = k + 1;
          break;
        }
      }
      if (d < highest) queueRetx(p);
    }
  }

  // Forward progress proves the link is alive and delivering: the retry
  // budget exists to detect a DEAD peer, not to cap how long a busy
  // bidirectional session may take to schedule everything (observed
  // failure: sustained two-way bulk starved ACK turnaround, timeouts
  // requeued aggressively, and a healthy session hit max-retries).
  if (progress) {
    for (auto& p : pending_) p.retries = 0;
    lastProgressAt_ = now;
    // Backoff DECAY on forward progress (halve, don't reset): under
    // sustained retransmission Karn never takes a clean sample, so the
    // 8x backoff pinned itself at max and every cycle cost the full
    // 90 s clamp even while frames were demonstrably getting through
    // (measured in the floor soak as one burst per 90 s at 7 dB of mode
    // margin). Progress proves the link delivers at something near the
    // estimate; halving re-probes without trusting a single event.
    // NOTE: first tried during the confounded-chanmem campaign and
    // reverted on evidence later retracted (ROADMAP "CORRECTION");
    // re-landed on the isolated harness.
    backoff_ = std::max(1.0, backoff_ * 0.5);
  }

  // Rearm/clear the timer. Only forward progress may push the deadline
  // out: a peer repeatedly reporting the SAME floor (head-of-line frame
  // lost, nothing after it received) would otherwise defer our timeout
  // forever and the hole would never be retransmitted.
  if (pending_.empty()) {
    deadline_ = 0.0;
  } else if (progress || deadline_ == 0.0) {
    deadline_ = now + timeoutSec() + extraSec_;
  }
}

bool ArqTx::onPiggybackAck(uint16_t ackBase, double now) {
  // Sanity bound: a peer cannot have received something we never sent.
  // The envelope field is unauthenticated on cleartext links and a
  // wrapped serial number makes "everything below" a dangerous phrase,
  // so an implausible floor is ignored rather than acted on.
  if (seqDiff(ackBase, nextSeq_) > 0) return false;
  bool progress = false;
  while (!pending_.empty() && seqDiff(pending_.front().seq, ackBase) < 0) {
    releaseFront(now);
    progress = true;
  }
  if (!progress) return false;

  // Released seqs leave the retransmit queue with them.
  retxQueue_.erase(
      std::remove_if(retxQueue_.begin(), retxQueue_.end(),
                     [&](uint16_t s) { return find(s) == nullptr; }),
      retxQueue_.end());
  // Forward progress proves the peer is alive and delivering (same rule
  // as onAck: the budget detects a dead peer, not a busy schedule).
  for (auto& p : pending_) p.retries = 0;
  lastProgressAt_ = now;
  backoff_ = std::max(1.0, backoff_ * 0.5);  // see onAck
  deadline_ = pending_.empty() ? 0.0 : now + timeoutSec() + extraSec_;
  return true;
}

std::optional<ArqTx::Retransmit> ArqTx::nextRetransmit(double now) {
  if (suspended_) return std::nullopt;
  while (!retxQueue_.empty()) {
    uint16_t seq = retxQueue_.front();
    retxQueue_.pop_front();
    Pending* p = find(seq);
    if (!p) continue;  // released since queued
    p->inRetxQueue = false;
    ++p->retries;
    ++p->rv;
    deadline_ = now + timeoutSec() + extraSec_;
    return Retransmit{p->seq, p->rv, p->payload};
  }
  return std::nullopt;
}

void ArqTx::setSuspended(bool s, double now) {
  if (s == suspended_) return;
  suspended_ = s;
  if (!s && !pending_.empty()) {
    // Regaining the sending role with frames still in flight: they were
    // sent BEFORE the handover and have been outstanding for the peer's
    // whole turn without an acknowledgement (the peer's piggybacked
    // ack_base would have released anything it received). Re-arming a
    // fresh timeout here made them wait another full ARQ period - up to
    // 90 s - before the first retransmission (measured: one frame per
    // minute-plus, a 10 minute drain for an 8-frame window). Treat them
    // as timed out NOW; the retry budget still bounds a dead peer.
    for (auto& p : pending_) queueRetx(p);
    deadline_ = now + timeoutSec() + extraSec_;
  }
}

void ArqTx::tick(double now) {
  if (suspended_) return;
  if (deadline_ == 0.0 || now < deadline_ || pending_.empty()) return;
  // ACK never came: requeue every in-flight frame, oldest first.
  ++timeouts_;
  for (auto& p : pending_) queueRetx(p);
  // Exponential backoff, capped. The open-loop estimate can only be an
  // estimate; when it is short of the real round trip this is what
  // stops the sender retransmitting into the peer's decode window.
  backoff_ = std::min(backoff_ * 2.0, 8.0);
  deadline_ = now + timeoutSec() + extraSec_;
}

void ArqTx::resumeFrom(uint16_t peerNextSeq, double now) {
  while (!pending_.empty() &&
         seqDiff(pending_.front().seq, peerNextSeq) < 0) {
    pending_.pop_front();
  }
  retxQueue_.clear();
  failed_ = false;
  for (auto& p : pending_) {
    p.retries = 0;
    p.inRetxQueue = false;
    queueRetx(p);
  }
  deadline_ = pending_.empty() ? 0.0 : now + timeoutSec() + extraSec_;
}

// -------------------------------------------------------------------- ArqRx

ArqRx::Accept ArqRx::onData(uint16_t seq, std::vector<uint8_t> payload) {
  const int16_t d = seqDiff(seq, base_);
  if (d < 0) return Accept::Duplicate;  // already delivered
  if (d > 16 || d >= 2 * window_ + 16) return Accept::OutsideWindow;
  if (d == 0) {
    delivered_.push_back(std::move(payload));
    ++base_;
    // Drain any stashed continuation.
    bool advanced = true;
    while (advanced) {
      advanced = false;
      for (auto it = stash_.begin(); it != stash_.end(); ++it) {
        if (it->seq == base_) {
          delivered_.push_back(std::move(it->payload));
          stash_.erase(it);
          ++base_;
          advanced = true;
          break;
        }
      }
    }
    return Accept::InOrder;
  }
  // Out of order within the bitmap span.
  for (const auto& s : stash_) {
    if (s.seq == seq) return Accept::Duplicate;
  }
  stash_.push_back({seq, std::move(payload)});
  return Accept::OutOfOrder;
}

std::vector<std::vector<uint8_t>> ArqRx::takeDelivered() {
  auto out = std::move(delivered_);
  delivered_.clear();
  return out;
}

uint16_t ArqRx::gapBitmap() const {
  uint16_t bm = 0;
  for (const auto& s : stash_) {
    const int16_t d = seqDiff(s.seq, base_);
    if (d >= 1 && d <= 16) bm |= uint16_t(1u << (d - 1));
  }
  return bm;
}

AckBody ArqRx::makeAck(int8_t snrHalfDb, uint8_t rxFlags,
                       int8_t cfoHalfHz) const {
  AckBody a;
  a.snrHalfDb = snrHalfDb;
  a.rxFlags = rxFlags;
  a.ackBase = base_;
  a.gapBitmap = gapBitmap();
  a.cfoHalfHz = cfoHalfHz;
  return a;
}

}  // namespace skw
