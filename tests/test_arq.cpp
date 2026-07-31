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

TEST_CASE(max_retries_teardown) {
  ArqTx tx(2, 3);
  tx.setTimeout(4.0);
  double t = 0;
  tx.send(pay(0), t);
  for (int i = 0; i < 3; ++i) {
    t += 4.5;
    tx.tick(t);
    auto r = tx.nextRetransmit(t);
    CHECK(r.has_value());
  }
  t += 4.5;
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

SKYTEST_MAIN
