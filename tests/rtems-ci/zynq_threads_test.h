#pragma once
// Cross-task use of ESPHome's core helpers on RTEMS (#34).
//
// #34 was opened because an audit counted 27 core blockers across six
// task/queue files. Most of that turned out to be answered by the thread model
// rather than by porting: the RTEMS platform selects ESPHOME_THREAD_MULTI_ATOMICS,
// under which freertos_queue.h, static_task.* and main_task.* all compile out.
// See the issue for the full accounting.
//
// What that leaves is the part the audit could not see, and it is the part
// that matters: this port genuinely has other tasks -- lwIP runs its own tcpip
// thread -- so the helpers that DO compile have to be correct when a second
// task uses them. That is what this checks, and every check here runs against
// a real second RTEMS task rather than a simulated one.
//
// The RTEMS side is deliberately written the way a component would write it,
// because #34 asks whether a task-creation helper is needed. rtems_task_create
// plus rtems_task_start takes a per-task argument directly, which is the gap
// that made OSAL unsuitable -- so the answer is that the classic API is already
// the helper, and the two tasks below are the evidence.

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/lock_free_queue.h"
#include "esphome/core/log.h"

#include <rtems.h>

namespace rtems_threads {

static const char *const TAG = "threads";

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static int failures = 0;

static void check(const char *what, bool ok) {
  ESP_LOGI(TAG, "%-52s %s", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++failures;
  }
}

static rtems_id g_main_task = 0;
static rtems_id g_done_sem = 0;

// --- 1. two tasks, one entry point, different arguments ------------------
// #34's exit criterion, made concrete: OSAL's task API cannot pass a void *
// instance argument, which is why static_task could not be built on it.
static rtems_task_argument g_seen_arg[2] = {0, 0};
static rtems_id g_seen_self[2] = {0, 0};

static void arg_task(rtems_task_argument arg) {
  const unsigned slot = static_cast<unsigned>(arg) - 1;
  g_seen_arg[slot] = arg;
  g_seen_self[slot] = rtems_task_self();
  rtems_semaphore_release(g_done_sem);
  rtems_task_exit();
}

// --- 2. a queue with a producer on another task ---------------------------
static constexpr int ITEMS = 200;
static uint32_t g_items[ITEMS];
static esphome::LockFreeQueue<uint32_t, 8> g_queue;
static bool g_producer_done = false;

static void producer_task(rtems_task_argument arg) {
  (void) arg;
  for (int i = 0; i < ITEMS; i++) {
    // Retry rather than drop: a full queue is the consumer being behind, which
    // is the ordinary case for a ring this small and 200 items.
    while (!g_queue.push(&g_items[i])) {
      rtems_task_wake_after(1);
    }
  }
  g_producer_done = true;
  rtems_semaphore_release(g_done_sem);
  rtems_task_exit();
}

// --- 3. the scheduler, called from another task ---------------------------
// ESPHome's contract under MULTI_ATOMICS is that the scheduler is safe to call
// from any thread, but that it does NOT wake the loop by itself: a background
// producer calls App.wake_loop_threadsafe(). Both halves are checked, because
// each fails silently -- an unsafe scheduler corrupts rarely, and a missing
// wake only shows up as latency.
static volatile bool g_deferred_ran[2] = {false, false};
static rtems_id g_deferred_on[2] = {0, 0};
static uint64_t g_defer_sent_ns[2] = {0, 0};
static uint64_t g_defer_ran_ns[2] = {0, 0};
static volatile bool g_deferrer_done = false;

static void deferrer_task(rtems_task_argument arg) {
  (void) arg;

  for (int round = 0; round < 2; round++) {
    // Let the main loop reach its sleep, so the measurement is of a loop that
    // has to be woken rather than one that was about to run anyway.
    rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 4);

    g_defer_sent_ns[round] = rtems_clock_get_uptime_nanoseconds();
    esphome::App.scheduler.set_timeout(nullptr, "from_task", 0, [round]() {
      g_deferred_ran[round] = true;
      g_deferred_on[round] = rtems_task_self();
      g_defer_ran_ns[round] = rtems_clock_get_uptime_nanoseconds();
    });

    // Round 0 does not wake: the loop sleeps out its interval first.
    // Round 1 does, which is what a component pushing work from a task must do.
    if (round == 1) {
      esphome::App.wake_loop_threadsafe();
    }
  }

  g_deferrer_done = true;
  rtems_task_exit();
}
// --- 4. a task to contend for the mutex -----------------------------------
static esphome::Mutex g_mutex;
static rtems_id g_hold_taken = 0;
static rtems_id g_hold_release = 0;
static rtems_id g_hold_done = 0;

