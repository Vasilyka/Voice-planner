#pragma once
#include <cstdint>
constexpr int pdTRUE = 1, pdFALSE = 0, pdPASS = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
inline uint32_t pdMS_TO_TICKS(uint32_t value) { return value; }
