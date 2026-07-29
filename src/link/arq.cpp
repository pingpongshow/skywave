#include "link/arq.hpp"

#include <algorithm>

namespace skw {

// -------------------------------------------------------------------- ArqTx

void ArqTx::setTimeout(double sec) {
  timeoutSec_ = std::clamp(sec, 4.0, 45.0);
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
  p.payload = std::move(payload);
  pending_.push_back(std::move(p));
  if (deadline_ == 0.0) deadline_ = now + timeoutSec_;
  return pending_.back().seq;
}

void ArqTx::onAck(const AckBody& ack, double now) {
  // Release everything below the cumulative base.
  while (!pending_.empty() && seqDiff(pending_.front().seq, ack.ackBase) < 0) {
    pending_.pop_front();
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
        pending_.erase(it);
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

  // Rearm/clear the timer.
  deadline_ = pending_.empty() ? 0.0 : now + timeoutSec_;
}

std::optional<ArqTx::Retransmit> ArqTx::nextRetransmit(double now) {
  while (!retxQueue_.empty()) {
    uint16_t seq = retxQueue_.front();
    retxQueue_.pop_front();
    Pending* p = find(seq);
    if (!p) continue;  // released since queued
    p->inRetxQueue = false;
    ++p->retries;
    ++p->rv;
    deadline_ = now + timeoutSec_;
    return Retransmit{p->seq, p->rv, p->payload};
  }
  return std::nullopt;
}

void ArqTx::tick(double now) {
  if (deadline_ == 0.0 || now < deadline_ || pending_.empty()) return;
  // ACK never came: requeue every in-flight frame, oldest first.
  for (auto& p : pending_) queueRetx(p);
  deadline_ = now + timeoutSec_;
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
