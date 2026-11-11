#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "planner_client.h"
#include "planner_protocol.h"

namespace {
enum class CommandKind { Track, Ack, NextPage };
struct ClientCommand { CommandKind kind; char jobId[33]; };
QueueHandle_t commands = nullptr, events = nullptr;
String baseUrl;
uint32_t page = 0, pages = 1;
char jobId[33] = {};
PlannerEvent incoming = {}; // Не помещаем кадровый буфер на стек задачи.
String revision;

const char* field(cJSON* root, const char* name) {
  cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(value) ? value->valuestring : "";
}

bool getFrame(bool force) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  const char* headers[] = {"X-Frame-Revision", "X-Pages", "X-Page"};
  http.collectHeaders(headers, 3);
  if (!http.begin(client, baseUrl + "/display/frame.bin?page=" + String(page))) return false;
  const int code = http.GET();
  bool ok = code == 200 && http.getSize() == static_cast<int>(PLANNER_FRAME_BYTES);
  if (ok) {
    ok = readPlannerFrame(http.getStreamPtr(), incoming.pixels);
    if (ok) {
      const String nextRevision = http.header("X-Frame-Revision");
      pages = static_cast<uint32_t>(max(1L, http.header("X-Pages").toInt()));
      page = static_cast<uint32_t>(max(0L, http.header("X-Page").toInt()));
      if (force || nextRevision != revision) {
        incoming.kind = PlannerEventKind::Table;
        snprintf(incoming.jobId, sizeof(incoming.jobId), "%s", jobId);
        incoming.detail[0] = 0;
        xQueueOverwrite(events, &incoming);
      }
      revision = nextRevision;
    }
  }
  http.end();
  return ok;
}

void sendStatus(PlannerEventKind kind, const char* message) {
  incoming.kind = kind;
  snprintf(incoming.jobId, sizeof(incoming.jobId), "%s", jobId);
  snprintf(incoming.detail, sizeof(incoming.detail), "%s", message);
  xQueueOverwrite(events, &incoming);
}

void pollJob() {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2500);
  if (!http.begin(client, baseUrl + "/jobs/" + String(jobId) + "?compact=true")) {
    sendStatus(PlannerEventKind::Error, "SERVER URL");
    return;
  }
  const int code = http.GET();
  const String response = code == 200 ? http.getString() : "";
  http.end();
  if (code != 200) { sendStatus(PlannerEventKind::Error, "STATUS OFFLINE"); return; }
  cJSON* json = cJSON_Parse(response.c_str());
  if (!json) { sendStatus(PlannerEventKind::Error, "BAD SERVER JSON"); return; }
  if (strcmp(field(json, "id"), jobId) != 0) {
    cJSON_Delete(json);
    sendStatus(PlannerEventKind::Error, "BAD SERVER JSON");
    return;
  }
  const PlannerJobState state = plannerJobState(field(json, "state"));
  if (state == PlannerJobState::Done) {
    // Повторяем выдачу результата, пока основной цикл не подтвердит его получение.
    if (!getFrame(true)) sendStatus(PlannerEventKind::Error, "TABLE OFFLINE");
  } else if (state == PlannerJobState::Failed || state == PlannerJobState::NeedsConfiguration) {
    Serial.println("Planner: " + String(field(json, "error")));
    sendStatus(PlannerEventKind::Error, state == PlannerJobState::Failed ? "CLOUD FAILED" : "CLOUD NOT SET");
  }
  if (state == PlannerJobState::Queued || state == PlannerJobState::Processing)
    sendStatus(PlannerEventKind::Processing, "RECOGNIZING");
  else if (state == PlannerJobState::Invalid)
    sendStatus(PlannerEventKind::Error, "BAD JOB STATE");
  cJSON_Delete(json);
}

void clientTask(void*) {
  uint32_t lastRequest = millis() - 15000;
  while (true) {
    ClientCommand command;
    bool changed = false;
    while (xQueueReceive(commands, &command, 0) == pdTRUE) {
      if (command.kind == CommandKind::Track) {
        snprintf(jobId, sizeof(jobId), "%s", command.jobId);
        page = 0;
        changed = true;
      } else if (command.kind == CommandKind::Ack) {
        if (strcmp(jobId, command.jobId) == 0) jobId[0] = 0;
      } else {
        page = (page + 1) % pages;
        changed = true;
      }
    }
    const uint32_t interval = jobId[0] ? 2000 : 15000;
    if (WiFi.status() == WL_CONNECTED && (changed || millis() - lastRequest >= interval)) {
      lastRequest = millis();
      if (jobId[0]) pollJob();
      else getFrame(changed);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool sendCommand(CommandKind kind, const char* id) {
  if (!commands) return false;
  ClientCommand command{};
  command.kind = kind;
  snprintf(command.jobId, sizeof(command.jobId), "%s", id);
  return xQueueSend(commands, &command, 0) == pdTRUE;
}
}

bool beginPlannerClient(const char* voiceUrl) {
  baseUrl = voiceUrl;
  if (!baseUrl.endsWith("/voice")) return false;
  baseUrl.remove(baseUrl.length() - 6);
  commands = xQueueCreate(4, sizeof(ClientCommand));
  events = xQueueCreate(1, sizeof(PlannerEvent));
  if (!commands || !events) {
    if (commands) vQueueDelete(commands);
    if (events) vQueueDelete(events);
    commands = events = nullptr;
    return false;
  }
  if (xTaskCreatePinnedToCore(clientTask, "planner-http", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    vQueueDelete(commands); vQueueDelete(events);
    commands = events = nullptr;
    return false;
  }
  return true;
}

bool trackPlannerJob(const char* id) {
  return validPlannerJobId(id) && sendCommand(CommandKind::Track, id);
}
void acknowledgePlannerJob(const char* id) { sendCommand(CommandKind::Ack, id); }
void nextPlannerPage() { sendCommand(CommandKind::NextPage, ""); }
bool receivePlannerEvent(PlannerEvent& event) {
  return events && xQueueReceive(events, &event, 0) == pdTRUE;
}
