#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include "audio_format.h"
#include "board_pins.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif


constexpr bool MIC_RIGHT_CHANNEL = false;  // false: L/R -> GND; true: L/R -> 3.3 V.
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t MAX_SECONDS = 30;  // При наличии включённой PSRAM.
constexpr int MIC_GAIN = 4;           // Усиление PCM перед ограничением амплитуды.
constexpr uint32_t DEBOUNCE_MS = 30;
constexpr size_t FRAMES_PER_READ = 128;

i2s_chan_handle_t microphone = nullptr;
uint8_t* wavBuffer = nullptr;
size_t pcmCapacity = 0, pcmBytes = 0;
int32_t inputFrames[FRAMES_PER_READ * 2];
bool ready = false, recording = false, bufferFull = false, pendingAudio = false;
bool waitForRelease = true;  // После включения сначала отпусти кнопку.
bool rawPressed = false, stablePressed = false;
uint32_t rawChangedAt = 0, lastAudioAt = 0, lastWifiRetry = 0;
int previousWifiStatus = -1, peak = 0;
size_t clippedSamples = 0;

void setLed(bool on) {
  if (LED_PIN >= 0) digitalWrite(LED_PIN, on ? HIGH : LOW);
}

bool setupMicrophone() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  channel.dma_desc_num = 8;
  channel.dma_frame_num = FRAMES_PER_READ;
  esp_err_t error = i2s_new_channel(&channel, nullptr, &microphone);
  if (error != ESP_OK) {
    Serial.printf("Ошибка создания I2S: %s\n", esp_err_to_name(error));
    return false;
  }
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  // INMP441: 64 такта SCK на кадр, два слота по 32 бита.
  // Читаем оба слота, в WAV оставляем только канал микрофона.
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = static_cast<gpio_num_t>(MIC_SCK);
  config.gpio_cfg.ws = static_cast<gpio_num_t>(MIC_WS);
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = static_cast<gpio_num_t>(MIC_SD);
  error = i2s_channel_init_std_mode(microphone, &config);
  if (error == ESP_OK) error = i2s_channel_enable(microphone);
  if (error != ESP_OK) {
    Serial.printf("Ошибка настройки I2S: %s\n", esp_err_to_name(error));
    i2s_del_channel(microphone);
    microphone = nullptr;
    return false;
  }
  return true;
}

