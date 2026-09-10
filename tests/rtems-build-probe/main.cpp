// Shaped like a generated ESPHome image: C++, a class with virtuals, the
// standard library, and a main loop that ticks.  If this links and runs, the
// build backend for #4 is a matter of flags rather than of feasibility.
#include <rtems.h>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>

namespace esphome_probe {

class Component {
 public:
  virtual ~Component() = default;
  virtual const char *name() const = 0;
  virtual void loop() = 0;
};

class Counter : public Component {
 public:
  const char *name() const override { return "counter"; }
  void loop() override { ++count_; }
  int count() const { return count_; }
 private:
  int count_{0};
};

}  // namespace esphome_probe

extern "C" rtems_task Init(rtems_task_argument)
{
  std::vector<std::unique_ptr<esphome_probe::Component>> components;
  components.push_back(std::make_unique<esphome_probe::Counter>());

  std::string banner = std::string("PROBE-MARKER c++ ok, ") +
                       std::to_string(components.size()) + " component";
  printf("%s\n", banner.c_str());

  for (int i = 0; i < 5; ++i) {
    for (auto &c : components) c->loop();
    rtems_task_wake_after(rtems_clock_get_ticks_per_second() / 10);
  }
  auto *counter = static_cast<esphome_probe::Counter *>(components[0].get());
  printf("PROBE-MARKER loop ran %d times\n", counter->count());
  printf("PROBE-MARKER done\n");
  exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER
#define CONFIGURE_MAXIMUM_TASKS 4
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_STACK_SIZE (16 * 1024)
#define CONFIGURE_INIT
#include <rtems/confdefs.h>
