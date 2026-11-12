#pragma once
#include <stdint.h>

constexpr unsigned PLANNER_FRAME_BYTES = 184 * 384 / 8;
enum class PlannerEventKind { Table, Processing, Error, JobFailed };
struct PlannerEvent {
  PlannerEventKind kind;
  char jobId[33];
  char detail[24];
  uint8_t pixels[PLANNER_FRAME_BYTES];
};

bool beginPlannerClient(const char* voiceUrl);
bool trackPlannerJob(const char* jobId);
void acknowledgePlannerJob(const char* jobId);
void nextPlannerPage();
bool receivePlannerEvent(PlannerEvent& event);
