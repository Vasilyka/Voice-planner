#pragma once
#include <stdint.h>

enum class ScreenState : uint8_t {
  Connecting, Ready, Recording, BufferFull, Sending, Saved, Error, Processing, Tasks
};

bool beginStatusDisplay();
void showStatus(ScreenState state, const char* detail, uint32_t limitSeconds);

void notifyRecordingFinished();

void showTaskTable(const uint8_t* pixels, uint32_t limitSeconds);
