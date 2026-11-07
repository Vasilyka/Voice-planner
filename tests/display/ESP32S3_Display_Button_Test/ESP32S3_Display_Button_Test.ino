/*
  GDEM035T81 (SSD1685, 184 x 384) + DESPI-C02 + ESP32-S3.
  Отдельный тест, без сторонних библиотек. Нужен пакет esp32 by Espressif.

  DESPI-C02: BUSY=7, RES=8, D/C=9, CS=10, SDI=11, SCK=12.
  Питание DESPI: 3.3V -> 3V3, GND -> GND. P3/RESE: 2.2 Ом.
  Отдельная нормально разомкнутая кнопка: GPIO17 -- кнопка -- GND.

  При старте: белый экран, чёрный экран, тестовая страница.
  Кнопка увеличивает счётчик и меняет рисунок. Только полное обновление.
  Последовательность SSD1685 взята из приложенного примера Good Display.
*/
#include <Arduino.h>
#include <SPI.h>
#include <string.h>

constexpr uint8_t PIN_BUSY = 7;
constexpr uint8_t PIN_RST = 8;
constexpr uint8_t PIN_DC = 9;
constexpr uint8_t PIN_CS = 10;
constexpr uint8_t PIN_MOSI = 11;
constexpr uint8_t PIN_SCK = 12;
constexpr uint8_t PIN_BUTTON = 17;

constexpr int WIDTH = 184;
constexpr int HEIGHT = 384;
// Компенсация зеркальности: отражаем координату до упаковки пикселя в RAM.
constexpr bool MIRROR_X = true;
constexpr bool MIRROR_Y = false;
constexpr size_t BUFFER_SIZE = WIDTH * HEIGHT / 8;
constexpr uint32_t BUSY_TIMEOUT_MS = 30000;
constexpr uint32_t DEBOUNCE_MS = 40;
constexpr uint32_t REFRESH_GAP_MS = 2000;
const SPISettings EPD_SPI(4000000, MSBFIRST, SPI_MODE0);

uint8_t image[BUFFER_SIZE]; // 1 = белый, 0 = чёрный; 8832 байта.
uint32_t presses = 0;
uint32_t lastRefreshEnd = 0;
uint32_t buttonChangedAt = 0;
int buttonRaw = HIGH;
int buttonStable = HIGH;
bool buttonArmed = false;
bool updatePending = false;
bool displayFault = false;

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
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(value);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

void dataByte(uint8_t value) {
  SPI.beginTransaction(EPD_SPI);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(value);
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

bool waitReady(const char *stage) {
  const uint32_t started = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - started >= BUSY_TIMEOUT_MS) {
      Serial.printf("ERROR: BUSY stayed HIGH at %s for 30 s.\n", stage);
      return false;
    }
    delay(1);
  }
  return true;
}

bool initDisplay() {
  digitalWrite(PIN_RST, LOW);
  delay(10);
  digitalWrite(PIN_RST, HIGH);
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

void writeFrame(uint8_t ramCommand, bool useImage) {
  command(ramCommand);
  SPI.beginTransaction(EPD_SPI);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < BUFFER_SIZE; ++i) {
    SPI.transfer(useImage ? image[i] : 0x00);
  }
  digitalWrite(PIN_CS, HIGH);
  SPI.endTransaction();
}

bool refreshDisplay() {
  const uint32_t started = millis();
  Serial.println("EPD: initialize and send frame");
  if (!initDisplay()) return false;
  writeFrame(0x24, true);
  writeFrame(0x26, false); // Второй RAM plane = 0x00, как у Good Display.
  command(0x22); dataByte(0xF4); // Полное обновление из примера панели.
  command(0x20);

  // BUSY должен подняться. Иначе обрыв BUSY/SPI может выглядеть как успех.
  const uint32_t activation = millis();
  while (digitalRead(PIN_BUSY) == LOW) {
    if (millis() - activation >= 1000) {
      Serial.println("ERROR: no BUSY HIGH after refresh command. Check wiring/power/FPC.");
      return false;
    }
    delay(1);
  }
  Serial.println("EPD: BUSY HIGH - refreshing");
  if (!waitReady("full refresh")) return false;
  Serial.println("EPD: BUSY LOW - controller ready");

  command(0x10); dataByte(0x01); // Deep sleep дисплея, ESP32 продолжает опрос кнопки.
  delay(100);
  lastRefreshEnd = millis();
  Serial.printf("EPD: controller cycle complete, %lu ms; panel asleep. Check image visually.\n",
                static_cast<unsigned long>(millis() - started));
  return true;
}

