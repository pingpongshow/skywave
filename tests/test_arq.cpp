// ARQ engine (ROADMAP 2.4): loss/reorder/duplicate matrices, window
// stalls, adaptive timeout retransmission, max-retries teardown,
// sequence wrap-around.
#include <vector>

#include "link/arq.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> pay(uint8_t tag) { return {tag, uint8_t(tag + 1)}; }

// Deliver TX->RX with a loss mask; returns the RX-generated ACK.
AckBody exchange(ArqTx& tx, ArqRx& rx, const std::vector<bool>& deliver,
                 double now) {
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> sent;
  size_t i = 0;
  while (tx.canSend() && i < deliver.size()) {
    auto seq = tx.send(pay(uint8_t(i)), now);
    if (!seq) break;
    sent.push_back({*seq, pay(uint8_t(i))});
    ++i;
  }
  for (size_t k = 0; k < sent.size(); ++k) {
    if (deliver[k]) rx.onData(sent[k].first, sent[k].second);
  }
  return rx.makeAck(20, 0, 0);
}

}  // namespace

TEST_CASE(in_order_flow) {
  ArqTx tx(4, 8);
  ArqRx rx(4);
  double t = 0;
  for (int round = 0; round < 5; ++round) {
    auto ack = exchange(tx, rx, {true, true, true, true}, t);
    CHECK(!ack.isNak());
    tx.onAck(ack, t);
    CHECK_EQ(int(tx.inFlight()), 0);
    t += 1;
  }
  auto got = rx.takeDelivered();
  CHECK_EQ(int(got.size()), 20);
  CHECK_EQ(int(rx.ackBase()), 20);
}

TEST_CASE(single_loss_nak_retransmit_recover) {
  ArqTx tx(4, 8);
  ArqRx rx(4);
  double t = 0;
  // Send 4, lose the second.
  auto ack = exchange(tx, rx, {true, false, true, true}, t);
  CHECK(ack.isNak());
  CHECK_EQ(int(ack.ackBase), 1);          // hole at seq 1
  CHECK_EQ(int(ack.gapBitmap), 0b011);    // seqs 2,3 arrived
  tx.onAck(ack, t);
  // Only the lost frame retransmits, rv incremented.
  auto r = tx.nextRetransmit(t);
  CHECK(r.has_value());
  CHECK_EQ(int(r->seq), 1);
  CHECK_EQ(int(r->rv), 1);
  CHECK(!tx.nextRetransmit(t).has_value());
  // Deliver it; RX drains the stash in order.
  rx.onData(r->seq, r->payload);
  auto got = rx.takeDelivered();
  CHECK_EQ(int(got.size()), 4);
  CHECK_EQ(int(rx.ackBase()), 4);
  auto ack2 = rx.makeAck(20, 0, 0);
  CHECK(!ack2.isNak());
  tx.onAck(ack2, t);
  CHECK_EQ(int(tx.inFlight()), 0);
  CHECK(!tx.failed());
}

TEST_CASE(reorder_and_duplicates) {
  ArqRx rx(8);
  // Arrive 2, 0, 1 (+ duplicate of 2).
  CHECK(rx.onData(2, pay(2)) == ArqRx::Accept::OutOfOrder);
  CHECK(rx.onData(2, pay(2)) == ArqRx::Accept::Duplicate);
  CHECK(rx.onData(0, pay(0)) == ArqRx::Accept::InOrder);
  CHECK_EQ(int(rx.ackBase()), 1);
  CHECK(rx.onData(1, pay(1)) == ArqRx::Accept::InOrder);
  CHECK_EQ(int(rx.ackBase()), 3);  // stash drained through 2
  auto got = rx.takeDelivered();
  CHECK_EQ(int(got.size()), 3);
  CHECK(got[0] == pay(0) && got[1] == pay(1) && got[2] == pay(2));
  // Old sequence after delivery: duplicate.
  CHECK(rx.onData(0, pay(0)) == ArqRx::Accept::Duplicate);
  // Far-future sequence: outside window.
  CHECK(rx.onData(200, pay(9)) == ArqRx::Accept::OutsideWindow);
}

TEST_CASE(window_stall_and_release) {
  ArqTx tx(2, 8);
  ArqRx rx(2);
  double t = 0;
  CHECK(tx.send(pay(0), t).has_value());
  CHECK(tx.send(pay(1), t).has_value());
  CHECK(!tx.canSend());
  CHECK(!tx.send(pay(2), t).has_value());  // stalled
  rx.onData(0, pay(0));
  tx.onAck(rx.makeAck(0, 0, 0), t);  // base 1: releases one
  CHECK(tx.canSend());
  CHECK_EQ(int(tx.inFlight()), 1);
}

