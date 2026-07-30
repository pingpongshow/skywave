// File-transfer service (ROADMAP 4.3): record formats, FileTx/FileRx
// checkpointing + CRC verification, and the two-modem transfer scenario
// (loss, interleaved stream traffic, EVT surface).
#include <cstdio>
#include <string>
#include <vector>

#include "link/engine.hpp"
#include "proto/filesvc.hpp"
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

TEST_CASE(file_records_roundtrip) {
  FileMeta m;
  m.fileId = 0xDEAD01;
  m.name = "logs/pass_2026-07-29.txt";
  m.size = 123456;
  m.mtime = 1785600000;
  m.crc32 = 0xCAFEF00D;
  m.chunkSize = 1024;
  auto p = m.pack();
  auto q = FileMeta::parse(p);
  CHECK(q.has_value());
  CHECK_EQ(q->fileId, m.fileId);
  CHECK(q->name == m.name);
  CHECK_EQ(int(q->size), int(m.size));
  CHECK_EQ(int(q->mtime & 0xFFFFFFF), int(m.mtime & 0xFFFFFFF));
  CHECK_EQ(int(q->crc32), int(m.crc32));
  CHECK_EQ(int(q->chunkSize), int(m.chunkSize));
  // Malformed: truncated, wrong record type, zero chunk size.
  CHECK(!FileMeta::parse({p.data(), p.size() - 3}).has_value());
  auto bad = p;
  bad[0] = kFileRecChunk;
  CHECK(!FileMeta::parse(bad).has_value());
  auto zc = m;
  zc.chunkSize = 0;
  CHECK(!FileMeta::parse(zc.pack()).has_value());
  // Chunk layout: tag + id + offset + payload.
  auto c = packFileChunk(7, 65536, std::vector<uint8_t>{1, 2, 3});
  CHECK_EQ(int(c.size()), 16);
  CHECK_EQ(int(c[0]), int(kFileRecChunk));
}

TEST_CASE(file_tx_rx_direct) {
  // 100 KiB pumped straight across: checkpoints at every 32 KiB, CRC
  // verified, byte-exact.
  auto blob = randomBytes(100 * 1024, 5);
  FileTx tx;
  FileRx rx;
  CHECK(tx.start(42, "blob.bin", blob, 1785601234));
  CHECK(!tx.start(43, "other.bin", {1, 2}, 0));  // one at a time
  while (auto m = tx.nextMessage()) rx.onMessage(*m);
  CHECK(tx.drained());
  auto evs = rx.takeEvents();
  int started = 0, checkpoints = 0, done = 0;
  for (auto& e : evs) {
    if (e.kind == FileRxEvent::Kind::Started) ++started;
    if (e.kind == FileRxEvent::Kind::Checkpoint) ++checkpoints;
    if (e.kind == FileRxEvent::Kind::Done) {
      ++done;
      CHECK(e.crcOk);
      CHECK_EQ(int(e.offset), int(blob.size()));
    }
  }
  CHECK_EQ(started, 1);
  CHECK_EQ(checkpoints, 3);  // 32/64/96 KiB
  CHECK_EQ(done, 1);
  auto got = rx.takeCompleted();
  CHECK(got.has_value());
  CHECK(got->data == blob);
  CHECK(got->crcOk);
  CHECK(got->meta.name == "blob.bin");
}

TEST_CASE(file_rx_gap_and_zero_length) {
  // A skipped chunk (impossible over ARQ, defensive) errors out cleanly.
  FileTx tx;
  FileRx rx;
  auto blob = randomBytes(4096, 9);
  CHECK(tx.start(1, "gap.bin", blob, 0));
  rx.onMessage(*tx.nextMessage());  // META
  (void)tx.nextMessage();           // drop the first chunk
  rx.onMessage(*tx.nextMessage());  // offset 1024 -> gap
  bool sawError = false;
  for (auto& e : rx.takeEvents()) {
    if (e.kind == FileRxEvent::Kind::Error) {
      sawError = true;
      CHECK(e.reason == "gap");
    }
  }
  CHECK(sawError);
  CHECK(!rx.inProgress());

  // Zero-length file: META alone completes with a passing CRC.
  FileTx tz;
  FileRx rz;
  CHECK(tz.start(2, "empty.bin", {}, 0));
  while (auto m = tz.nextMessage()) rz.onMessage(*m);
  auto got = rz.takeCompleted();
  CHECK(got.has_value());
  CHECK(got->data.empty());
  CHECK(got->crcOk);
}

