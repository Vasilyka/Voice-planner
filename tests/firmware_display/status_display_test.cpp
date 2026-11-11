#include <algorithm>
#include <cassert>
#include <iostream>
#include "../../firmware/voice_planner/status_display.cpp"

bool testIsData() { return testPins[EPD_DC] == HIGH; }

const std::vector<uint8_t>& dataFor(uint8_t command) {
  for (const auto& item : transfers)
    if (item.command == command) return item.data;
  assert(false && "Expected command missing");
  static const std::vector<uint8_t> empty;
  return empty;
}

void testRotation() {
  memset(image, 0xFF, BUFFER_SIZE);
  pixel(0, 0);
  assert(image[(HEIGHT - 1) * (WIDTH / 8)] == 0x7F);
  pixel(WIDTH - 1, HEIGHT - 1);
  assert(image[WIDTH / 8 - 1] == 0xFE);
  assert(std::count(image, image + BUFFER_SIZE, 0xFF) == BUFFER_SIZE - 2);
}

void testRefreshCommands() {
  memset(image, 0xA5, BUFFER_SIZE);
  memset(previousImage, 0x5A, BUFFER_SIZE);
  transfers.clear();
  assert(refreshDisplay(true));
  assert(dataFor(0x22) == std::vector<uint8_t>{0xF4});
  assert(dataFor(0x24) == std::vector<uint8_t>(BUFFER_SIZE, 0xA5));
  assert(dataFor(0x26) == std::vector<uint8_t>(BUFFER_SIZE, 0));
  assert(dataFor(0x10) == std::vector<uint8_t>{1});

  transfers.clear();
  assert(refreshDisplay(false));
  assert(dataFor(0x22) == std::vector<uint8_t>{0xFF});
  assert(dataFor(0x3C) == std::vector<uint8_t>{1}); // Инициализация до переключения border.
  const auto partialBorder = std::find_if(transfers.begin(), transfers.end(), [](const Transfer& t) {
    return t.command == 0x3C && t.data == std::vector<uint8_t>{0x84};
  });
  assert(partialBorder != transfers.end());
  assert((dataFor(0x21) == std::vector<uint8_t>{0, 0x40}));
  assert(dataFor(0x24) == std::vector<uint8_t>(BUFFER_SIZE, 0xA5));
  assert(dataFor(0x26) == std::vector<uint8_t>(BUFFER_SIZE, 0x5A));
  assert(dataFor(0x10) == std::vector<uint8_t>{1});
}

void testCompletionSurvivesQueueOverwrite() {
  assert(beginStatusDisplay());
  StatusFrame status{};
  showStatus(ScreenState::Ready, "WIFI CONNECTED", 30);
  assert(xQueueReceive(statusQueue, &status, 0));
  assert(chooseRefresh(status, false, 0, 0) == RefreshMode::Full);

  notifyRecordingFinished();
  showStatus(ScreenState::Sending, "PLEASE WAIT", 30);
  assert(xQueueReceive(statusQueue, &status, 0));
  assert(chooseRefresh(status, true, 0, 0) == RefreshMode::Partial);

  showStatus(ScreenState::Saved, "SERVER RECEIVED", 30);
  showStatus(ScreenState::Ready, "WIFI CONNECTED", 30);
  assert(xQueueReceive(statusQueue, &status, 0));
  assert(status.completedRecordings == 1);
  assert(chooseRefresh(status, true, 0, 5) == RefreshMode::Full);
  assert(chooseRefresh(status, true, 1, 5) == RefreshMode::Skip);

  showStatus(ScreenState::Recording, "SPEAK NOW", 30);
  assert(xQueueReceive(statusQueue, &status, 0));
  assert(chooseRefresh(status, true, 0, 0) == RefreshMode::Partial);
  assert(chooseRefresh(status, false, 0, 0) == RefreshMode::Skip);

  notifyRecordingFinished();
  showStatus(ScreenState::Error, "UPLOAD FAILED", 30);
  assert(xQueueReceive(statusQueue, &status, 0));
  assert(chooseRefresh(status, true, 0, 5) == RefreshMode::Full);
  vQueueDelete(statusQueue);
  vSemaphoreDelete(tableMutex);
  statusQueue = nullptr;
}


void testServerBitmapAndProcessing() {
  assert(beginStatusDisplay());
  uint8_t bitmap[BUFFER_SIZE];
  memset(bitmap, 0xFF, sizeof(bitmap));
  bitmap[0] = 0x7F;
  showTaskTable(bitmap, 30);
  StatusFrame status{};
  assert(xQueueReceive(statusQueue, &status, 0));
  assert(status.state == ScreenState::Tasks);
  drawStatus(status);
  assert(image[(HEIGHT - 1) * (WIDTH / 8)] == 0x7F);
  assert(std::count(image, image + BUFFER_SIZE, 0xFF) == BUFFER_SIZE - 1);
  status.state = ScreenState::Processing;
  status.completedRecordings = 10;
  assert(chooseRefresh(status, true, 9, 0) == RefreshMode::Partial);
  status.state = ScreenState::Tasks;
  assert(chooseRefresh(status, true, 9, 5) == RefreshMode::Full);
  vQueueDelete(statusQueue);
  vSemaphoreDelete(tableMutex);
  statusQueue = nullptr;
}

int main() {
  testServerBitmapAndProcessing();
  testRotation();
  testRefreshCommands();
  testCompletionSurvivesQueueOverwrite();
  std::cout << "Rotation, full/partial SPI, queue overwrite and refresh limits: OK\n";
}
