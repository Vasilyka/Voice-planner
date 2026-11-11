#include <cassert>
#include <cstring>
#include <iostream>
#include "../../firmware/voice_planner/planner_protocol.h"

const char* const ID = "0123456789abcdef0123456789abcdef";
const char* const OTHER = "abcdef0123456789abcdef0123456789";

struct TestStream {
  size_t bytes;
  unsigned timeout = 0;
  bool called = false;
  void setTimeout(unsigned value) { timeout = value; }
  size_t readBytes(uint8_t* data, size_t size) {
    called = true;
    memset(data, 0x5A, bytes < size ? bytes : size);
    return bytes;
  }
};

int main() {
  for (const char* state : {"queued", "processing", "done", "failed", "needs_configuration"})
    assert(validPlannerAck("ok", ID, state));
  assert(!validPlannerAck(nullptr, ID, "queued"));
  assert(!validPlannerAck("ok", nullptr, "queued"));
  assert(!validPlannerAck("ok", ID, nullptr));
  assert(!validPlannerAck("ok", "", "queued"));
  assert(!validPlannerAck("ok", "short", "queued"));
  assert(!validPlannerAck("ok", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "queued"));
  assert(!validPlannerAck("error", ID, "queued"));
  assert(!validPlannerAck("ok", ID, "unknown"));

  uint8_t pixels[PLANNER_FRAME_BYTES] = {};
  assert(!readPlannerFrame<TestStream>(nullptr, pixels));
  TestStream partial{20};
  assert(!readPlannerFrame(&partial, pixels));
  TestStream complete{PLANNER_FRAME_BYTES};
  assert(readPlannerFrame(&complete, pixels));
  assert(complete.called && complete.timeout == 2500);
  assert(pixels[PLANNER_FRAME_BYTES - 1] == 0x5A);

  PlannerEvent event{};
  event.kind = PlannerEventKind::Table;
  assert(acceptPlannerEvent("", event, true));
  assert(!acceptPlannerEvent("", event, false)); // Ошибку нельзя скрыть фоновым кадром.
  strcpy(event.jobId, OTHER);
  assert(!acceptPlannerEvent(ID, event, true));
  assert(!acceptPlannerEvent("", event, true)); // Запоздалый результат уже закрытого задания.
  strcpy(event.jobId, ID);
  assert(acceptPlannerEvent(ID, event, false));
  event.kind = PlannerEventKind::Processing;
  assert(acceptPlannerEvent(ID, event, false)); // Восстановление после STATUS OFFLINE.
  std::cout << "Acknowledgements, disconnected/truncated stream and stale events: OK\n";
}