bool allocateAudio() {
  pcmCapacity = SAMPLE_RATE * 2 * MAX_SECONDS;
  if (psramFound()) {
    wavBuffer = static_cast<uint8_t*>(heap_caps_malloc(
        WAV_HEADER_BYTES + pcmCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (wavBuffer == nullptr) {
    // Без PSRAM пробуем 4, 3, 2 секунды, оставляя RAM для Wi-Fi/HTTP.
    for (int seconds = 4; seconds >= 2; --seconds) {
      const size_t bytes = WAV_HEADER_BYTES + SAMPLE_RATE * 2 * seconds;
      const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (freeInternal < bytes + 80 * 1024) continue;
      wavBuffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      if (wavBuffer != nullptr) {
        pcmCapacity = bytes - WAV_HEADER_BYTES;
        break;
      }
    }
  }
  if (wavBuffer == nullptr) {
    Serial.println("Не хватает памяти для записи. Проверь наличие и настройку PSRAM.");
    return false;
  }
  Serial.printf("Лимит записи: %.1f с; PSRAM: %s\n",
                pcmCapacity / (SAMPLE_RATE * 2.0), psramFound() ? "включена" : "нет/выключена");
  return true;
}

void beginRecording() {
  if (pendingAudio) Serial.println("Предыдущая неотправленная запись заменяется новой.");
  // Убираем кадры до нажатия; тактирование микрофона не прерываем.
  for (int i = 0; i < 16; ++i) {
    size_t discarded = 0;
    i2s_channel_read(microphone, inputFrames, sizeof(inputFrames), &discarded, 0);
    if (discarded == 0) break;
  }
  pcmBytes = 0;
  peak = 0;
  clippedSamples = 0;
  bufferFull = false;
  pendingAudio = false;
  recording = true;
  lastAudioAt = millis();
  setLed(true);
  Serial.println("Запись. Говори и удерживай кнопку; отпусти для отправки.");
}

void sendRecording() {
  if (!pendingAudio) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Ожидаю Wi-Fi до 8 секунд...");
    WiFi.reconnect();
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 8000) delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Нет Wi-Fi. Запись сохранена в RAM: отправь r в Serial для повтора.");
    return;
  }
  makeWavHeader(wavBuffer, static_cast<uint32_t>(pcmBytes), SAMPLE_RATE);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(15000);
  if (!http.begin(client, serverUrl)) {
    Serial.println("Не удалось открыть адрес сервера. Запись сохранена в RAM.");
    return;
  }
  http.addHeader("Content-Type", "audio/wav");
  setLed(true);
  Serial.printf("Отправка WAV: %u байт -> %s\n", static_cast<unsigned>(pcmBytes + WAV_HEADER_BYTES), serverUrl);
  const int code = http.POST(wavBuffer, pcmBytes + WAV_HEADER_BYTES);
  Serial.printf("HTTP: %d\n", code);
  if (code > 0) {
    Serial.println("Ответ: " + http.getString());
    if (code >= 200 && code < 300) {
      pendingAudio = false;
      Serial.println("Сервер принял запись.");
    } else {
      Serial.println("Сервер не принял запись. Проверь обработчик /voice.");
    }
  } else {
    Serial.println("Ошибка отправки: " + http.errorToString(code));
  }
  http.end();
  setLed(false);
  if (pendingAudio) Serial.println("Запись осталась в RAM. r — повтор; новое нажатие — новая запись.");
}

void finishRecording(bool sendAudio) {
  recording = false;
  waitForRelease = true;
  setLed(false);
  Serial.printf("Записано %.2f с, пик: %d/32768, отсчётов с клиппингом: %u\n",
                pcmBytes / (SAMPLE_RATE * 2.0), peak, static_cast<unsigned>(clippedSamples));
  if (pcmBytes < SAMPLE_RATE * 2 / 5) {
    Serial.println("Запись короче 0.2 с — пропущена.");
    return;
  }
  pendingAudio = true;
  if (peak == 0) Serial.println("В записи нули: проверь питание, SD и L/R микрофона.");
  if (sendAudio) sendRecording();
  // Во время HTTP новые нажатия не записываются. Сначала нужна отпущенная кнопка.
}

void pollButton() {
  const bool pressed = digitalRead(BUTTON_PIN) == LOW;
  const uint32_t now = millis();
  if (pressed != rawPressed) {
    rawPressed = pressed;
    rawChangedAt = now;
  }
  if (now - rawChangedAt < DEBOUNCE_MS) return;
  if (!rawPressed && !stablePressed) waitForRelease = false;
  if (rawPressed == stablePressed) return;
  stablePressed = rawPressed;
  if (stablePressed) {
    if (!waitForRelease && ready) beginRecording();
  } else if (recording) {
    finishRecording(true);
  } else {
    waitForRelease = false;
  }
}

void readMicrophone() {
  size_t received = 0;
  const esp_err_t error = i2s_channel_read(microphone, inputFrames, sizeof(inputFrames),
                                         &received, recording ? 20 : 0);
  if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
    Serial.printf("Ошибка чтения I2S: %s\n", esp_err_to_name(error));
    if (recording) finishRecording(false);
    ready = false;
    return;
  }
  if (!recording) return;  // Вне записи кадры только выбрасываем.
  if (received > 0) lastAudioAt = millis();
  if (millis() - lastAudioAt > 1000) {
    Serial.println("I2S не выдаёт данные. Запись остановлена; проверь микрофон.");
    finishRecording(false);
    return;
  }
  if (bufferFull) return;
  const size_t frames = received / (sizeof(int32_t) * 2);
  for (size_t i = 0; i < frames; ++i) {
    if (pcmBytes + 2 > pcmCapacity) {
      bufferFull = true;
      setLed(false);
      Serial.println("Достигнут лимит памяти. Запись остановлена; отпусти кнопку для отправки.");
      break;
    }
    bool clipped = false;
    const int16_t sample = microphoneToPcm16(inputFrames[i * 2 + (MIC_RIGHT_CHANNEL ? 1 : 0)], MIC_GAIN, clipped);
    if (clipped) ++clippedSamples;
    const int level = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
    if (level > peak) peak = level;
    putLe16(wavBuffer + WAV_HEADER_BYTES + pcmBytes, static_cast<uint16_t>(sample));
    pcmBytes += 2;
  }
}

void pollWifi() {
  const int status = WiFi.status();
  digitalWrite(WIFI_LED_PIN, status == WL_CONNECTED ? HIGH : LOW);
  if (status != previousWifiStatus) {
    previousWifiStatus = status;
    if (status == WL_CONNECTED) {
      Serial.print("Wi-Fi подключён. IP ESP32: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("Wi-Fi пока не подключён. Запись доступна, отправка требует сети.");
    }
  }
  if (!recording && status != WL_CONNECTED && millis() - lastWifiRetry >= 10000) {
    lastWifiRetry = millis();
    WiFi.reconnect();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_RST, LOW);
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_DC, LOW);
  pinMode(EPD_DC, OUTPUT);
  digitalWrite(EPD_SCK, LOW);
  pinMode(EPD_SCK, OUTPUT);
  digitalWrite(EPD_MOSI, LOW);
  pinMode(EPD_MOSI, OUTPUT);
  pinMode(EPD_BUSY, INPUT);
  pinMode(BATTERY_ADC, INPUT);
  digitalWrite(WIFI_LED_PIN, LOW);
  pinMode(WIFI_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);
  setLed(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  Serial.println("Voice-planner: INMP441 -> WAV -> HTTP");
  Serial.println("Display reserved; controller held in reset.");
  ready = setupMicrophone() && allocateAudio();
  rawPressed = digitalRead(BUTTON_PIN) == LOW;
  rawChangedAt = millis();
  Serial.println(ready ? "Готово. Отпусти кнопку, затем зажми для записи." : "Запись недоступна: смотри ошибку выше.");
}

void loop() {
  pollButton();
  if (ready) readMicrophone();
  pollWifi();
  while (Serial.available()) {
    const char command = Serial.read();
    if ((command == 'r' || command == 'R') && !recording) {
      sendRecording();
      waitForRelease = true;
    }
  }
  delay(1);
}