TEST_CASE(timeout_retransmits_all_inflight) {
  ArqTx tx(4, 8);
  tx.setTimeout(6.0);
  double t = 0;
  tx.send(pay(0), t);
  tx.send(pay(1), t);
  tx.tick(5.9);
  CHECK(!tx.nextRetransmit(5.9).has_value());  // not yet
  tx.tick(6.1);
  auto r0 = tx.nextRetransmit(6.1);
  auto r1 = tx.nextRetransmit(6.1);
  CHECK(r0 && r0->seq == 0 && r0->rv == 1);
  CHECK(r1 && r1->seq == 1 && r1->rv == 1);
}

TEST_CASE(timeout_clamp) {
  ArqTx tx(4, 8);
  tx.setTimeout(1.0);
  CHECK_NEAR(tx.timeoutSec(), 4.0, 1e-9);
  // Ceiling 90 s: deep CSS data+ACK round trips legitimately exceed
  // the old 45 s ceiling, which made every first transmission time out
  // before its ACK could physically return.
  tx.setTimeout(100.0);
  CHECK_NEAR(tx.timeoutSec(), 90.0, 1e-9);
}

// The ACK timeout must track the MEASURED round trip, not just burst
// airtime. A clean 30 dB one-way link delivered ZERO bytes because the
// airtime-only estimate (6.7 s at M10) sat far below the real one-way
// latency (median 12.4 s in the ST2 world, dominated by the receiver's
// decode ladder): every frame retransmitted three times before its ACK
// could physically arrive, and each retransmission lengthened the
// peer's decode queue further.
TEST_CASE(timeout_tracks_measured_round_trip) {
  ArqTx tx(4, 8);
  ArqRx rx(4);
  tx.setTimeout(6.0);  // airtime-only floor, far short of the truth
  CHECK(!tx.haveRtt());
  CHECK_NEAR(tx.timeoutSec(), 6.0, 1e-9);

  // A frame acknowledged on its first transmission after 20 s is an
  // unambiguous sample: the timeout must grow past the round trip.
  double t = 0.0;
  auto seq = tx.send(pay(0), t);
  CHECK(seq.has_value());
  rx.onData(*seq, pay(0));
  t = 20.0;
  tx.onAck(rx.makeAck(0, 0, 0), t);
  CHECK(tx.haveRtt());
  CHECK_NEAR(tx.srttSec(), 20.0, 1e-6);
  CHECK(tx.timeoutSec() > 20.0);
  CHECK(tx.timeoutSec() <= 90.0);
}

// Karn's algorithm: a retransmitted frame's ACK cannot be attributed to
// a particular transmission, so it must never become an RTT sample.
TEST_CASE(retransmitted_frames_never_sample_rtt) {
  ArqTx tx(4, 8);
  ArqRx rx(4);
  tx.setTimeout(5.0);
  double t = 0.0;
  auto seq = tx.send(pay(0), t);
  t = 6.0;
  tx.tick(t);                       // times out, requeues
  auto r = tx.nextRetransmit(t);
  CHECK(r.has_value());
  rx.onData(*seq, pay(0));
  t = 7.0;
  tx.onAck(rx.makeAck(0, 0, 0), t);
  CHECK(!tx.haveRtt());             // ambiguous: no sample taken
}

// Without a seed the estimator cannot bootstrap on a link whose round
// trip already exceeds the airtime floor: the first frames all time out,
// every one becomes a retransmission, and Karn (correctly) refuses to
// sample any of them. The handshake supplies the missing measurement.
TEST_CASE(seed_rtt_bootstraps_the_estimator) {
  ArqTx tx(4, 8);
  tx.setTimeout(6.0);
  tx.seedRtt(18.0);
  CHECK(tx.haveRtt());
  CHECK_NEAR(tx.srttSec(), 18.0, 1e-9);
  CHECK(tx.timeoutSec() > 18.0);
  // A seed never overrides a real measurement.
  tx.seedRtt(2.0);
  CHECK_NEAR(tx.srttSec(), 18.0, 1e-9);
  // Implausible seeds are ignored outright.
  ArqTx tx2(4, 8);
  tx2.seedRtt(-1.0);
  CHECK(!tx2.haveRtt());
  tx2.seedRtt(1e6);
  CHECK(!tx2.haveRtt());
}

