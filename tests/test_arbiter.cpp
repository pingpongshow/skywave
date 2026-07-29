// TX arbiter (ROADMAP 2.5): priority/bypass cases, CCA gate with
// proceed-anyway cap, PTT sequencing, watchdog fault injection, TXTEST
// gating.
#include <vector>

#include "link/arbiter.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

ArbiterConfig cfg() {
  ArbiterConfig c;
  c.leadSec = 0.05;
  c.tailSec = 0.02;
  c.watchdogSec = 10.0;  // small for tests
  return c;
}

std::vector<TxArbiter::Event> drain(TxArbiter& a) {
  std::vector<TxArbiter::Event> v;
  while (auto e = a.nextEvent()) v.push_back(*e);
  return v;
}

// Run ticks at 10 ms steps until `until`.
void run(TxArbiter& a, double from, double until) {
  for (double t = from; t <= until + 1e-9; t += 0.010) a.tick(t);
}

}  // namespace

TEST_CASE(priority_ordering) {
  TxArbiter a(cfg());
  a.enqueue({1, TxPriority::Beacon, false, 0.1});
  a.enqueue({2, TxPriority::Data, false, 0.1});
  a.enqueue({3, TxPriority::ControlUrgent, true, 0.1});
  a.enqueue({4, TxPriority::Retransmit, false, 0.1});
  a.enqueue({5, TxPriority::Data, false, 0.1});  // FIFO after id 2

  std::vector<uint32_t> order;
  double t = 0;
  for (int burst = 0; burst < 5; ++burst) {
    run(a, t, t + 0.5);
    t += 0.5 + 0.011;
    for (auto& e : drain(a)) {
      if (e.kind == TxArbiter::Event::Kind::BeginAudio) order.push_back(e.id);
    }
  }
  CHECK_EQ(int(order.size()), 5);
  CHECK_EQ(int(order[0]), 3);  // control-urgent
  CHECK_EQ(int(order[1]), 4);  // retransmit
  CHECK_EQ(int(order[2]), 2);  // data FIFO
  CHECK_EQ(int(order[3]), 5);
  CHECK_EQ(int(order[4]), 1);  // beacon last
}

TEST_CASE(ptt_sequencing_timing) {
  TxArbiter a(cfg());
  a.enqueue({7, TxPriority::Data, false, 0.30});
  a.tick(0.0);
  auto v = drain(a);
  CHECK_EQ(int(v.size()), 1);
  CHECK(v[0].kind == TxArbiter::Event::Kind::KeyPtt);
  CHECK(a.state() == TxState::Lead);
  // Audio begins only after the 50 ms lead.
  a.tick(0.049);
  CHECK(drain(a).empty());
  a.tick(0.051);
  v = drain(a);
  CHECK(v.size() == 1 && v[0].kind == TxArbiter::Event::Kind::BeginAudio);
  // Audio ends after airtime; unkey after tail.
  a.tick(0.351 + 0.011);
  v = drain(a);
  CHECK(v.size() == 1 && v[0].kind == TxArbiter::Event::Kind::EndAudio);
  CHECK(a.state() == TxState::Tail);
  a.tick(0.362 + 0.021);
  v = drain(a);
  CHECK(v.size() == 1 && v[0].kind == TxArbiter::Event::Kind::UnkeyPtt);
  CHECK(a.state() == TxState::Rx);
}

TEST_CASE(cca_blocks_then_proceeds_at_cap) {
  TxArbiter a(cfg());
  a.setBusy(true);
  a.enqueue({9, TxPriority::Data, false, 0.1});
  a.tick(0.0);
  CHECK(a.state() == TxState::CcaWait);
  CHECK(drain(a).empty());
  // Stays blocked while busy (polls every 250 ms).
  run(a, 0.0, 4.5);
  CHECK(a.state() == TxState::CcaWait);
  // At the 5 s cap: proceed anyway.
  run(a, 4.5, 5.1);
  CHECK(a.state() != TxState::CcaWait);
  auto v = drain(a);
  CHECK(!v.empty() && v[0].kind == TxArbiter::Event::Kind::KeyPtt);
}

TEST_CASE(cca_clears_early_and_bypass) {
  // Busy clears at 1 s: TX starts at the next poll.
  {
    TxArbiter a(cfg());
    a.setBusy(true);
    a.enqueue({1, TxPriority::Data, false, 0.1});
    a.tick(0.0);
    run(a, 0.0, 0.9);
    CHECK(a.state() == TxState::CcaWait);
    a.setBusy(false);
    run(a, 0.9, 1.3);
    CHECK(a.state() != TxState::CcaWait);
  }
  // Bypass flag ignores busy entirely.
  {
    TxArbiter a(cfg());
    a.setBusy(true);
    a.enqueue({2, TxPriority::ControlUrgent, true, 0.1});
    a.tick(0.0);
    CHECK(a.state() == TxState::Lead);
  }
  // preTxCca=false disables the gate.
  {
    ArbiterConfig c = cfg();
    c.preTxCca = false;
    TxArbiter a(c);
    a.setBusy(true);
    a.enqueue({3, TxPriority::Data, false, 0.1});
    a.tick(0.0);
    CHECK(a.state() == TxState::Lead);
  }
}

TEST_CASE(watchdog_aborts_runaway_tx) {
  TxArbiter a(cfg());  // watchdog 10 s
  a.enqueue({5, TxPriority::Data, false, 60.0});  // absurd airtime
  run(a, 0.0, 0.1);
  drain(a);
  run(a, 0.1, 10.2);
  auto v = drain(a);
  bool abort = false, unkey = false;
  for (auto& e : v) {
    if (e.kind == TxArbiter::Event::Kind::WatchdogAbort && e.id == 5) {
      abort = true;
    }
    if (e.kind == TxArbiter::Event::Kind::UnkeyPtt) unkey = true;
  }
  CHECK(abort);
  CHECK(unkey);
  CHECK(a.state() == TxState::Rx);
}

TEST_CASE(txtest_gating) {
  TxArbiter a(cfg());
  // Refused while connected.
  a.setConnected(true);
  CHECK(!a.txTest(0.0, 10.0).has_value());
  a.setConnected(false);
  // Refused with traffic queued.
  a.enqueue({1, TxPriority::Data, false, 0.1});
  CHECK(!a.txTest(0.0, 10.0).has_value());
  run(a, 0.0, 0.3);
  drain(a);
  CHECK(a.state() == TxState::Rx);
  // Accepted when idle; duration caps at 30 s.
  auto id = a.txTest(1.0, 120.0);
  CHECK(id.has_value());
  run(a, 1.0, 1.1);
  drain(a);
  CHECK(a.state() == TxState::Audio);
  // Audio must end at the 30 s cap (well before the 120 requested);
  // watchdog (10 s here) fires first in this small-config test, which
  // also proves the cap chain is bounded.
  run(a, 1.1, 12.0);
  auto v = drain(a);
  bool ended = false;
  for (auto& e : v) {
    if (e.kind == TxArbiter::Event::Kind::EndAudio ||
        e.kind == TxArbiter::Event::Kind::WatchdogAbort) {
      ended = true;
    }
  }
  CHECK(ended);
  CHECK(a.state() == TxState::Rx);
}

SKYTEST_MAIN
