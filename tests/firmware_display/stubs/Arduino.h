#pragma once
#include <cstdint>
#include <cstdio>
constexpr int LOW = 0, HIGH = 1, INPUT = 0, OUTPUT = 1;
inline uint32_t testTime = 0;
inline uint32_t busyUntil = 0;
inline int testPins[64] = {};
inline uint32_t millis() { return testTime; }
inline void delay(uint32_t ms) { testTime += ms; }
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int value) { testPins[pin] = value; }
inline int digitalRead(int) { return testTime < busyUntil ? HIGH : LOW; }
struct SerialStub {
  void println(const char*) {}
  template<class... Args> void printf(const char*, Args...) {}
};
inline SerialStub Serial;