static void holder_task(rtems_task_argument arg) {
  (void) arg;
  g_mutex.lock();
  rtems_semaphore_release(g_hold_taken);
  rtems_semaphore_obtain(g_hold_release, RTEMS_WAIT, RTEMS_NO_TIMEOUT);
  g_mutex.unlock();
  rtems_semaphore_release(g_hold_done);
  rtems_task_exit();
}
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

static rtems_id spawn(const char *name, rtems_task_entry entry, rtems_task_argument arg) {
  rtems_task_priority prio = 0;
  rtems_task_set_priority(RTEMS_SELF, RTEMS_CURRENT_PRIORITY, &prio);

  rtems_id id = RTEMS_INVALID_ID;
  rtems_status_code sc = rtems_task_create(rtems_build_name(name[0], name[1], name[2], name[3]), prio,
                                           RTEMS_MINIMUM_STACK_SIZE * 4, RTEMS_DEFAULT_MODES,
                                           RTEMS_FLOATING_POINT, &id);
  if (sc != RTEMS_SUCCESSFUL) {
    ESP_LOGE(TAG, "rtems_task_create: %s", rtems_status_text(sc));
    return RTEMS_INVALID_ID;
  }
  sc = rtems_task_start(id, entry, arg);
  if (sc != RTEMS_SUCCESSFUL) {
    ESP_LOGE(TAG, "rtems_task_start: %s", rtems_status_text(sc));
    return RTEMS_INVALID_ID;
  }
  return id;
}

