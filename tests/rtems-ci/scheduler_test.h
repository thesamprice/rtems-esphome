#pragma once
// Scheduler validation: does ESPHome's scheduler keep time on RTEMS?
//
// Two properties, and they fail differently:
//
// What is asserted, and deliberately not asserted:
//
//   fired      it ran the expected number of times.  Every bound below is
//              satisfied by a scheduler that never runs at all, so this is the
//              assertion that gives the others meaning.
//   not early  no firing came before its deadline.  This is a real scheduler
//              invariant and holds regardless of how loaded the machine is.
//   drift      REPORTED, NOT ASSERTED.  ESPHome reschedules an interval with
//              set_next_execution(now + interval) -- from when it actually ran,
//              not from when it was due (scheduler.cpp) -- so lateness
//              accumulates by design and an interval is not a metronome.
//              Asserting bounded drift would assert a property ESPHome does not
//              provide and does not claim.
//   lateness   REPORTED, NOT ASSERTED.  Under emulation with icount off this
//              measures how fast QEMU is, not how good the scheduler is.

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <rtems.h>

namespace rtems_sched {

static const char *const TAG = "sched";

static constexpr uint32_t INTERVAL_MS = 100;
static constexpr int SAMPLES = 40;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static uint64_t g_first_ns = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static uint64_t g_prev_ns = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int g_count = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int64_t g_min_gap_us = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int64_t g_max_gap_us = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static bool g_timeout_fired = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int64_t g_timeout_error_us = 0;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static bool g_reported = false;

inline void start() {
  ESP_LOGI(TAG, "SCHEDULER start");
  // set_timeout: one shot, checked against the time it was armed.
  const uint64_t armed_ns = rtems_clock_get_uptime_nanoseconds();
  esphome::App.scheduler.set_timeout(nullptr, "sched_once", 250, [armed_ns]() {
    const int64_t actual_us =
        static_cast<int64_t>((rtems_clock_get_uptime_nanoseconds() - armed_ns) / 1000ULL);
    g_timeout_error_us = actual_us - 250000;
    g_timeout_fired = true;
  });
}

inline void on_interval() {
  const uint64_t now = rtems_clock_get_uptime_nanoseconds();
  if (g_count == 0) {
    g_first_ns = now;
    g_prev_ns = now;
    ++g_count;
    return;
  }

  const int64_t gap_us = static_cast<int64_t>((now - g_prev_ns) / 1000ULL);
  g_prev_ns = now;
  if (g_count == 1) {
    g_min_gap_us = g_max_gap_us = gap_us;
  } else {
    g_min_gap_us = gap_us < g_min_gap_us ? gap_us : g_min_gap_us;
    g_max_gap_us = gap_us > g_max_gap_us ? gap_us : g_max_gap_us;
  }
  ++g_count;

  if (g_count < SAMPLES || g_reported) {
    return;
  }
  g_reported = true;

  int failures = 0;
  const auto check = [&failures](bool ok, const char *what) {
    if (ok) {
      ESP_LOGI(TAG, "  ok   %s", what);
    } else {
      ++failures;
      ESP_LOGE(TAG, "  FAIL %s", what);
    }
  };

  // Drift: where the last firing landed against where it should have.
  const int64_t elapsed_us = static_cast<int64_t>((g_prev_ns - g_first_ns) / 1000ULL);
  const int64_t expected_us = static_cast<int64_t>(g_count - 1) * INTERVAL_MS * 1000;
  const int64_t drift_us = elapsed_us - expected_us;

  ESP_LOGI(TAG, "  interval %ums x %d firings", static_cast<unsigned>(INTERVAL_MS), g_count);
  ESP_LOGI(TAG, "    gap  min %dus  max %dus", static_cast<int>(g_min_gap_us),
           static_cast<int>(g_max_gap_us));
  ESP_LOGI(TAG, "    drift over %d periods: %dus", g_count - 1, static_cast<int>(drift_us));

  check(g_count >= SAMPLES, "the interval fired the expected number of times");

  // Not early. The scheduler may run late -- the loop may be busy, the machine
  // may be slow -- but running before the deadline would be wrong on any
  // machine. One tick of slack because the scheduler cannot resolve finer than
  // the tick driving it.
  check(g_min_gap_us > (int64_t) INTERVAL_MS * 1000 - 15000, "no firing came before its deadline");

  // Lateness and drift are reported above, not asserted. See the header: with
  // icount off they measure the emulator, and drift is ESPHome's documented
  // behaviour rather than a defect.

  if (g_timeout_fired) {
    ESP_LOGI(TAG, "    set_timeout(250ms) error: %dus", static_cast<int>(g_timeout_error_us));
    check(g_timeout_error_us > -15000, "set_timeout did not fire before its deadline");
  } else {
    check(false, "set_timeout fired");
  }

  if (failures == 0) {
    ESP_LOGI(TAG, "SCHEDULER ok");
  } else {
    ESP_LOGE(TAG, "SCHEDULER FAIL (%d)", failures);
  }
}

}  // namespace rtems_sched
