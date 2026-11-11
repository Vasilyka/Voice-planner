#pragma once
#include <stddef.h>
#include <string.h>
#include "planner_client.h"

enum class PlannerJobState { Invalid, Queued, Processing, Done, Failed, NeedsConfiguration };

inline bool validPlannerJobId(const char* id) {
  if (!id || strlen(id) != 32) return false;
  for (const char* p = id; *p; ++p)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
  return true;
}

inline PlannerJobState plannerJobState(const char* state) {
  if (!state) return PlannerJobState::Invalid;
  if (!strcmp(state, "queued")) return PlannerJobState::Queued;
  if (!strcmp(state, "processing")) return PlannerJobState::Processing;
  if (!strcmp(state, "done")) return PlannerJobState::Done;
  if (!strcmp(state, "failed")) return PlannerJobState::Failed;
  if (!strcmp(state, "needs_configuration")) return PlannerJobState::NeedsConfiguration;
  return PlannerJobState::Invalid;
}

inline bool validPlannerAck(const char* status, const char* id, const char* state) {
  return status && !strcmp(status, "ok") && validPlannerJobId(id) &&
         plannerJobState(state) != PlannerJobState::Invalid;
}

inline bool acceptPlannerEvent(const char* activeJob, const PlannerEvent& event, bool idleTableAllowed) {
  if (activeJob && *activeJob) return !strcmp(activeJob, event.jobId);
  return !event.jobId[0] && event.kind == PlannerEventKind::Table && idleTableAllowed;
}

template<class Stream>
bool readPlannerFrame(Stream* stream, uint8_t* pixels) {
  if (!stream || !pixels) return false;
  stream->setTimeout(2500);
  return stream->readBytes(pixels, PLANNER_FRAME_BYTES) == PLANNER_FRAME_BYTES;
}