// --- two-modem scenario ------------------------------------------------------

namespace {

struct Pair {
  LinkEngine a, b;
  ChansimParams chanAB, chanBA;
  double now = 0.0;
  uint64_t seedCtr = 1;

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

  void step(bool dropAB = false, bool dropBA = false) {
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
    if (burstB && !dropBA) push(*burstB, a, chanBA);
    now += dt;
  }

  // Total link death until both ends give the session up.
  bool blackout() {
    for (int i = 0; i < 4000 && (a.state() != FsmState::Idle ||
                                 b.state() != FsmState::Idle);
         ++i) {
      step(true, true);
      now += 2.0;  // let ARQ/DISC/idle timers run their course
    }
    return a.state() == FsmState::Idle && b.state() == FsmState::Idle;
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
};

}  // namespace

TEST_CASE(file_two_modem_transfer) {
  // 80 KiB incompressible file under 1-in-9 loss, with interleaved
  // stream traffic: file arrives byte-exact + CRC-clean, stream bytes
  // route independently, both EVT surfaces fire.
  Pair p(25.0, 7300);
  CHECK(p.connect());
  auto blob = randomBytes(80 * 1024, 11);
  CHECK(p.a.sendFile("pass.bin", blob, 1785600000));
  CHECK(!p.a.sendFile("second.bin", blob, 0));  // busy
  const std::vector<uint8_t> chat = {'h', 'e', 'l', 'l', 'o'};
  std::vector<uint8_t> stream;
  std::vector<std::string> evA, evB;
  std::optional<ReceivedFile> got;
  int n = 0, chatsSent = 0;
  // Loop until the file AND all interleaved chats are through: a faster
  // AMC ramp can finish the file before iteration-paced chats flush.
  for (int i = 0;
       i < 9000 && (!got || chatsSent < 5 || stream.size() < 25); ++i) {
    if (i % 40 == 0 && chatsSent < 5) {
      p.a.sendData(chat);  // interleave stream traffic mid-file
      ++chatsSent;
    }
    p.step((++n % 9) == 0);
    auto d = p.b.takeReceivedData();
    stream.insert(stream.end(), d.begin(), d.end());
    for (auto& e : p.a.takeEvents()) evA.push_back(e);
    for (auto& e : p.b.takeEvents()) evB.push_back(e);
    if (!got) got = p.b.takeReceivedFile();
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK(got.has_value());
  CHECK(got->crcOk);
  CHECK(got->data == blob);
  CHECK(got->meta.name == "pass.bin");
  std::printf("  interleaved stream: %zu bytes (expect %zu)\n",
              stream.size(), chat.size() * 5);
  CHECK_EQ(int(stream.size()), int(chat.size() * 5));
  // Receiver surfaced EVT:FILE_RX; sender finishes with EVT:TXCOMPLETE
  // once the pipeline fully drains (give it a few more exchanges).
  bool fileRxEvt = false;
  for (auto& e : evB) {
    if (e.find("EVT:FILE_RX pass.bin 81920 1") == 0) fileRxEvt = true;
  }
  CHECK(fileRxEvt);
  bool txDone = false;
  for (int i = 0; i < 200 && !txDone; ++i) {
    p.step();
    for (auto& e : p.a.takeEvents()) {
      if (e == "EVT:TXCOMPLETE") txDone = true;
    }
  }
  CHECK(txDone);
  CHECK(!p.a.fileTxActive());
  std::printf("file transfer done in %.0f s simulated\n", p.now);
}

TEST_CASE(resume_wire_roundtrip) {
  StartAckBody sa;
  sa.finalMode = 8;
  sa.hasResumeToken = true;
  sa.resumeToken = {1, 2, 3, 4, 5, 6, 7, 8};
  auto q = StartAckBody::parse(sa.pack());
  CHECK(q.has_value());
  CHECK(q->hasResumeToken);
  CHECK(q->resumeToken == sa.resumeToken);

  ResumeReqBody rr;
  rr.token = sa.resumeToken;
  rr.lastRxSeq = 555;
  rr.hasFileCkpt = true;
  rr.fileId = 42;
  rr.fileOffset = 65536;
  auto r2 = ResumeReqBody::parse(rr.pack());
  CHECK(r2.has_value());
  CHECK(r2->token == rr.token);
  CHECK_EQ(int(r2->lastRxSeq), 555);
  CHECK(r2->hasFileCkpt);
  CHECK_EQ(int(r2->fileOffset), 65536);

  ResumeAckBody ra;
  ra.lastRxSeq = 700;
  auto a2 = ResumeAckBody::parse(ra.pack());
  CHECK(a2.has_value());
  CHECK(!a2->hasFileCkpt);
  CHECK_EQ(int(a2->lastRxSeq), 700);

  auto nak = StartNakBody::parse(
      StartNakBody{StartNakReason::ResumeInvalid}.pack());
  CHECK(nak.has_value());
  CHECK(nak->reason == StartNakReason::ResumeInvalid);
}

TEST_CASE(file_resume_after_link_death) {
  // ROADMAP 4.4 gate: kill the link mid-transfer, reconnect inside the
  // resume window, and the file completes CRC-clean from the checkpoint
  // - no restart from zero.
  Pair p(25.0, 9100);
  CHECK(p.connect());
  auto blob = randomBytes(80 * 1024, 21);
  CHECK(p.a.sendFile("resume.bin", blob, 1785600000));
  for (int i = 0; i < 4000 && p.b.fileRxCheckpoint() < 32 * 1024; ++i) {
    p.step();
  }
  CHECK(p.b.fileRxCheckpoint() >= 32 * 1024);
  // The link dies. Both sides give up the session but keep the token,
  // pipeline, and file state (failure-mode teardown, not operator DISC).
  CHECK(p.blackout());
  CHECK(p.a.hasResumeContext());
  CHECK(p.b.hasResumeContext());
  const uint64_t ckpt = p.b.fileRxCheckpoint();
  CHECK(ckpt >= 32 * 1024);
  std::printf("resume: link died at checkpoint %llu\n",
              (unsigned long long)ckpt);
  // Reconnect well inside the 10 min window.
  p.now += 30.0;
  p.a.connectTo("K6XYZ", p.now);
  for (int i = 0; i < 400 && (p.a.state() != FsmState::Connected ||
                              p.b.state() != FsmState::Connected);
       ++i) {
    p.step();
  }
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(p.a.sessionResumed());
  CHECK(p.b.sessionResumed());
  // The transfer continues to a CRC-clean completion.
  std::optional<ReceivedFile> got;
  for (int i = 0; i < 6000 && !got; ++i) {
    p.step();
    if (!got) got = p.b.takeReceivedFile();
    if (p.a.state() != FsmState::Connected) break;
  }
  CHECK(got.has_value());
  CHECK(got->crcOk);
  CHECK(got->data == blob);
  CHECK(got->meta.name == "resume.bin");
}

TEST_CASE(resume_invalid_falls_back_to_clean_start) {
  // The responder lost its state (power cycle): RESUME_REQ draws
  // START_NAK(ResumeInvalid) and the same call completes as a clean
  // session - never a protocol failure.
  Pair p(25.0, 9200);
  CHECK(p.connect());
  auto blob = randomBytes(48 * 1024, 22);
  CHECK(p.a.sendFile("lost.bin", blob, 0));
  for (int i = 0; i < 4000 && p.b.fileRxCheckpoint() < 32 * 1024; ++i) {
    p.step();
  }
  CHECK(p.blackout());
  CHECK(p.a.hasResumeContext());
  p.b = LinkEngine(Pair::mk("K6XYZ"));  // responder restarted: no token
  p.now += 10.0;
  p.a.connectTo("K6XYZ", p.now);
  for (int i = 0; i < 400 && (p.a.state() != FsmState::Connected ||
                              p.b.state() != FsmState::Connected);
       ++i) {
    p.step();
  }
  CHECK(p.a.state() == FsmState::Connected);
  CHECK(!p.a.sessionResumed());
  CHECK(!p.a.hasResumeContext() || p.a.sessionResumed() == false);
  // The paused file died with the fallback; the operator is told.
  bool fileError = false;
  for (auto& e : p.a.takeEvents()) {
    if (e.find("EVT:FILE_ERROR lost.bin") == 0) fileError = true;
  }
  CHECK(fileError);
  CHECK(!p.a.fileTxActive());
  // The clean session still works end to end.
  const std::vector<uint8_t> msg = {'o', 'k'};
  p.a.sendData(msg);
  std::vector<uint8_t> got;
  for (int i = 0; i < 400 && got.size() < msg.size(); ++i) {
    p.step();
    auto d = p.b.takeReceivedData();
    got.insert(got.end(), d.begin(), d.end());
  }
  CHECK(got == msg);
}

SKYTEST_MAIN