// Consecutive timeouts back the interval off, so a persistently
// under-estimated timeout stops retransmitting at the same wrong
// cadence; a clean sample clears the backoff again.
TEST_CASE(timeout_backs_off_and_recovers) {
  ArqTx tx(4, 8);
  ArqRx rx(4);
  tx.setTimeout(5.0);
  double t = 0.0;
  tx.send(pay(0), t);
  const double base = tx.timeoutSec();
  t = 6.0;
  tx.tick(t);
  CHECK(tx.timeoutSec() > base);
  const double backedOff = tx.timeoutSec();
  t = 6.0 + backedOff + 1.0;
  tx.tick(t);
  CHECK(tx.timeoutSec() > backedOff);
  // Backoff is capped, not unbounded.
  for (int i = 0; i < 20; ++i) tx.tick(t + double(i) * 200.0);
  CHECK(tx.timeoutSec() <= 90.0);

  // A fresh frame acked on its first transmission clears the backoff.
  ArqTx tx3(4, 8);
  ArqRx rx3(4);
  tx3.setTimeout(5.0);
  double u = 0.0;
  tx3.send(pay(0), u);
  u = 6.0;
  tx3.tick(u);                      // backoff engaged
  auto rr = tx3.nextRetransmit(u);
  CHECK(rr.has_value());
  rx3.onData(rr->seq, pay(0));
  tx3.onAck(rx3.makeAck(0, 0, 0), u);   // ambiguous, no sample
  auto s2 = tx3.send(pay(1), u);
  CHECK(s2.has_value());
  rx3.onData(*s2, pay(1));
  u += 3.0;
  tx3.onAck(rx3.makeAck(0, 0, 0), u);   // clean sample -> backoff clears
  CHECK(tx3.haveRtt());
}

TEST_CASE(max_retries_teardown) {
  ArqTx tx(2, 3);
  tx.setTimeout(4.0);
  double t = 0;
  tx.send(pay(0), t);
  // Step by the CURRENT timeout, not a fixed interval: consecutive
  // expiries back the timeout off (see timeout_backs_off_and_recovers),
  // so a constant cadence no longer reaches the next deadline. What is
  // under test is the retry BUDGET, not the retry period.
  for (int i = 0; i < 3; ++i) {
    t += tx.timeoutSec() + 0.5;
    tx.tick(t);
    auto r = tx.nextRetransmit(t);
    CHECK(r.has_value());
  }
  t += tx.timeoutSec() + 0.5;
  tx.tick(t);  // 4th expiry exceeds maxRetries=3
  CHECK(tx.failed());
}

TEST_CASE(sequence_wraparound) {
  ArqTx tx(4, 8);
  ArqRx rx(4);
  // Fast-forward both sides to near the wrap by simulating traffic.
  double t = 0;
  for (int i = 0; i < 65534; ++i) {
    auto s = tx.send(pay(uint8_t(i)), t);
    rx.onData(*s, pay(uint8_t(i)));
    tx.onAck(rx.makeAck(0, 0, 0), t);
  }
  rx.takeDelivered();
  // Now cross the boundary with a loss.
  auto s0 = tx.send(pay(1), t);  // seq 65534
  auto s1 = tx.send(pay(2), t);  // seq 65535
  auto s2 = tx.send(pay(3), t);  // seq 0 (wrapped)
  CHECK_EQ(int(*s1), 65535);
  CHECK_EQ(int(*s2), 0);
  rx.onData(*s0, pay(1));
  // lose 65535
  rx.onData(*s2, pay(3));
  auto ack = rx.makeAck(0, 0, 0);
  CHECK(ack.isNak());
  CHECK_EQ(int(ack.ackBase), 65535);
  CHECK_EQ(int(ack.gapBitmap), 1);  // seq 0 arrived
  tx.onAck(ack, t);
  auto r = tx.nextRetransmit(t);
  CHECK(r && r->seq == 65535);
  rx.onData(r->seq, r->payload);
  CHECK_EQ(int(rx.ackBase()), 1);  // wrapped cleanly
  CHECK(!rx.makeAck(0, 0, 0).isNak());
}

