#pragma once
#include <stdint.h>

enum class ScreenState : uint8_t {
  Connecting, Ready, Recording, BufferFull, Sending, Saved, Error
};

bool beginStatusDisplay();
void showStatus(ScreenState state, const char* detail, uint32_t limitSeconds);
