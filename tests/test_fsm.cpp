// Session FSM (ROADMAP 2.2): table-driven transition coverage including
// every timeout path, retry budgets, idle disconnect, keepalive, and the
// airtime-derived timeout formula against real PHY airtime figures.
#include <vector>

#include "link/fsm.hpp"
#include "phy/framer.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

FsmConfig quickCfg() {
  FsmConfig c;
  c.timings = {2.0, 2.0, 2.0, 2.0, 1.0};  // timeouts clamp to 12 s
  return c;
}

std::vector<FsmAction> drain(SessionFsm& f) {
  std::vector<FsmAction> out;
  while (auto a = f.nextAction()) out.push_back(*a);
  return out;
}

bool has(const std::vector<FsmAction>& v, FsmAction a) {
  for (auto x : v) {
    if (x == a) return true;
  }
  return false;
}

}  // namespace

TEST_CASE(timeout_clamp_formula) {
  CHECK_NEAR(clampResponseTimeout(3.0), 12.0, 1e-9);
  CHECK_NEAR(clampResponseTimeout(20.0), 20.0, 1e-9);
  CHECK_NEAR(clampResponseTimeout(60.0), 45.0, 1e-9);
}

TEST_CASE(happy_path_initiator_and_responder) {
  SessionFsm a(quickCfg());  // initiator
  SessionFsm b(quickCfg());  // responder
  double t = 0;

  a.cmdConnect(t);
  auto av = drain(a);
  CHECK(has(av, FsmAction::SendCall));
  CHECK(a.state() == FsmState::Calling);

  b.rxCall(t += 2);
  auto bv = drain(b);
  CHECK(has(bv, FsmAction::SendCallAck));
  CHECK(b.state() == FsmState::CallAnswered);

  a.rxCallAck(t += 2);
  av = drain(a);
  CHECK(has(av, FsmAction::SendStart));
  CHECK(a.state() == FsmState::WaitStartAck);

  b.rxStart(t += 2, true);
  bv = drain(b);
  CHECK(has(bv, FsmAction::SendStartAck));
  CHECK(has(bv, FsmAction::IndicateConnected));
  CHECK(b.state() == FsmState::Connected);

  a.rxStartAck(t += 2);
  av = drain(a);
  CHECK(has(av, FsmAction::IndicateConnected));
  CHECK(a.state() == FsmState::Connected);
}

TEST_CASE(start_nak_paths) {
  // Responder rejects.
  SessionFsm b(quickCfg());
  b.rxCall(0);
  drain(b);
  b.rxStart(1, false);
  auto bv = drain(b);
  CHECK(has(bv, FsmAction::SendStartNak));
  CHECK(has(bv, FsmAction::IndicateDisconnected));
  CHECK(b.state() == FsmState::Idle);
  CHECK(b.lastDiscCause() == FsmDiscCause::StartNak);

  // Initiator receives NAK.
  SessionFsm a(quickCfg());
  a.cmdConnect(0);
  drain(a);
  a.rxCallAck(1);
  drain(a);
  a.rxStartNak(2);
  auto av = drain(a);
  CHECK(has(av, FsmAction::IndicateDisconnected));
  CHECK(a.state() == FsmState::Idle);
  CHECK(a.lastDiscCause() == FsmDiscCause::StartNak);
}

TEST_CASE(call_retry_budget_then_teardown) {
  SessionFsm a(quickCfg());
  a.cmdConnect(0);
  int sends = int(drain(a).size());  // initial SendCall
  // 4 attempts total: initial + 3 retries; each timeout is 12 s (clamped).
  double t = 12.5;
  for (int i = 0; i < 3; ++i) {
    a.tick(t);
    auto v = drain(a);
    CHECK(has(v, FsmAction::SendCall));
    sends += int(v.size());
    t += 12.5;
  }
  CHECK_EQ(sends, 4);
  a.tick(t);
  auto v = drain(a);
  CHECK(has(v, FsmAction::IndicateDisconnected));
  CHECK(a.state() == FsmState::Idle);
  CHECK(a.lastDiscCause() == FsmDiscCause::CallRetriesExhausted);
}

TEST_CASE(start_retry_budget) {
  SessionFsm a(quickCfg());
  a.cmdConnect(0);
  drain(a);
  a.rxCallAck(1);
  drain(a);
  double t = 13.5;
  for (int i = 0; i < 3; ++i) {
    a.tick(t);
    CHECK(has(drain(a), FsmAction::SendStart));
    t += 12.5;
  }
  a.tick(t);
  CHECK(a.state() == FsmState::Idle);
  CHECK(a.lastDiscCause() == FsmDiscCause::StartRetriesExhausted);
}

TEST_CASE(responder_start_window_expiry) {
  SessionFsm b(quickCfg());
  b.rxCall(0);
  drain(b);
  b.tick(12.001);
  CHECK(b.state() == FsmState::Idle);
  CHECK(b.lastDiscCause() == FsmDiscCause::IdleTimeout);
}

