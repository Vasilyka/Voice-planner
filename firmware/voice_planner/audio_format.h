#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

constexpr size_t WAV_HEADER_BYTES = 44;

inline void putLe16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

inline void putLe32(uint8_t* destination, uint32_t value) {
  for (int i = 0; i < 4; ++i) destination[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void makeWavHeader(uint8_t* header, uint32_t pcmBytes, uint32_t sampleRate) {
  memcpy(header, "RIFF", 4);
  putLe32(header + 4, 36 + pcmBytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  putLe32(header + 16, 16);
  putLe16(header + 20, 1);  // PCM.
  putLe16(header + 22, 1);  // Mono.
  putLe32(header + 24, sampleRate);
  putLe32(header + 28, sampleRate * 2);
  putLe16(header + 32, 2);
  putLe16(header + 34, 16);
  memcpy(header + 36, "data", 4);
  putLe32(header + 40, pcmBytes);
}

inline int16_t microphoneToPcm16(int32_t raw, int gain, bool& clipped) {
  // 24 бита INMP441 выровнены по старшим битам 32-битного слова.
  // Усиление в 64 битах не переполняет int32_t.
  const int64_t sample = static_cast<int64_t>(raw) * gain / 65536;
  clipped = sample > 32767 || sample < -32768;
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return static_cast<int16_t>(sample);
}
