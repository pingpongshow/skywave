// Advisor layer (LINK_ADAPTATION.md "Advisor Layer", ROADMAP 6.3): a
// bounded, deterministic, dependency-free predictive helper.
//
//   domains : AMC step bias, CFO drift shaping, mask threshold tuning
//   modes   : off / shadow (log only) / assist (apply when confident)
//   bounds  : single-step influence only, clamped to legal ranges,
//             never overrides handshake mode selection, no voice effect
//   control : environment variables ONLY (CONFIGURATION.md) - the
//             Advisor deliberately has no INI/GUI/TCP surface
//
// Every predictor is closed-form over a short bounded history (least
// squares / event counting): same inputs, same advice, no learned state.
#pragma once

#include <cstdint>
#include <deque>

namespace skw {

enum class AdvisorMode : uint8_t { Off, Shadow, Assist };

struct AdvisorConfig {
  AdvisorMode mode = AdvisorMode::Shadow;
  double amcConf = 0.6;   // per-domain confidence floors
  double cfoConf = 0.6;
  double maskConf = 0.55;
  // SKW_ADVISOR=off|shadow|assist, SKW_ADVISOR_{AMC,CFO,MASK}_CONF.
  static AdvisorConfig fromEnv();
};

class Advisor {
 public:
  explicit Advisor(AdvisorConfig cfg) : cfg_(cfg) {}

  AdvisorMode mode() const { return cfg_.mode; }
  const AdvisorConfig& config() const { return cfg_; }

  // --- AMC domain: SNR-trend step bias -----------------------------------
  // Feed the effective SNR each evaluation; advice is a single-step bias
  // (+1 leans one evaluation toward promotion, -1 toward demotion).
  struct AmcAdvice {
    int stepBias = 0;         // in {-1, 0, +1}
    double confidence = 0.0;  // [0, 1]
  };
  void onSnr(double effSnrDb, double now);
  AmcAdvice adviseAmc(double retryRatio) const;

  // --- CFO domain: drift prediction ---------------------------------------
  struct CfoAdvice {
    double driftHzPerMin = 0.0;
    double predictedHz = 0.0;
    double confidence = 0.0;
  };
  void onCfo(double cfoHz, double now);
  CfoAdvice adviseCfo(double now) const;

  // --- mask domain: threshold tuning --------------------------------------
  // Feed mask-proposal changes; churn advises a HARDER threshold (fewer,
  // stabler masks). Delta clamped to +-1.5 dB.
  struct MaskAdvice {
    double thresholdDeltaDb = 0.0;
    double confidence = 0.0;
  };
  void onMaskChange(double now);
  MaskAdvice adviseMask(double now) const;

  // Assist gating: true when this domain's advice should be APPLIED.
  bool assistAmc(const AmcAdvice& a) const {
    return cfg_.mode == AdvisorMode::Assist && a.confidence >= cfg_.amcConf;
  }
  bool assistCfo(const CfoAdvice& a) const {
    return cfg_.mode == AdvisorMode::Assist && a.confidence >= cfg_.cfoConf;
  }
  bool assistMask(const MaskAdvice& a) const {
    return cfg_.mode == AdvisorMode::Assist &&
           a.confidence >= cfg_.maskConf;
  }

  void reset();

 private:
  struct Sample {
    double t, v;
  };
  static void push(std::deque<Sample>& q, double t, double v, size_t cap,
                   double horizonSec);
  // Least-squares slope (v per MINUTE) + fit quality over a sample set.
  static bool fit(const std::deque<Sample>& q, double* slopePerMin,
                  double* rr, double* last);

  AdvisorConfig cfg_;
  std::deque<Sample> snr_;
  std::deque<Sample> cfo_;
  std::deque<double> maskChanges_;
};

}  // namespace skw