inline void start() {
  ESP_LOGI(TAG, "--- cross-task test ---");

  g_main_task = rtems_task_self();
  rtems_semaphore_create(rtems_build_name('T', 'D', 'N', 'E'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY, 0,
                         &g_done_sem);
  rtems_semaphore_create(rtems_build_name('H', 'T', 'K', 'N'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY, 0,
                         &g_hold_taken);
  rtems_semaphore_create(rtems_build_name('H', 'R', 'E', 'L'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY, 0,
                         &g_hold_release);
  rtems_semaphore_create(rtems_build_name('H', 'D', 'N', 'E'), 0, RTEMS_COUNTING_SEMAPHORE | RTEMS_PRIORITY, 0,
                         &g_hold_done);

  // Raise the loop interval so the wake test below measures something
  // unambiguous rather than racing a 16 ms loop.
  esphome::App.set_loop_interval(500);

  // --- 1. two tasks, one entry point, different arguments ----------------
  {
    const rtems_id a = spawn("ARG1", arg_task, 1);
    const rtems_id b = spawn("ARG2", arg_task, 2);
    check("two tasks start from one entry point", a != RTEMS_INVALID_ID && b != RTEMS_INVALID_ID);

    bool joined = true;
    for (int i = 0; i < 2; i++) {
      joined = joined && rtems_semaphore_obtain(g_done_sem, RTEMS_WAIT,
                                                rtems_clock_get_ticks_per_second()) == RTEMS_SUCCESSFUL;
    }
    check("both finish", joined);
    check("each sees its own argument", g_seen_arg[0] == 1 && g_seen_arg[1] == 2);
    check("and they are genuinely different tasks",
          g_seen_self[0] != g_seen_self[1] && g_seen_self[0] != g_main_task && g_seen_self[1] != g_main_task);
  }

  // --- 2. the queue, full and empty, with no second task -----------------
  // Deterministic, so the capacity claim does not depend on scheduling. A ring
  // of SIZE holds SIZE-1: the full test is tail+1 == head, not tail == head,
  // which is how it tells full from empty.
  {
    esphome::LockFreeQueue<uint32_t, 4> q;
    uint32_t v[4] = {1, 2, 3, 4};
    check("an empty queue pops nothing", q.pop() == nullptr);
    check("a queue of 4 accepts 3", q.push(&v[0]) && q.push(&v[1]) && q.push(&v[2]));
    check("and refuses the fourth", !q.push(&v[3]));
    check("the refusal is counted as a drop", q.get_and_reset_dropped_count() == 1);
    check("popping one makes room again", q.pop() == &v[0] && q.push(&v[3]));
  }

  // --- 3. the queue, with a producer on another task ---------------------
  {
    for (int i = 0; i < ITEMS; i++) {
      g_items[i] = static_cast<uint32_t>(i);
    }

    const rtems_id p = spawn("PROD", producer_task, 0);
    check("a producer task starts", p != RTEMS_INVALID_ID);

    int received = 0;
    bool in_order = true;
    const uint64_t deadline_ns = rtems_clock_get_uptime_nanoseconds() + 10ULL * 1000000000ULL;

    while (received < ITEMS && rtems_clock_get_uptime_nanoseconds() < deadline_ns) {
      uint32_t *item = g_queue.pop();
      if (item == nullptr) {
        rtems_task_wake_after(1);
        continue;
      }
      if (*item != static_cast<uint32_t>(received)) {
        in_order = false;
      }
      ++received;
    }

    check("every item crosses the task boundary", received == ITEMS);
    check("in the order the producer pushed them", in_order);

    // Drops are expected and are the point: the producer retries when the ring
    // fills, so a non-zero count is evidence that the full path was really
    // crossed under concurrency rather than the consumer happening to keep up.
    // Nothing is lost, because a refused push is retried -- "dropped" here
    // counts refusals, not lost items, and 200 items through a ring of 8 has
    // to hit it.
    const uint16_t refused = g_queue.get_and_reset_dropped_count();
    ESP_LOGI(TAG, "     pushes refused while the ring was full: %u", static_cast<unsigned>(refused));
    check("the ring really filled during the run", refused > 0);

    rtems_semaphore_obtain(g_done_sem, RTEMS_WAIT, rtems_clock_get_ticks_per_second());
    check("the producer finished", g_producer_done);
  }

  // --- 4. the mutex, contended across tasks ------------------------------
  // try_lock() is the reason this platform does not use NASA OSAL, whose mutex
  // API has no non-blocking form. The contention has to come from another
  // task: RTEMS binary semaphores with RTEMS_INHERIT_PRIORITY permit nested
  // access by their owner, so try_lock() from the holding task SUCCEEDS here
  // and a same-task check would prove nothing. See #71.
  {
    const rtems_id h = spawn("HOLD", holder_task, 0);
    check("a holder task starts", h != RTEMS_INVALID_ID);

    // Wait until it actually holds the mutex.
    check("the holder has taken it",
          rtems_semaphore_obtain(g_hold_taken, RTEMS_WAIT, rtems_clock_get_ticks_per_second()) ==
              RTEMS_SUCCESSFUL);
    check("try_lock fails while another task holds it", !g_mutex.try_lock());

    rtems_semaphore_release(g_hold_release);
    check("the holder has released it",
          rtems_semaphore_obtain(g_hold_done, RTEMS_WAIT, rtems_clock_get_ticks_per_second()) ==
              RTEMS_SUCCESSFUL);
    check("and try_lock succeeds once it is free", g_mutex.try_lock());

    // Recorded rather than worked around: the same call from the owning task
    // succeeds, which is what a recursive mutex does and what FreeRTOS's
    // xSemaphoreCreateMutex does not. #71.
    const bool recursive = g_mutex.try_lock();
    ESP_LOGI(TAG, "     re-taking from the owning task: %s", recursive ? "succeeds (recursive)" : "fails");
    if (recursive) {
      g_mutex.unlock();
    }
    g_mutex.unlock();
  }

  // --- 5. arm the scheduler test, which needs the loop turning -----------
  spawn("DEFR", deferrer_task, 0);
  ESP_LOGI(TAG, "waiting for the main loop to run the deferred work");
}

// Called from an interval, so the main loop is really running.
inline void poll() {
  static bool reported = false;
  if (reported || !g_deferrer_done || !g_deferred_ran[0] || !g_deferred_ran[1]) {
    return;
  }
  reported = true;

  check("work deferred from another task runs", g_deferred_ran[0] && g_deferred_ran[1]);
  check("on the main loop task, not the caller's",
        g_deferred_on[0] == g_main_task && g_deferred_on[1] == g_main_task);

  const uint64_t no_wake_us = (g_defer_ran_ns[0] - g_defer_sent_ns[0]) / 1000ULL;
  const uint64_t wake_us = (g_defer_ran_ns[1] - g_defer_sent_ns[1]) / 1000ULL;
  ESP_LOGI(TAG, "     deferred without a wake: %uus", static_cast<unsigned>(no_wake_us));
  ESP_LOGI(TAG, "     deferred with a wake:    %uus", static_cast<unsigned>(wake_us));

  // What the no-wake case waits for is the loop's own cadence, and that is set
  // by the 100 ms poll interval in the YAML rather than by loop_interval_ --
  // an armed interval wakes the loop long before a 500 ms sleep expires. So
  // the bound is tens of milliseconds, not hundreds, and the claim worth
  // asserting is the gap between the two rather than either number: a wake is
  // what makes deferred work prompt, and without one it waits.
  ESP_LOGI(TAG, "     ratio: %ux", static_cast<unsigned>(no_wake_us / (wake_us == 0 ? 1 : wake_us)));
  check("without a wake it waits for the loop", no_wake_us > 10000);
  check("a wake gets it run promptly", wake_us < 5000);
  check("and the wake is the thing that made the difference", no_wake_us > 10 * wake_us);

  ESP_LOGI(TAG, "%d failure(s)", failures);
  if (failures == 0) {
    ESP_LOGI(TAG, "CI-MARKER threads ok");
  }
}

}  // namespace rtems_threads
