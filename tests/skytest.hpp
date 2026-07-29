// Minimal test harness (deviation from BUILD_PLAN's vendored GoogleTest,
// logged in ROADMAP.md: avoids a network fetch; same CTest integration).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace skytest {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
inline bool registerCase(const char* name, std::function<void()> fn) {
  cases().push_back({name, std::move(fn)});
  return true;
}

#define TEST_CASE(name)                                             \
  static void skytest_fn_##name();                                  \
  static const bool skytest_reg_##name =                            \
      ::skytest::registerCase(#name, skytest_fn_##name);            \
  static void skytest_fn_##name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ++::skytest::failures();                                            \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                      \
  do {                                                                      \
    if (!((a) == (b))) {                                                    \
      ++::skytest::failures();                                              \
      std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);  \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
  do {                                                                         \
    double va = double(a), vb = double(b);                                     \
    if (std::fabs(va - vb) > (tol)) {                                          \
      ++::skytest::failures();                                                 \
      std::printf("  FAIL %s:%d: |%s - %s| = |%g - %g| > %g\n", __FILE__,      \
                  __LINE__, #a, #b, va, vb, double(tol));                      \
    }                                                                          \
  } while (0)

inline int run() {
  int failedCases = 0;
  for (auto& c : cases()) {
    int before = failures();
    c.fn();
    bool ok = failures() == before;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
    if (!ok) ++failedCases;
  }
  std::printf("%zu case(s), %d failed\n", cases().size(), failedCases);
  return failedCases == 0 ? 0 : 1;
}

}  // namespace skytest

#define SKYTEST_MAIN \
  int main() { return ::skytest::run(); }