TEST_CASE(loss_matrix) {
  // Every loss pattern over a 4-frame burst recovers within one
  // retransmit round.
  for (int mask = 0; mask < 15; ++mask) {  // 15 = all lost handled below
    ArqTx tx(4, 8);
    ArqRx rx(4);
    double t = 0;
    std::vector<bool> deliver(4);
    for (int k = 0; k < 4; ++k) deliver[size_t(k)] = (mask >> k) & 1;
    auto ack = exchange(tx, rx, deliver, t);
    tx.onAck(ack, t);
    // Retransmit whatever the ACK reported missing; then force a timeout
    // pass for losses the bitmap could not describe (trailing losses).
    for (int round = 0; round < 3 && rx.ackBase() != 4; ++round) {
      while (auto r = tx.nextRetransmit(t)) rx.onData(r->seq, r->payload);
      tx.onAck(rx.makeAck(0, 0, 0), t);
      if (rx.ackBase() != 4) {
        t += 46.0;
        tx.tick(t);
      }
    }
    CHECK_EQ(int(rx.ackBase()), 4);
    CHECK(!tx.failed());
    CHECK_EQ(int(tx.inFlight()), 0);
    auto got = rx.takeDelivered();
    CHECK_EQ(int(got.size()), 4);
    for (int k = 0; k < 4; ++k) CHECK(got[size_t(k)] == pay(uint8_t(k)));
  }
}

// PROTOCOL.md: every session frame piggybacks the peer's cumulative
// receive floor in the envelope. Consuming it releases delivered frames
// without waiting for a dedicated ACK - it was written on TX and ignored
// on RX, so every acknowledgement cost a whole extra frame.
TEST_CASE(piggyback_ack_releases_without_a_dedicated_ack) {
  ArqTx tx(8, 10);
  tx.setTimeout(10.0);
  double t = 100.0;
  for (int i = 0; i < 4; ++i) tx.send({uint8_t(i)}, t);
  CHECK_EQ(int(tx.inFlight()), 4);

  // Peer reports "next expected = 2": frames 0 and 1 are delivered.
  CHECK(tx.onPiggybackAck(2, t));
  CHECK_EQ(int(tx.inFlight()), 2);

  // The same floor again is not progress and must release nothing.
  CHECK(!tx.onPiggybackAck(2, t));
  CHECK_EQ(int(tx.inFlight()), 2);

  // It never queues a retransmission - it reports no holes.
  CHECK(!tx.peekRetransmit().has_value());

  CHECK(tx.onPiggybackAck(4, t));
  CHECK_EQ(int(tx.inFlight()), 0);

  // A floor beyond anything we ever sent is ignored: the envelope field
  // is unauthenticated, and with wrapped sequence numbers "release
  // everything below" would otherwise be exploitable nonsense.
  ArqTx t2(8, 10);
  t2.setTimeout(10.0);
  for (int i = 0; i < 3; ++i) t2.send({uint8_t(i)}, t);
  CHECK(!t2.onPiggybackAck(60000, t));
  CHECK_EQ(int(t2.inFlight()), 3);
}

// Forward progress resets the retry budget, exactly like a dedicated ACK:
// the budget exists to detect a dead peer, not a busy schedule.
TEST_CASE(piggyback_ack_resets_retry_budget) {
  ArqTx tx(8, 3);
  tx.setTimeout(10.0);
  double t = 100.0;
  for (int i = 0; i < 3; ++i) tx.send({uint8_t(i)}, t);

  // Two timeout rounds charge retries against everything in flight.
  for (int r = 0; r < 2; ++r) {
    t += 11.0;
    tx.tick(t);
    while (auto rt = tx.nextRetransmit(t)) { (void)rt; }
  }
  CHECK(!tx.failed());

  // Progress arrives piggybacked; the budget must reset, so the next two
  // timeout rounds cannot exhaust it.
  CHECK(tx.onPiggybackAck(1, t));
  for (int r = 0; r < 2; ++r) {
    t += 11.0;
    tx.tick(t);
    while (auto rt = tx.nextRetransmit(t)) { (void)rt; }
  }
  CHECK(!tx.failed());
}

// A peer stuck at the same receive floor must not be able to defer our
// retransmission timer forever: only forward progress re-arms it.
TEST_CASE(duplicate_ack_does_not_defer_the_timeout) {
  ArqTx tx(8, 10);
  tx.setTimeout(10.0);
  double t = 100.0;
  tx.send({1}, t);
  tx.send({2}, t);

  // Repeated ACKs reporting the same floor (head-of-line frame lost,
  // nothing after it received) arrive faster than the timeout.
  AckBody stuck;
  stuck.ackBase = 0;
  stuck.gapBitmap = 0;
  for (int i = 0; i < 5; ++i) {
    t += 2.0;
    tx.onAck(stuck, t);
  }
  // 10 s have passed: the timeout must have been reachable.
  t += 1.0;
  tx.tick(t);
  CHECK(tx.peekRetransmit().has_value());
}

SKYTEST_MAIN
