#include "core/log.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace skw::log {
namespace {
std::atomic<int> g_level{int(LogLevel::Info)};
std::mutex g_mutex;
FILE* g_file = nullptr;

const char* tag(LogLevel lvl) {
  switch (lvl) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}
}  // namespace

void setLevel(LogLevel lvl) { g_level.store(int(lvl), std::memory_order_relaxed); }

bool setFile(const std::string& path) {
  std::lock_guard lock(g_mutex);
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
  if (path.empty()) return true;
  g_file = fopen(path.c_str(), "a");
  return g_file != nullptr;
}

void write(LogLevel lvl, const char* fmt, ...) {
  if (int(lvl) < g_level.load(std::memory_order_relaxed)) return;

  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm tmv{};
  localtime_r(&ts.tv_sec, &tmv);
  char stamp[32];
  snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min,
           tmv.tm_sec, int(ts.tv_nsec / 1000000));

  std::lock_guard lock(g_mutex);
  fprintf(stderr, "%s %s %s\n", stamp, tag(lvl), msg);
  if (g_file) {
    fprintf(g_file, "%s %s %s\n", stamp, tag(lvl), msg);
    fflush(g_file);
  }
}

}  // namespace skw::log
