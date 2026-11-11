#pragma once
using SemaphoreHandle_t = int*;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new int(1); }
inline int xSemaphoreTake(SemaphoreHandle_t, unsigned) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }
