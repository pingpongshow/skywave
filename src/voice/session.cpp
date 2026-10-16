#ifdef SKW_HAVE_VOICE

#include "voice/session.hpp"

#include <algorithm>

#include "core/bytepack.hpp"

namespace skw {

std::optional<VoiceAction> VoiceFsm::nextAction() {
  if (queue_.empty()) return std::nullopt;
  auto a = queue_.front();
  queue_.pop_front();
  return a;
}

void VoiceFsm::cmdOpen(VoiceCodec codec, double now) {
  if (state_ != VoiceState::Closed) return;
  codec_ = codec;
  state_ = VoiceState::Opening;
  attempts_ = cfg_.openAttempts - 1;
  deadline_ = now + cfg_.openTimeoutSec;
  emit(VoiceAction::SendOpen);
}

void VoiceFsm::cmdClose(double now) {
  if (state_ == VoiceState::Closed) return;
  // A local close while talking still ends the talkspurt cleanly first.
  if (state_ == VoiceState::Talking) emit(VoiceAction::SendPttEnd);
  state_ = VoiceState::Closed;
  pttWanted_ = false;
  deadline_ = 0.0;
  (void)now;
  emit(VoiceAction::SendClose);
  emit(VoiceAction::IndicateClosed);
}

void VoiceFsm::cmdPttOn(double now) {
  pttWanted_ = true;
  if (state_ != VoiceState::Open) return;  // busy or closed: ignored
  state_ = VoiceState::Requesting;
  deadline_ = now + cfg_.openTimeoutSec;
  attempts_ = cfg_.openAttempts - 1;  // REQ is retried like OPEN
  emit(VoiceAction::SendPttReq);
}

void VoiceFsm::cmdPttOff(double now) {
  pttWanted_ = false;
  if (state_ == VoiceState::Talking) {
    state_ = VoiceState::Open;
    deadline_ = 0.0;
    emit(VoiceAction::SendPttEnd);
    emit(VoiceAction::IndicateStopped);
  } else if (state_ == VoiceState::Requesting) {
    state_ = VoiceState::Open;  // withdrawn before the grant arrived
    deadline_ = 0.0;
  }
  (void)now;
}

void VoiceFsm::rxOpen(VoiceCodec codec, double now) {
  // The responder accepts the proposal (the engine has already clamped
  // the codec to what the transport carries).
  codec_ = codec;
  if (state_ == VoiceState::Closed || state_ == VoiceState::Opening) {
    state_ = VoiceState::Open;
    deadline_ = 0.0;
    emit(VoiceAction::SendOpenAck);
    emit(VoiceAction::IndicateOpen);
  } else {
    emit(VoiceAction::SendOpenAck);  // idempotent re-open
  }
  (void)now;
}

void VoiceFsm::rxOpenAck(VoiceCodec codec, double now) {
  if (state_ != VoiceState::Opening) return;
  codec_ = codec;  // peer may only lower the proposal
  state_ = VoiceState::Open;
  deadline_ = 0.0;
  emit(VoiceAction::IndicateOpen);
  if (pttWanted_) cmdPttOn(now);  // operator was already holding PTT
}

void VoiceFsm::rxClose(double now) {
  if (state_ == VoiceState::Closed) return;
  state_ = VoiceState::Closed;
  pttWanted_ = false;
  deadline_ = 0.0;
  (void)now;
  emit(VoiceAction::IndicateClosed);
}

void VoiceFsm::rxPttReq(double now, bool weWinTie) {
  switch (state_) {
    case VoiceState::Open:
      state_ = VoiceState::Receiving;
      holdUntil_ = now + cfg_.grantHoldSec;
      deadline_ = holdUntil_;
      emit(VoiceAction::SendPttGrant);
      break;
    case VoiceState::Receiving:
      // The peer is asking again, so it never got our grant (lost on the
      // air) or our view is stale (its PTT_END was lost and this is a new
      // spurt). Either way the peer cannot be talking: grant again and
      // restart the spurt rather than leave both sides waiting out the
      // hold cap.
      holdUntil_ = now + cfg_.grantHoldSec;
      deadline_ = holdUntil_;
      emit(VoiceAction::SendPttGrant);
      break;
    case VoiceState::Requesting:
      // Simultaneous requests: deterministic tie-break so the two
      // stations never both believe they hold the channel.
      if (weWinTie) {
        // Ignore the peer's request; ours stands.
      } else {
        state_ = VoiceState::Receiving;
        holdUntil_ = now + cfg_.grantHoldSec;
        deadline_ = holdUntil_;
        emit(VoiceAction::SendPttGrant);
      }
      break;
    case VoiceState::Talking:
      break;  // we hold the channel; peer must wait for PTT_END
    default:
      break;
  }
}

void VoiceFsm::rxPttGrant(double now) {
  if (state_ != VoiceState::Requesting) return;
  state_ = VoiceState::Talking;
  deadline_ = now + cfg_.grantHoldSec;
  emit(VoiceAction::IndicateTalking);
}

void VoiceFsm::rxPttEnd(double now) {
  if (state_ != VoiceState::Receiving) return;
  state_ = VoiceState::Open;
  deadline_ = 0.0;
  emit(VoiceAction::IndicateStopped);
  if (pttWanted_) cmdPttOn(now);  // our queued request goes out now
}

void VoiceFsm::rxVoiceActivity(double now) {
  if (state_ != VoiceState::Receiving) return;
  // Speech is flowing: the spurt ends at PTT_END, or - if that is lost -
  // rxIdleSec after the last unit, never later than the hold cap.
  deadline_ = std::min(now + cfg_.rxIdleSec, holdUntil_);
}

void VoiceFsm::tick(double now) {
  if (deadline_ == 0.0 || now < deadline_) return;
  switch (state_) {
    case VoiceState::Opening:
      if (attempts_-- > 0) {
        deadline_ = now + cfg_.openTimeoutSec;
        emit(VoiceAction::SendOpen);
      } else {
        state_ = VoiceState::Closed;
        deadline_ = 0.0;
        emit(VoiceAction::IndicateClosed);
      }
      break;
    case VoiceState::Requesting:
      // No grant yet: the REQ or the GRANT may have been lost, so ask
      // again (the peer re-grants a repeated REQ). After the last attempt
      // give up this request but keep the voice session open.
      if (attempts_-- > 0) {
        deadline_ = now + cfg_.openTimeoutSec;
        emit(VoiceAction::SendPttReq);
      } else {
        state_ = VoiceState::Open;
        deadline_ = 0.0;
      }
      break;
    case VoiceState::Talking:
    case VoiceState::Receiving: {
      // Talkspurt hold cap: a peer that vanished mid-spurt must not lock
      // the channel forever.
      const bool wasTalking = state_ == VoiceState::Talking;
      state_ = VoiceState::Open;
      deadline_ = 0.0;
      if (wasTalking) emit(VoiceAction::SendPttEnd);
      emit(VoiceAction::IndicateStopped);
      break;
    }
    default:
      deadline_ = 0.0;
      break;
  }
}

// --- jitter buffer -------------------------------------------------------------

void JitterBuffer::push(std::span<const int16_t> pcm) {
  buf_.insert(buf_.end(), pcm.begin(), pcm.end());
  constexpr size_t kMaxSamples = size_t(kMaxMs) * 8000 / 1000;
  if (buf_.size() > kMaxSamples) {
    buf_.erase(buf_.begin(), buf_.begin() + long(buf_.size() - kMaxSamples));
  }
  if (!primed_ && depthMs() >= kTargetMs) primed_ = true;
}

std::vector<int16_t> JitterBuffer::take(size_t samples) {
  std::vector<int16_t> out;
  if (!primed_) return out;  // still filling toward the target depth
  const size_t n = std::min(samples, buf_.size());
  out.assign(buf_.begin(), buf_.begin() + long(n));
  buf_.erase(buf_.begin(), buf_.begin() + long(n));
  if (n < samples) {
    ++underruns_;
    // Drained below the adaptive floor: re-prime before playing again so
    // the stream does not stutter sample-by-sample.
    if (buf_.empty()) primed_ = false;
  }
  return out;
}

void JitterBuffer::reset() {
  buf_.clear();
  primed_ = false;
  underruns_ = 0;
}

// --- voice port records --------------------------------------------------------

std::vector<uint8_t> VoiceRecord::pack() const {
  std::vector<uint8_t> out;
  ByteWriter w(out);
  w.u8(kVoiceRecVersion);
  w.u8(type);
  w.u16be(uint16_t(pcm.size() * 2));
  w.u32be(sequence);
  w.u64be(sampleCounter);
  for (int16_t s : pcm) {
    // PCM16LE payload (INTERFACES.md): header fields are BE, media is LE.
    w.u8(uint8_t(uint16_t(s) & 0xFF));
    w.u8(uint8_t(uint16_t(s) >> 8));
  }
  return out;
}

void VoiceRecordParser::feed(std::span<const uint8_t> bytes) {
  acc_.insert(acc_.end(), bytes.begin(), bytes.end());
  constexpr size_t kHeader = 16;
  size_t off = 0;
  while (acc_.size() - off >= kHeader) {
    ByteReader r({acc_.data() + off, acc_.size() - off});
    const uint8_t ver = r.u8();
    const uint8_t type = r.u8();
    const uint16_t len = r.u16be();
    const uint32_t seq = r.u32be();
    const uint64_t ctr = r.u64be();
    if (ver != kVoiceRecVersion ||
        (type != kVoiceRecUplink && type != kVoiceRecDownlink) ||
        (len % 2) != 0) {
      // Unrecoverable framing: drop everything buffered and resync on the
      // next feed (a PCM stream has no unique delimiter to hunt for).
      ++bad_;
      acc_.clear();
      return;
    }
    if (acc_.size() - off < kHeader + len) break;  // wait for the payload
    VoiceRecord rec;
    rec.type = type;
    rec.sequence = seq;
    rec.sampleCounter = ctr;
    rec.pcm.resize(len / 2);
    for (size_t i = 0; i < rec.pcm.size(); ++i) {
      const size_t p = off + kHeader + i * 2;
      rec.pcm[i] = int16_t(uint16_t(acc_[p]) | (uint16_t(acc_[p + 1]) << 8));
    }
    out_.push_back(std::move(rec));
    off += kHeader + len;
  }
  if (off > 0) acc_.erase(acc_.begin(), acc_.begin() + long(off));
}

std::vector<VoiceRecord> VoiceRecordParser::take() {
  auto out = std::move(out_);
  out_.clear();
  return out;
}

}  // namespace skw

#endif  // SKW_HAVE_VOICE
