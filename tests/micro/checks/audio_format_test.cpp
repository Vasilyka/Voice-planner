#include "../micro/audio_format.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

int main(int argc, char** argv) {
  bool clipped = false;
  assert(microphoneToPcm16(0, 4, clipped) == 0 && !clipped);
  assert(microphoneToPcm16(65536, 4, clipped) == 4 && !clipped);
  assert(microphoneToPcm16(-65536, 4, clipped) == -4 && !clipped);
  assert(microphoneToPcm16(std::numeric_limits<int32_t>::max(), 4, clipped) == 32767 && clipped);
  assert(microphoneToPcm16(std::numeric_limits<int32_t>::min(), 4, clipped) == -32768 && clipped);
  assert(microphoneToPcm16(std::numeric_limits<int32_t>::min(), 1, clipped) == -32768 && !clipped);
  assert(microphoneToPcm16(std::numeric_limits<int32_t>::max(), 1, clipped) == 32767 && !clipped);
  // Файл создаётся тем же кодом заголовка/PCM, который использует ESP32.
  std::vector<uint8_t> wav(WAV_HEADER_BYTES + 16000 * 2);
  makeWavHeader(wav.data(), 32000, 16000);
  for (size_t i = 0; i < 16000; ++i) {
    const int32_t raw = (i % 32 < 16 ? 1 : -1) * 65536 * 1000;
    const int16_t sample = microphoneToPcm16(raw, 4, clipped);
    putLe16(wav.data() + WAV_HEADER_BYTES + i * 2, static_cast<uint16_t>(sample));
  }
  assert(argc == 2);
  std::ofstream output(argv[1], std::ios::binary);
  output.write(reinterpret_cast<const char*>(wav.data()), wav.size());
  assert(output.good());
  std::cout << "PCM sign, gain, clipping and WAV fixture: OK\n";
}