void stopOnError() {
  displayFault = true;
  digitalWrite(PIN_CS, HIGH);
  digitalWrite(PIN_RST, LOW); // Оставляем контроллер в аппаратном сбросе.
  Serial.println("STOP: check connections with power OFF, then power on again.");
}

void pixel(int x, int y, bool black = true) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
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

void drawTestPage() {
  memset(image, 0xFF, sizeof(image));
  frame(0, 0, WIDTH, HEIGHT);
  centeredText(14, "GDEM035T81", 2);
  centeredText(39, "DISPLAY TEST", 2);
  fillRect(8, 64, WIDTH - 16, 2);
  centeredText(80, "PRESSES", 2);

  char number[11]; // Максимум uint32_t: 10 цифр + завершающий ноль.
  snprintf(number, sizeof(number), "%lu", static_cast<unsigned long>(presses));
  const int numberScale = strlen(number) <= 7 ? 4 : 2;
  centeredText(106, number, numberScale);

  const uint8_t mode = presses % 4;
  char label[16];
  snprintf(label, sizeof(label), "PATTERN %u", static_cast<unsigned>(mode + 1));
  centeredText(154, label, 2);
  frame(8, 179, 168, 152);
  for (int y = 183; y < 327; ++y) {
    for (int x = 12; x < 172; ++x) {
      bool black = false;
      if (mode == 0) black = ((x / 16 + y / 16) & 1) == 0; // Шахматка.
      if (mode == 1) black = ((x / 8) & 1) == 0;          // Вертикальные полосы.
      if (mode == 2) black = ((y / 8) & 1) == 0;          // Горизонтальные полосы.
      if (mode == 3) black = true;                       // Чёрное поле.
      pixel(x, y, black);
    }
  }
  if (mode == 3) fillRect(52, 223, 80, 64, false); // Белое окно на чёрном.
  centeredText(345, "PRESS BUTTON", 2);
  centeredText(369, "GPIO17 TO GND");
}

void syncButton() {
  buttonRaw = buttonStable = digitalRead(PIN_BUTTON);
  buttonChangedAt = millis();
  buttonArmed = buttonStable == HIGH; // Удержание не запускает повторные обновления.
}

void setup() {
  Serial.begin(115200);
  // Ограниченная пауза: дисплей работает и без открытого монитора порта.
  delay(1000);
  Serial.println("\nGDEM035T81 + DESPI-C02 / ESP32-S3 button test");
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUSY, INPUT); // HIGH = занят, LOW = готов.
  digitalWrite(PIN_CS, HIGH); pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_RST, HIGH); pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_DC, HIGH); pinMode(PIN_DC, OUTPUT);
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS); // MISO не нужен.

  Serial.println("Startup 1/3: WHITE");
  memset(image, 0xFF, sizeof(image));
  if (!refreshDisplay()) { stopOnError(); return; }
  delay(REFRESH_GAP_MS);
  Serial.println("Startup 2/3: BLACK");
  memset(image, 0x00, sizeof(image));
  if (!refreshDisplay()) { stopOnError(); return; }
  delay(REFRESH_GAP_MS);
  Serial.println("Startup 3/3: TEST PAGE");
  drawTestPage();
  if (!refreshDisplay()) { stopOnError(); return; }
  syncButton();
  Serial.println("READY: press external button on GPIO17. Refreshes cause flicker.");
}

void loop() {
  if (displayFault) { delay(100); return; }
  const uint32_t now = millis();
  const int raw = digitalRead(PIN_BUTTON);
  if (raw != buttonRaw) {
    buttonRaw = raw;
    buttonChangedAt = now;
  }
  if (raw != buttonStable && now - buttonChangedAt >= DEBOUNCE_MS) {
    buttonStable = raw;
    if (buttonStable == HIGH) {
      buttonArmed = true;
    } else if (buttonArmed) {
      buttonArmed = false;
      ++presses;
      updatePending = true;
      Serial.printf("BUTTON: count = %lu; update queued\n", static_cast<unsigned long>(presses));
    }
  }
  // Нажатие в паузе сохраняется; несколько нажатий могут объединиться в один кадр.
  if (updatePending && now - lastRefreshEnd >= REFRESH_GAP_MS) {
    updatePending = false;
    drawTestPage();
    if (!refreshDisplay()) { stopOnError(); return; }
    syncButton(); // Нажатия во время блокирующего обновления не учитываются.
    Serial.println("READY: release, then press button for next pattern.");
  }
  delay(5);
}
