// Environment knobs (CONFIGURATION.md "Environment variables"): one reader
// and one parsing rule for every SKW_* tuning value and debug switch.
//
//   unset or empty            -> the caller's default
//   flag: "0", "off", "false" -> off (any case); any other value -> on
//   number: parsed as a double; text that is not a number -> the default
//
// Each variable is read from the process environment ONCE, on first use,
// and cached (thread-safe): a knob cannot change under a running modem,
// and a hot path pays a map lookup, not a getenv. Hot loops still keep the
// result in a function-local static.
#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace skw {

namespace envdetail {
inline std::optional<std::string> cachedRaw(const char* name) {
  static std::mutex mu;
  static std::map<std::string, std::optional<std::string>> cache;
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(name);
  if (it == cache.end()) {
    const char* e = std::getenv(name);
    std::optional<std::string> v;
    if (e && *e) v = std::string(e);
    it = cache.emplace(name, std::move(v)).first;
  }
  return it->second;
}
}  // namespace envdetail

// On/off switch; `def` when unset.
inline bool envFlag(const char* name, bool def = false) {
  const auto v = envdetail::cachedRaw(name);
  if (!v) return def;
  std::string s;
  for (char c : *v) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  return !(s == "0" || s == "off" || s == "false");
}

// Numeric value; `def` when unset or unparsable.
inline double envDouble(const char* name, double def) {
  const auto v = envdetail::cachedRaw(name);
  if (!v) return def;
  char* end = nullptr;
  const double d = std::strtod(v->c_str(), &end);
  return end == v->c_str() ? def : d;
}

}  // namespace skw
