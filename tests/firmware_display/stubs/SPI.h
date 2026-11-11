#pragma once
#include <vector>
#include "Arduino.h"
constexpr int MSBFIRST = 1, SPI_MODE0 = 0;
struct SPISettings { SPISettings(int, int, int) {} };
struct Transfer { uint8_t command; std::vector<uint8_t> data; };
inline std::vector<Transfer> transfers;
bool testIsData();
struct SPIStub {
  void begin(int, int, int, int) {}
  void beginTransaction(const SPISettings&) {}
  void endTransaction() {}
  uint8_t transfer(uint8_t value) {
    if (testIsData()) {
      transfers.back().data.push_back(value);
    } else {
      transfers.push_back({value, {}});
      if (value == 0x20) busyUntil = testTime + 3;
    }
    return 0;
  }
};
inline SPIStub SPI;
