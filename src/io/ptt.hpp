// Physical PTT keying (INTERFACES.md "PTT").
//
// The modem owns burst-accurate keying only; frequency and rig CAT belong to
// the companion app. Four methods:
//
//   NONE  no keying at all (an external system keys the rig)
//   RTS   assert/release the serial RTS control line
//   DTR   assert/release the serial DTR control line
//   VOX   the rig keys itself off the audio; nothing to drive
//   TCP   send `KEY\n` / `UNKEY\n` to a companion-app PTT server
//
// Drivers are used from the modem worker thread only. Every call is
// non-blocking: a TCP server that vanishes must never stall the audio
// pipeline, so the driver reconnects opportunistically and reports its
// health through `healthy()`.
#pragma once

#include <memory>
#include <string>

#include "core/config.hpp"

namespace skw {

class PttDriver {
 public:
  virtual ~PttDriver() = default;

  // Assert / release the key. Return false when the transport failed; the
  // caller keeps its own notion of the intended state and retries.
  virtual bool key() = 0;
  virtual bool unkey() = 0;

  // False once the transport has failed (port gone, server dropped). A
  // driver may recover on a later key()/unkey().
  virtual bool healthy() const = 0;

  // Human-readable state for STATUS / logs.
  virtual const char* name() const = 0;

  // Last transport error, empty when healthy.
  virtual std::string lastError() const { return {}; }
};

// Never returns null: an unsupported or unopenable target yields a driver
// that reports itself unhealthy, so the modem still runs (and still meters
// audio) with keying disabled rather than refusing to start.
std::unique_ptr<PttDriver> makePttDriver(PttMethod method,
                                         const std::string& target, int baud);

}  // namespace skw
