#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "board_pins.h"
#include "status_display.h"

namespace {
constexpr int WIDTH = 184;
constexpr int HEIGHT = 384;
constexpr bool ROTATE_180 = true;
constexpr bool MIRROR_X = true;
constexpr bool MIRROR_Y = false;
constexpr size_t BUFFER_SIZE = WIDTH * HEIGHT / 8;
constexpr uint32_t BUSY_TIMEOUT_MS = 30000;
const SPISettings EPD_SPI(4000000, MSBFIRST, SPI_MODE0);
uint8_t image[BUFFER_SIZE];
uint8_t previousImage[BUFFER_SIZE];
uint8_t tableImage[BUFFER_SIZE];
SemaphoreHandle_t tableMutex = nullptr;
constexpr uint32_t PARTIAL_GAP_MS = 300;
constexpr unsigned MAX_PARTIAL_UPDATES = 5;
uint32_t lastRefreshEnd = 0;

struct StatusFrame {
  ScreenState state;
  char detail[24];
  uint32_t limitSeconds;
  uint32_t completedRecordings;
};
QueueHandle_t statusQueue = nullptr;
// Изменяется только из основного цикла, вместе с отправкой StatusFrame.
uint32_t completedRecordings = 0;

bool isRecordingState(ScreenState state) {
  return state == ScreenState::Recording || state == ScreenState::BufferFull;
}

enum class RefreshMode { Skip, Partial, Full };

RefreshMode chooseRefresh(const StatusFrame& status, bool initialized,
                          uint32_t lastFullRecording, unsigned partialUpdates) {
  if (!initialized)
    return isRecordingState(status.state) ? RefreshMode::Skip : RefreshMode::Full;
  if (status.completedRecordings != lastFullRecording &&
      !isRecordingState(status.state) && status.state != ScreenState::Sending && status.state != ScreenState::Processing)
    return RefreshMode::Full;
  // Лимит из примера Good Display: следующая очистка — после записи.
  return partialUpdates < MAX_PARTIAL_UPDATES ? RefreshMode::Partial : RefreshMode::Skip;
}

// Шрифт 5 x 7. Каждый байт - столбец, младший бит - верхний пиксель.
const char FONT_CHARS[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-.";
const uint8_t FONT[][5] = {
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
  {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
  {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
  {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
  {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
  {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
  {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
  {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
  {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
  {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
  {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
  {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
  {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
  {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
  {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
  {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
  {0x00,0x36,0x36,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
  {0x00,0x60,0x60,0x00,0x00}
};
static_assert(sizeof(FONT) / sizeof(FONT[0]) == sizeof(FONT_CHARS) - 1,
              "Font table mismatch");

void command(uint8_t value) {
  SPI.beginTransaction(EPD_SPI);
  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  SPI.transfer(value);
  digitalWrite(EPD_CS, HIGH);
  SPI.endTransaction();
}

void dataByte(uint8_t value) {
  SPI.beginTransaction(EPD_SPI);
  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  SPI.transfer(value);
  digitalWrite(EPD_CS, HIGH);
  SPI.endTransaction();
}

bool waitReady(const char *stage) {
  const uint32_t started = millis();
  while (digitalRead(EPD_BUSY) == HIGH) {
    if (millis() - started >= BUSY_TIMEOUT_MS) {
      Serial.printf("ERROR: BUSY stayed HIGH at %s for 30 s.\n", stage);
      return false;
    }
    delay(1);
  }
  return true;
}

bool initDisplay() {
  digitalWrite(EPD_RST, LOW);
  delay(10);
  digitalWrite(EPD_RST, HIGH);
  delay(10);
  if (!waitReady("hardware reset")) return false;
  command(0x12); // SWRESET.
  delay(2);
  if (!waitReady("software reset")) return false;

  command(0x01); // Driver output control: 384 строки.
  dataByte((HEIGHT - 1) & 0xFF);
  dataByte((HEIGHT - 1) >> 8);
  dataByte(0x00);
  command(0x11); dataByte(0x01); // X растёт, Y уменьшается.
  command(0x44); dataByte(0x00); dataByte(WIDTH / 8 - 1);
  command(0x45);
  dataByte((HEIGHT - 1) & 0xFF); dataByte((HEIGHT - 1) >> 8);
  dataByte(0x00); dataByte(0x00);
  command(0x3C); dataByte(0x01); // Border waveform.
  command(0x18); dataByte(0x80); // Встроенный датчик температуры.
  command(0x4E); dataByte(0x00);
  command(0x4F);
  dataByte((HEIGHT - 1) & 0xFF); dataByte((HEIGHT - 1) >> 8);
  return waitReady("initialization");
}

void writeFrame(uint8_t ramCommand, const uint8_t* pixels) {
  command(ramCommand);
  SPI.beginTransaction(EPD_SPI);
  digitalWrite(EPD_DC, HIGH);
  digitalWrite(EPD_CS, LOW);
  for (size_t i = 0; i < BUFFER_SIZE; ++i) {
    SPI.transfer(pixels ? pixels[i] : 0x00);
  }
  digitalWrite(EPD_CS, HIGH);
  SPI.endTransaction();
}

bool refreshDisplay(bool full) {
  const uint32_t started = millis();
  Serial.println(full ? "EPD: FULL refresh" : "EPD: PARTIAL refresh");
  if (!initDisplay()) return false;
  if (!full) {
    command(0x3C); dataByte(0x84); // Border: partial mode из примера Good Display.
    command(0x21); dataByte(0x00); dataByte(0x40);
  }
  writeFrame(0x24, image);
  // После deep sleep восстанавливаем старый кадр для дифференциального обновления.
  writeFrame(0x26, full ? nullptr : previousImage);
  command(0x22); dataByte(full ? 0xF4 : 0xFF);
  command(0x20);

  // BUSY должен подняться. Иначе обрыв BUSY/SPI может выглядеть как успех.
  const uint32_t activation = millis();
  while (digitalRead(EPD_BUSY) == LOW) {
    if (millis() - activation >= 1000) {
      Serial.println("ERROR: no BUSY HIGH after refresh command. Check wiring/power/FPC.");
      return false;
    }
    delay(1);
  }
  Serial.println("EPD: BUSY HIGH - refreshing");
  if (!waitReady(full ? "full refresh" : "partial refresh")) return false;
  Serial.println("EPD: BUSY LOW - controller ready");

  command(0x10); dataByte(0x01); // Deep sleep дисплея, ESP32 продолжает опрос кнопки.
  delay(100);
  lastRefreshEnd = millis();
  Serial.printf("EPD: controller cycle complete, %lu ms; panel asleep. Check image visually.\n",
                static_cast<unsigned long>(millis() - started));
  return true;
}

void pixel(int x, int y, bool black = true) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
  if (ROTATE_180) {
    x = WIDTH - 1 - x;
    y = HEIGHT - 1 - y;
  }
  if (MIRROR_X) x = WIDTH - 1 - x;
  if (MIRROR_Y) y = HEIGHT - 1 - y;
  const size_t index = y * (WIDTH / 8) + x / 8;
  const uint8_t mask = 0x80 >> (x & 7);
  if (black) image[index] &= static_cast<uint8_t>(~mask);
  else image[index] |= mask;
}

void fillRect(int x, int y, int width, int height, bool black = true) {
  for (int row = y; row < y + height; ++row)
    for (int col = x; col < x + width; ++col) pixel(col, row, black);
}

void frame(int x, int y, int width, int height) {
  fillRect(x, y, width, 2);
  fillRect(x, y + height - 2, width, 2);
  fillRect(x, y, 2, height);
  fillRect(x + width - 2, y, 2, height);
}

void textAt(int x, int y, const char *text, int scale = 1) {
  for (; *text; ++text, x += 6 * scale) {
    const char *glyph = strchr(FONT_CHARS, *text);
    if (glyph == nullptr) continue;
    const size_t index = glyph - FONT_CHARS;
    for (int col = 0; col < 5; ++col)
      for (int row = 0; row < 7; ++row)
        if (FONT[index][col] & (1 << row))
          fillRect(x + col * scale, y + row * scale, scale, scale);
  }
}

void centeredText(int y, const char *text, int scale = 1) {
  const int width = static_cast<int>(strlen(text)) * 6 * scale - scale;
  textAt((WIDTH - width) / 2, y, text, scale);
}


void drawStatus(const StatusFrame& status) {
  static const char* const titles[] = {
    "CONNECTING", "READY", "RECORDING", "BUFFER FULL",
    "SENDING", "SAVED", "ERROR", "PROCESSING", "TASKS"
  };
  memset(image, 0xFF, sizeof(image));
  if (status.state == ScreenState::Tasks) {
    xSemaphoreTake(tableMutex, portMAX_DELAY);
    for (int y = 0; y < HEIGHT; ++y)
      for (int x = 0; x < WIDTH; ++x)
        if (!(tableImage[y * (WIDTH / 8) + x / 8] & (0x80 >> (x & 7))))
          pixel(x, y);
    xSemaphoreGive(tableMutex);
    return;
  }
  frame(0, 0, WIDTH, HEIGHT);
  centeredText(22, "VOICE", 3);
  centeredText(52, "PLANNER", 3);
  fillRect(12, 90, WIDTH - 24, 2);
  centeredText(120, titles[static_cast<unsigned>(status.state)], 2);
  centeredText(170, status.detail);
  if (status.limitSeconds > 0) {
    char limit[24];
    snprintf(limit, sizeof(limit), "LIMIT %lu SEC",
             static_cast<unsigned long>(status.limitSeconds));
    centeredText(215, limit);
  }
  if (status.state == ScreenState::Recording || status.state == ScreenState::BufferFull)
    centeredText(285, "RELEASE TO SEND");
  else if (status.state == ScreenState::Error)
    centeredText(285, "CHECK SERIAL");
  else
    centeredText(285, "HOLD TO RECORD");
  centeredText(350, "16000 HZ MONO");
}

void displayTask(void*) {
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  StatusFrame status;
  bool initialized = false;
  unsigned partialUpdates = 0;
  uint32_t lastFullRecording = 0;
  while (true) {
    xQueueReceive(statusQueue, &status, portMAX_DELAY);
    if (initialized) {
      const uint32_t elapsed = millis() - lastRefreshEnd;
      if (elapsed < PARTIAL_GAP_MS)
        vTaskDelay(pdMS_TO_TICKS(PARTIAL_GAP_MS - elapsed));
    }
    while (xQueueReceive(statusQueue, &status, 0) == pdTRUE) {}
    const RefreshMode mode = chooseRefresh(status, initialized, lastFullRecording, partialUpdates);
    if (mode == RefreshMode::Skip) continue;
    drawStatus(status);
    if (mode == RefreshMode::Partial && memcmp(image, previousImage, BUFFER_SIZE) == 0)
      continue;
    const bool full = mode == RefreshMode::Full;
    if (!refreshDisplay(full)) {
      digitalWrite(EPD_CS, HIGH);
      digitalWrite(EPD_RST, LOW);
      Serial.println("DISPLAY ERROR: disabled until reboot; audio remains available.");
      vTaskDelete(nullptr);
    }
    memcpy(previousImage, image, BUFFER_SIZE);
    initialized = true;
    if (full) {
      partialUpdates = 0;
      lastFullRecording = status.completedRecordings;
    } else {
      ++partialUpdates;
    }
  }
}
}  // namespace

bool beginStatusDisplay() {
  digitalWrite(EPD_CS, HIGH); pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_RST, LOW); pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_DC, LOW); pinMode(EPD_DC, OUTPUT);
  pinMode(EPD_BUSY, INPUT);
  tableMutex = xSemaphoreCreateMutex();
  if (!tableMutex) return false;
  statusQueue = xQueueCreate(1, sizeof(StatusFrame));
  if (statusQueue == nullptr) { vSemaphoreDelete(tableMutex); return false; }
  if (xTaskCreatePinnedToCore(displayTask, "epaper", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    vQueueDelete(statusQueue);
    vSemaphoreDelete(tableMutex);
    statusQueue = nullptr;
    return false;
  }
  return true;
}

void showStatus(ScreenState state, const char* detail, uint32_t limitSeconds) {
  if (statusQueue == nullptr) return;
  StatusFrame frame = {};
  frame.state = state;
  frame.limitSeconds = limitSeconds;
  frame.completedRecordings = completedRecordings;
  snprintf(frame.detail, sizeof(frame.detail), "%s", detail);
  xQueueOverwrite(statusQueue, &frame);
}

void notifyRecordingFinished() {
  ++completedRecordings;
}

void showTaskTable(const uint8_t* pixels, uint32_t limitSeconds) {
  if (!statusQueue) return;
  xSemaphoreTake(tableMutex, portMAX_DELAY);
  memcpy(tableImage, pixels, BUFFER_SIZE);
  xSemaphoreGive(tableMutex);
  showStatus(ScreenState::Tasks, "", limitSeconds);
}