TEST_CASE(disconnect_paths) {
  // Orderly local disconnect with DISC_ACK.
  {
    SessionFsm a(quickCfg());
    a.cmdConnect(0);
    drain(a);
    a.rxCallAck(1);
    drain(a);
    a.rxStartAck(2);
    drain(a);
    a.cmdDisconnect(3);
    auto v = drain(a);
    CHECK(has(v, FsmAction::SendDisc));
    CHECK(a.state() == FsmState::Disconnecting);
    a.rxDiscAck(4);
    v = drain(a);
    CHECK(has(v, FsmAction::IndicateDisconnected));
    CHECK(a.lastDiscCause() == FsmDiscCause::LocalRequest);
  }
  // DISC retry cadence (2 * airtime + 3 = 5 s here), 3 attempts, then
  // unilateral teardown.
  {
    SessionFsm a(quickCfg());
    a.cmdConnect(0);
    drain(a);
    a.rxCallAck(1);
    drain(a);
    a.rxStartAck(2);
    drain(a);
    a.cmdDisconnect(3);
    drain(a);
    double t = 8.5;
    for (int i = 0; i < 2; ++i) {
      a.tick(t);
      CHECK(has(drain(a), FsmAction::SendDisc));
      t += 5.5;
    }
    a.tick(t);
    CHECK(a.state() == FsmState::Idle);
    CHECK(a.lastDiscCause() == FsmDiscCause::DiscRetriesExhausted);
  }
  // Peer-initiated DISC.
  {
    SessionFsm b(quickCfg());
    b.rxCall(0);
    drain(b);
    b.rxStart(1, true);
    drain(b);
    b.rxDisc(2);
    auto v = drain(b);
    CHECK(has(v, FsmAction::SendDiscAck));
    CHECK(has(v, FsmAction::IndicateDisconnected));
    CHECK(b.lastDiscCause() == FsmDiscCause::PeerRequest);
  }
}

TEST_CASE(idle_disconnect_and_keepalive) {
  FsmConfig cfg = quickCfg();
  cfg.keepaliveEnabled = true;
  SessionFsm a(cfg);
  a.cmdConnect(0);
  drain(a);
  a.rxCallAck(1);
  drain(a);
  a.rxStartAck(2);
  drain(a);
  // Keepalives every 20 s of TX idle.
  a.tick(22.5);
  CHECK(has(drain(a), FsmAction::SendKeepalive));
  a.tick(30.0);
  CHECK(drain(a).empty());
  a.tick(42.6);
  CHECK(has(drain(a), FsmAction::SendKeepalive));
  // Traffic refreshes the idle timer.
  a.rxTraffic(60.0);
  a.tick(121.0);  // 61 s since traffic - still alive
  CHECK(a.state() == FsmState::Connected);
  // 120 s with nothing -> unilateral teardown with one DISC.
  a.tick(180.5);
  auto v = drain(a);
  CHECK(has(v, FsmAction::SendDisc));
  CHECK(has(v, FsmAction::IndicateDisconnected));
  CHECK(a.state() == FsmState::Idle);
  CHECK(a.lastDiscCause() == FsmDiscCause::IdleTimeout);
}

TEST_CASE(ignores_out_of_state_events) {
  SessionFsm a(quickCfg());
  // Nothing should happen from any of these in Idle.
  a.rxCallAck(0);
  a.rxStartAck(0);
  a.rxStartNak(0);
  a.rxDiscAck(0);
  a.rxTraffic(0);
  a.tick(1000.0);
  CHECK(drain(a).empty());
  CHECK(a.state() == FsmState::Idle);
  // Connect twice: second is a no-op.
  a.cmdConnect(0);
  drain(a);
  a.cmdConnect(1);
  CHECK(drain(a).empty());
}

TEST_CASE(airtime_derived_timeouts_real_phy) {
  // Handshake frames ride calling chirp + robust header + M4. Timeouts
  // from real airtime: WIDE should sit inside the clamp window; NARROW
  // robust exchanges sit near the upper clamp by design.
  std::vector<uint8_t> callBody(40);
  auto air = [&](Family f) {
    TxFrameParams p;
    p.family = f;
    p.sig = ChirpSig::Calling;
    p.mode = 4;
    p.payload = callBody;
    return frameAirtimeSec(p);
  };
  double wide = air(Family::Wide);
  double narrow = air(Family::Narrow);
  CHECK(wide > 1.0);
  CHECK(narrow > wide);
  double wideTimeout = clampResponseTimeout(wide + wide + 6.0);
  double narrowTimeout = clampResponseTimeout(narrow + narrow + 6.0);
  CHECK(wideTimeout >= 12.0);
  CHECK(narrowTimeout >= wideTimeout);
  CHECK(narrowTimeout <= 45.0);
}

SKYTEST_MAIN
