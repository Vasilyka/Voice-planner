#pragma once
#include <cstring>
#include <vector>
#include "FreeRTOS.h"
struct TestQueue { std::vector<uint8_t> data; bool pending = false; };
using QueueHandle_t = TestQueue*;
inline QueueHandle_t xQueueCreate(int, size_t size) { return new TestQueue{std::vector<uint8_t>(size), false}; }
inline void vQueueDelete(QueueHandle_t queue) { delete queue; }
inline int xQueueOverwrite(QueueHandle_t queue, const void* frame) {
  memcpy(queue->data.data(), frame, queue->data.size());
  queue->pending = true;
  return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void* frame, uint32_t) {
  if (!queue->pending) return pdFALSE;
  memcpy(frame, queue->data.data(), queue->data.size());
  queue->pending = false;
  return pdTRUE;
}
