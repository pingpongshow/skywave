// Proto payload pipeline (ROADMAP 4.1 + 4.2): zstd marker framing,
// compression threshold/negotiation, coalesce window, exact codeword-
// budget segmentation, more-fragments reassembly, and large-message
// round-trips through the full two-modem stack under injected loss.
#include <cstdio>
#include <string>
#include <vector>

#include "link/engine.hpp"
#include "link/frame.hpp"
#include "phy/framer.hpp"
#include "proto/pipeline.hpp"
#include "sim/chansim.hpp"
#include "skytest.hpp"

using namespace skw;

namespace {

std::vector<uint8_t> randomBytes(size_t n, uint64_t seed) {
  std::vector<uint8_t> v(n);
  uint64_t s = seed * 2654435761u + 1;
  for (auto& b : v) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    b = uint8_t(s >> 32);
  }
  return v;
}

}  // namespace

TEST_CASE(proto_zstd_roundtrip_and_cap) {
  auto raw = randomBytes(3000, 1);
  // Random data barely compresses but must still round-trip.
  auto z = zstdPack(raw);
  CHECK(!z.empty());
  auto back = zstdUnpack(z);
  CHECK(back.has_value());
  CHECK(*back == raw);
  // Decompress cap: a 9 MiB message must be rejected at the 8 MiB cap.
  std::vector<uint8_t> big(9u << 20, 0x42);
  auto zbig = zstdPack(big);
  CHECK(!zstdUnpack(zbig).has_value());
  CHECK(zstdUnpack(zbig, 16u << 20).has_value());
  // Garbage is rejected, not fatal.
  CHECK(!zstdUnpack(raw).has_value());
}

TEST_CASE(proto_marker_and_threshold) {
  // Compressible payload with zstd negotiated -> 0x5A marker + flag.
  {
    ProtoTx tx;
    ProtoRx rx;
    tx.setZstd(true);
    std::vector<uint8_t> msg(2000, uint8_t('A'));
    tx.submit(msg, 0.0);
    auto s = tx.nextSegment(4096, 1.0);  // past the coalesce window
    CHECK(s.has_value());
    CHECK((s->flags & kFlagCompressed) != 0);
    CHECK_EQ(int(s->bytes[0]), int(kProtoMarkerZstd));
    CHECK(s->bytes.size() < 200);  // actually compressed
    rx.onSegment(s->flags, s->bytes);
    CHECK(rx.takeBytes() == msg);
  }
  // Tiny message: compression cannot save 8 bytes -> stored.
  {
    ProtoTx tx;
    ProtoRx rx;
    tx.setZstd(true);
    const std::string tiny = "hi";
    tx.submit({reinterpret_cast<const uint8_t*>(tiny.data()), tiny.size()},
              0.0);
    auto s = tx.nextSegment(4096, 1.0);
    CHECK(s.has_value());
    CHECK((s->flags & kFlagCompressed) == 0);
    CHECK_EQ(int(s->bytes[0]), int(kProtoMarkerStored));
  }
  // zstd NOT negotiated: compressible data stays stored.
  {
    ProtoTx tx;
    tx.setZstd(false);
    std::vector<uint8_t> msg(2000, uint8_t('B'));
    tx.submit(msg, 0.0);
    auto s = tx.nextSegment(4096, 1.0);
    CHECK(s.has_value());
    CHECK((s->flags & kFlagCompressed) == 0);
    CHECK_EQ(int(s->bytes[0]), int(kProtoMarkerStored));
  }
}

TEST_CASE(proto_coalesce_window) {
  ProtoTx tx;
  ProtoRx rx;
  tx.setZstd(false);
  const std::string a = "abc", b = "def";
  tx.submit({reinterpret_cast<const uint8_t*>(a.data()), a.size()}, 0.0);
  CHECK(!tx.nextSegment(1000, 0.0).has_value());  // window open
  tx.submit({reinterpret_cast<const uint8_t*>(b.data()), b.size()}, 0.1);
  CHECK(!tx.nextSegment(1000, 0.2).has_value());  // still open (t0+250ms)
  auto s = tx.nextSegment(1000, 0.26);            // window expired
  CHECK(s.has_value());
  rx.onSegment(s->flags, s->bytes);
  auto got = rx.takeBytes();
  CHECK(got == std::vector<uint8_t>(
                   {uint8_t('a'), uint8_t('b'), uint8_t('c'),
                    uint8_t('d'), uint8_t('e'), uint8_t('f')}));

  // Window-full: a large write seals kCoalesceMaxBytes immediately.
  auto big = randomBytes(5000, 7);
  tx.submit(big, 10.0);
  auto s1 = tx.nextSegment(4200, 10.0);  // no window wait
  CHECK(s1.has_value());
  // marker + service tag + window
  CHECK_EQ(int(s1->bytes.size()), int(kCoalesceMaxBytes + 2));
  CHECK(!tx.nextSegment(4200, 10.0).has_value());  // remainder waits
  auto s2 = tx.nextSegment(4200, 10.3);
  CHECK(s2.has_value());
  rx.onSegment(s1->flags, s1->bytes);
  rx.onSegment(s2->flags, s2->bytes);
  CHECK(rx.takeBytes() == big);
}

TEST_CASE(proto_fragment_chain) {
  ProtoTx tx;
  ProtoRx rx;
  tx.setZstd(false);
  auto msg = randomBytes(500, 3);
  tx.submit(msg, 0.0);
  std::vector<ProtoSegment> segs;
  while (auto s = tx.nextSegment(57, 1.0)) segs.push_back(std::move(*s));
  CHECK(segs.size() >= 8);
  for (size_t i = 0; i + 1 < segs.size(); ++i) {
    CHECK((segs[i].flags & kFlagMoreFragments) != 0);
  }
  CHECK((segs.back().flags & kFlagMoreFragments) == 0);
  for (auto& s : segs) rx.onSegment(s.flags, s.bytes);
  CHECK(rx.takeBytes() == msg);
  CHECK(tx.idle());
}

TEST_CASE(proto_bad_messages) {
  ProtoRx rx;
  // Unknown marker.
  rx.onSegment(0, std::vector<uint8_t>{0x37, 1, 2, 3});
  CHECK_EQ(int(rx.badMessages()), 1);
  // zstd marker over garbage.
  rx.onSegment(0, std::vector<uint8_t>{kProtoMarkerZstd, 9, 9, 9});
  CHECK_EQ(int(rx.badMessages()), 2);
  CHECK(rx.takeBytes().empty());
  // Runaway more-fragments chain hits the cap and is dropped whole.
  std::vector<uint8_t> mb(1u << 20, 0x55);
  for (int i = 0; i < 9; ++i) rx.onSegment(kFlagMoreFragments, mb);
  CHECK_EQ(int(rx.badMessages()), 3);
  // ...and the chain's own final fragment goes with it (resync), rather
  // than being parsed as a message of its own.
  rx.onSegment(0, std::vector<uint8_t>{0x55});
  CHECK_EQ(int(rx.badMessages()), 3);
  // Unknown service tag is dropped and counted.
  rx.onSegment(0, std::vector<uint8_t>{kProtoMarkerStored, 0x7E, 1});
  CHECK_EQ(int(rx.badMessages()), 4);
  // A valid message afterwards still decodes.
  rx.onSegment(0,
               std::vector<uint8_t>{kProtoMarkerStored, kSvcStream, 42});
  CHECK(rx.takeBytes() == std::vector<uint8_t>{42});
  CHECK_EQ(int(rx.badMessages()), 4);
}

TEST_CASE(proto_resync_after_oversize_chain) {
  // Review D: after the 8 MiB drop the rest of the chain (more fragments
  // AND its tail) must be discarded, so the next real message - and only
  // it - is delivered intact.
  ProtoRx rx;
  std::vector<uint8_t> mb(1u << 20, 0x55);
  for (int i = 0; i < 12; ++i) rx.onSegment(kFlagMoreFragments, mb);
  // Tail fragment whose first byte happens to look like a stored marker.
  rx.onSegment(0, std::vector<uint8_t>{kProtoMarkerStored, kSvcStream, 66});
  CHECK(rx.takeBytes().empty());
  CHECK_EQ(int(rx.badMessages()), 1);
  rx.onSegment(0, std::vector<uint8_t>{kProtoMarkerStored, kSvcStream, 7});
  CHECK(rx.takeBytes() == std::vector<uint8_t>{7});
  CHECK_EQ(int(rx.badMessages()), 1);
}

