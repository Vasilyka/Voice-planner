#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(uint32_t ticks) { delay(ticks); }
inline void vTaskDelete(void*) {}
inline int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int) { return pdPASS; }
