#pragma once
// Self-test for the RTEMS platform primitives, run from on_boot.
//
// These are the things components rely on and that nothing else in the lane
// exercises deliberately: the reference node proves the clock advances, but not
// that delay() waits long enough, that try_lock() fails when it should, or that
// a wake from another task reaches the main loop promptly.
//
// It runs in the target, on the real BSP, rather than as a host unit test,
// because every property here is a property of RTEMS on this board and a host
// test would prove nothing about it.
//
// Prints one line per check.  The harness asserts on "PRIMITIVES ok"; any
// failure prints "PRIMITIVES FAIL" first and the harness's failure signatures
// catch it.

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <rtems.h>

namespace rtems_primitives {

static const char *const TAG = "primtest";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int g_failures = 0;

inline void check(bool ok, const char *what) {
  if (ok) {
    ESP_LOGI(TAG, "  ok   %s", what);
  } else {
    ++g_failures;
    ESP_LOGE(TAG, "  FAIL %s", what);
  }
}

// --- shared state for the helper task ---------------------------------------
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static esphome::Mutex *g_mutex = nullptr;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_helper_holds = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_helper_release = false;

extern "C" inline rtems_task primitives_helper(rtems_task_argument arg) {
  (void) arg;
  g_mutex->lock();
  g_helper_holds = true;
  // Hold until the main task has had its look, then let go.
  while (!g_helper_release) {
    rtems_task_wake_after(1);
  }
  g_mutex->unlock();
  g_helper_holds = false;
  rtems_task_delete(RTEMS_SELF);
}

inline void test_time() {
  ESP_LOGI(TAG, "time:");

  // Monotonic: millis_64() must never go backwards across many reads. This is
  // the property the scheduler depends on and the one a truncating or
  // rollover-tracking clock gets wrong.
  uint64_t prev = esphome::millis_64();
  bool monotonic = true;
  for (int i = 0; i < 2000; ++i) {
    const uint64_t now = esphome::millis_64();
    if (now < prev) {
      monotonic = false;
      break;
    }
    prev = now;
  }
  check(monotonic, "millis_64 is monotonic over 2000 reads");

  // millis() must agree with the low bits of millis_64(); they are two views of
  // one clock and a disagreement means one of them is derived wrongly.
  const uint64_t wide = esphome::millis_64();
  const uint32_t narrow = esphome::millis();
  check(narrow - static_cast<uint32_t>(wide) < 50, "millis agrees with millis_64");

  // delay() must wait at least as long as asked. Waiting slightly longer is
  // fine and expected -- it rounds up to a whole tick -- but waiting less would
  // turn every rate limit built on it into a busy loop.
  const uint64_t before = esphome::millis_64();
  esphome::delay(50);
  const uint64_t elapsed = esphome::millis_64() - before;
  check(elapsed >= 50, "delay(50) waits at least 50ms");
  check(elapsed < 200, "delay(50) does not overshoot wildly");
  ESP_LOGI(TAG, "       delay(50) measured %ums", static_cast<unsigned>(elapsed));

  // micros() must advance across a busy wait far shorter than a tick, which is
  // the resolution claim the counter makes.
  const uint32_t us_before = esphome::micros();
  esphome::delayMicroseconds(1000);
  const uint32_t us_elapsed = esphome::micros() - us_before;
  check(us_elapsed >= 900 && us_elapsed < 20000, "delayMicroseconds(1000) is about 1ms");
  ESP_LOGI(TAG, "       delayMicroseconds(1000) measured %uus", static_cast<unsigned>(us_elapsed));
}

inline void test_mutex() {
  ESP_LOGI(TAG, "mutex:");

  esphome::Mutex m;
  g_mutex = &m;

  // Uncontended: try_lock must succeed on a free mutex.
  check(m.try_lock(), "try_lock succeeds when free");
  m.unlock();

  // Contended: this is the property NASA OSAL cannot express, and the reason
  // docs/architecture.md says not to use it for Mutex. It needs a second task,
  // because a mutex held by the asking task is a different question.
  rtems_id helper = RTEMS_INVALID_ID;
  const rtems_status_code sc =
      rtems_task_create(rtems_build_name('P', 'R', 'M', 'T'), 100, RTEMS_MINIMUM_STACK_SIZE * 4,
                        RTEMS_DEFAULT_MODES, RTEMS_DEFAULT_ATTRIBUTES, &helper);
  check(sc == RTEMS_SUCCESSFUL, "helper task created");
  if (sc != RTEMS_SUCCESSFUL) {
    return;
  }
  g_helper_release = false;
  rtems_task_start(helper, primitives_helper, 0);

  // Wait for the helper to take it.
  for (int i = 0; i < 500 && !g_helper_holds; ++i) {
    esphome::delay(1);
  }
  check(g_helper_holds, "helper acquired the mutex");
  check(!m.try_lock(), "try_lock fails when another task holds it");

  g_helper_release = true;
  for (int i = 0; i < 500 && g_helper_holds; ++i) {
    esphome::delay(1);
  }
  check(!g_helper_holds, "helper released the mutex");
  check(m.try_lock(), "try_lock succeeds again after release");
  m.unlock();
}

inline void test_isr_context() {
  ESP_LOGI(TAG, "isr context:");
  // Task context is not ISR context. The negative is all that can be checked
  // from here; proving the positive needs an actual interrupt handler, which
  // belongs with the wake stress test rather than here.
  check(!esphome::in_isr_context(), "in_isr_context is false in task context");
}

inline void run() {
  ESP_LOGI(TAG, "PRIMITIVES start");
  g_failures = 0;
  test_time();
  test_mutex();
  test_isr_context();
  if (g_failures == 0) {
    ESP_LOGI(TAG, "PRIMITIVES ok");
  } else {
    ESP_LOGE(TAG, "PRIMITIVES FAIL (%d)", g_failures);
  }
}

}  // namespace rtems_primitives