TEST_CASE(proto_exact_codeword_budget) {
  // For every mode and burst profile: the budget must land on the target
  // codeword count, adding one byte must exceed it (or bust the PHY
  // length field), and residual codeword padding must come only from
  // byte flooring (< 8 bits).
  for (int mode = kFirstMode; mode < kNumModes; ++mode) {
    for (int fpb : {1, 2, 4}) {
      const int target = (mode <= 7 ? 1 : 2) * fpb;
      const size_t p = dataPayloadBudget(mode, fpb, target);
      const size_t per = fpb == 1 ? 13 : 15;
      const size_t phyLen = size_t(fpb) * (per + p);
      CHECK(payloadCodewordCount(phyLen, mode) <= target);
      const size_t phyNext = size_t(fpb) * (per + p + 1);
      CHECK(payloadCodewordCount(phyNext, mode) > target ||
            phyNext > 4095);
      // Padding: total codeword capacity minus actual info bits.
      const CodeRate r = kModes[size_t(mode)].rate;
      size_t rest = (phyLen - kHeaderPiggybackBytes) * 8;
      size_t cap = 0, left = rest;
      while (left > 0) {
        const size_t k =
            size_t(payloadCodewordN(left, r)) * r.num / r.den;
        cap += k;
        left -= std::min(left, k);
      }
      if (phyNext <= 4095) {  // interior budgets must be exact fills
        // Byte flooring happens per frame, so an F-entry container can
        // strand up to 8*F - 1 bits - never a whole wasted codeword.
        CHECK(cap - rest < 8 * size_t(fpb));
      }
    }
  }
  std::printf("budgets: M4 solo=%zu  M8 x2=%zu  M13 x4=%zu\n",
              dataPayloadBudget(4, 1, 1), dataPayloadBudget(8, 2, 4),
              dataPayloadBudget(13, 4, 8));
}

// --- full-stack round-trip under loss (ROADMAP 4.2 gate) -------------------

namespace {

struct Pair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;
  int steps = 0;

  explicit Pair(double snrDb, uint64_t seed) : a(mk("W1AW")), b(mk("K6XYZ")) {
    chanAB.fading = false;
    chanAB.snr3kDb = snrDb;
    chanBA = chanAB;
    chanAB.seed = seed;
    chanBA.seed = seed + 5000;
  }

  static EngineConfig mk(const char* call) {
    EngineConfig c;
    c.policy.ownCall = call;
    c.policy.caps = kCapZstd | kCapResume;
    return c;
  }

  void step(bool dropAB = false) {
    ++steps;
    auto burstA = a.tick(now);
    auto burstB = b.tick(now);
    double dt = 0.05;
    auto push = [&](std::vector<cfloat>& burst, LinkEngine& to,
                    ChansimParams& chan) {
      dt += double(burst.size()) / kFsBaseband;
      std::vector<cfloat> x(1500, cfloat(0, 0));
      x.insert(x.end(), burst.begin(), burst.end());
      x.insert(x.end(), 2000, cfloat(0, 0));
      ChansimParams cp = chan;
      cp.seed = chan.seed + (seedCtr++) * 7919;
      auto y = Chansim::run(cp, x);
      to.rxBurst(y, now);
    };
    if (burstA && !dropAB) push(*burstA, b, chanAB);
    if (burstB) push(*burstB, a, chanBA);
    now += dt;
  }

  bool connect() {
    a.connectTo("K6XYZ", now);
    for (int i = 0; i < 400; ++i) {
      step();
      if (a.state() == FsmState::Connected &&
          b.state() == FsmState::Connected) {
        return true;
      }
    }
    return false;
  }

  bool transfer(const std::vector<uint8_t>& blob, int lossEvery,
                int budget = 6000) {
    a.sendData(blob);
    std::vector<uint8_t> got;
    int n = 0;
    for (int i = 0; i < budget && got.size() < blob.size(); ++i) {
      const bool drop = lossEvery > 0 && (++n % lossEvery) == 0;
      step(drop);
      auto d = b.takeReceivedData();
      got.insert(got.end(), d.begin(), d.end());
      if (a.state() != FsmState::Connected) return false;
    }
    return got == blob;
  }
};

}  // namespace

TEST_CASE(proto_bulk_under_loss) {
  // 16 KiB incompressible with 1-in-7 A->B bursts dropped: the fragment
  // chains cross many ARQ retransmission/reordering boundaries and must
  // reassemble byte-exact.
  Pair p(25.0, 4100);
  CHECK(p.connect());
  CHECK(p.transfer(randomBytes(16 * 1024, 99), 7));
  std::printf("incompressible 16 KiB under loss: %d steps\n", p.steps);

  // Same size, compressible content, same loss: must arrive byte-exact
  // and measurably faster (zstd actually shortening the air time).
  Pair q(25.0, 4100);
  CHECK(q.connect());
  std::vector<uint8_t> text;
  const std::string line =
      "CQ CQ CQ de W1AW W1AW K   the quick brown fox jumps over ";
  while (text.size() < 16 * 1024) {
    text.insert(text.end(), line.begin(), line.end());
  }
  text.resize(16 * 1024);
  CHECK(q.transfer(text, 7));
  std::printf("compressible   16 KiB under loss: %d steps\n", q.steps);
  CHECK(q.steps < p.steps / 2);
}

SKYTEST_MAIN
