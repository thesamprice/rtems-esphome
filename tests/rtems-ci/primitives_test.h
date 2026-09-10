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
#include "esphome/core/wake.h"

#include <rtems.h>
#include <bsp/irq.h>

// The SYSTIMER comparator the clock driver does not use.  TARGET0 is the clock
// tick; TARGET1 and TARGET2 are free, so an interrupt can be raised here
// without disturbing timekeeping.  Offsets from the C3 TRM, confirmed against
// QEMU's model.
#define ST_BASE 0x60023000U
#define ST_REG(off) (*(volatile uint32_t *) (ST_BASE + (off)))
#define ST_CONF 0x00U
#define ST_TARGET1_HI 0x24U
#define ST_TARGET1_LO 0x28U
#define ST_TARGET1_CONF 0x38U
#define ST_UNIT0_OP 0x04U
#define ST_UNIT0_VALUE_LO 0x44U
#define ST_COMP1_LOAD 0x54U
#define ST_INT_ENA 0x64U
#define ST_INT_CLR 0x6cU
#define ST_TARGET1_WORK_EN (1U << 23)
#define ST_INT_TARGET1 (1U << 1)
#define SYSTIMER_TARGET1_IRQ 38  // from the BSP's c3/chip_definitions.h

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
  check(!esphome::in_isr_context(), "in_isr_context is false in task context");
}

// --- ISR-safe wake ----------------------------------------------------------
// The reasoning behind wake_loop_isrsafe() -- that rtems_semaphore_release() is
// callable from an ISR and RTEMS defers dispatch to the outermost interrupt
// exit -- was never executed until this test. Nothing else in the lane raises
// an interrupt that calls it.

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_isr_ran = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile bool g_isr_saw_isr_context = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static volatile uint32_t g_isr_fired_us = 0;

inline void systimer_target1_isr(void *arg) {
  (void) arg;
  ST_REG(ST_INT_CLR) = ST_INT_TARGET1;
  g_isr_saw_isr_context = esphome::in_isr_context();
  g_isr_fired_us = esphome::micros();
  g_isr_ran = true;
  // The call under test.
  esphome::wake_loop_isrsafe();
}

inline void test_isr_wake() {
  ESP_LOGI(TAG, "isr wake:");

  const rtems_status_code sc = rtems_interrupt_handler_install(
      SYSTIMER_TARGET1_IRQ, "prim-t1", RTEMS_INTERRUPT_UNIQUE, systimer_target1_isr, nullptr);
  check(sc == RTEMS_SUCCESSFUL, "installed a handler on SYSTIMER TARGET1");
  if (sc != RTEMS_SUCCESSFUL) {
    return;
  }

  g_isr_ran = false;
  g_isr_saw_isr_context = false;

  // One-shot, 50 ms out: read the counter, add 50 ms of 16 MHz ticks, arm.
  ST_REG(ST_UNIT0_OP) = (1U << 30);
  while ((ST_REG(ST_UNIT0_OP) & (1U << 29)) == 0) {
  }
  const uint32_t now_ticks = ST_REG(ST_UNIT0_VALUE_LO);
  ST_REG(ST_TARGET1_CONF) = 0;  // one-shot: PERIOD_MODE clear, unit0 selected
  ST_REG(ST_TARGET1_HI) = 0;
  ST_REG(ST_TARGET1_LO) = now_ticks + (16000000U / 20U);
  ST_REG(ST_COMP1_LOAD) = 1U;
  ST_REG(ST_CONF) |= ST_TARGET1_WORK_EN;
  ST_REG(ST_INT_ENA) |= ST_INT_TARGET1;

  // Block the way Application::loop() does. If the ISR's wake works this
  // returns early; if it does not, the timeout expires and the elapsed time
  // gives it away.
  const uint32_t before_us = esphome::micros();
  esphome::internal::wakeable_delay(5000);
  const uint32_t waited_us = esphome::micros() - before_us;

  check(g_isr_ran, "the interrupt fired");
  check(g_isr_saw_isr_context, "in_isr_context is TRUE inside the handler");
  check(waited_us < 4000000U, "wakeable_delay returned before its 5s timeout");
  if (g_isr_ran) {
    ESP_LOGI(TAG, "       fired at %uus, loop resumed after %uus",
             static_cast<unsigned>(g_isr_fired_us), static_cast<unsigned>(waited_us));
    const uint32_t latency = esphome::micros() - g_isr_fired_us;
    ESP_LOGI(TAG, "       isr-to-loop latency %uus", static_cast<unsigned>(latency));
  }

  ST_REG(ST_INT_ENA) &= ~ST_INT_TARGET1;
  ST_REG(ST_CONF) &= ~ST_TARGET1_WORK_EN;
  rtems_interrupt_handler_remove(SYSTIMER_TARGET1_IRQ, systimer_target1_isr, nullptr);
}

inline void run() {
  ESP_LOGI(TAG, "PRIMITIVES start");
  g_failures = 0;
  test_time();
  test_mutex();
  test_isr_context();
  test_isr_wake();
  if (g_failures == 0) {
    ESP_LOGI(TAG, "PRIMITIVES ok");
  } else {
    ESP_LOGE(TAG, "PRIMITIVES FAIL (%d)", g_failures);
  }
}

}  // namespace rtems_primitives
